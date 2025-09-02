#include "spotify_web.h"

#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "tcpip_adapter.h"
#include "lwip/ip_addr.h"

#include "cJSON.h"
#include "mbedtls/md.h"
#include "mbedtls/base64.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// ==== Projekt-spezifische Includes aus squeezelite-esp32 ====
#include "nvs_utilities.h"          // config_alloc_get_*, config_set_*, wait_for_commit
#include "tools.h"                  // strlcpy portable, etc.

static const char *TAG = "spotify_web";

/* ---------------------------
 *        NVS-Keys
 * --------------------------- */
#define NVS_KEY_SP_CLIENT_ID   "sp_cid"
#define NVS_KEY_SP_SCOPES      "sp_scopes"
#define NVS_KEY_SP_REFRESH     "sp_rt"
#define NVS_KEY_SP_USER        "sp_user"   // Anzeige-Name (optional)

// Default-Scopes
static const char *SP_DEF_SCOPES =
  "user-modify-playback-state user-read-playback-state user-read-currently-playing";

/* ---------------------------
 *   Volatile Token-Cache
 * --------------------------- */
static char   s_access_token[768] = {0};
static int64_t s_access_exp_ms    = 0;

static inline int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
const char* spotify_get_access_token(void) { return s_access_token[0] ? s_access_token : NULL; }

static void set_access_token_(const char *tok, int64_t exp_ms) {
    if (!tok) { s_access_token[0] = 0; s_access_exp_ms = 0; return; }
    strlcpy(s_access_token, tok, sizeof(s_access_token));
    s_access_exp_ms = exp_ms;
}

/* ---------------------------
 *   Minimal URL-Encoder
 * --------------------------- */
static char *url_encode_(const char *s){
    static const char *hex="0123456789ABCDEF";
    size_t len = strlen(s), out=0;
    for(size_t i=0;i<len;i++){ unsigned c=(unsigned char)s[i];
        if( (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~') out++;
        else out+=3;
    }
    char *o = malloc(out+1), *p=o;
    for(size_t i=0;i<len;i++){ unsigned c=(unsigned char)s[i];
        if( (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~') *p++=c;
        else { *p++='%'; *p++=hex[c>>4]; *p++=hex[c&15]; }
    }
    *p=0; return o;
}

/* ---------------------------
 *   PKCE Helpers
 * --------------------------- */
static char *b64url_(const unsigned char *in, size_t inlen){
    size_t olen=0; mbedtls_base64_encode(NULL,0,&olen,in,inlen);
    unsigned char *buf = malloc(olen+4);
    if(mbedtls_base64_encode(buf,olen+4,&olen,in,inlen)!=0){ free(buf); return NULL; }
    for(size_t i=0;i<olen;i++){ if(buf[i]=='+') buf[i]='-'; else if(buf[i]=='/') buf[i]='_'; }
    while(olen && buf[olen-1]=='=') olen--;
    buf[olen]=0; return (char*)buf;
}

static char *random_verifier_(void){
    uint8_t raw[32]; esp_fill_random(raw, sizeof(raw));
    return b64url_(raw, sizeof(raw));  // 43..44 chars
}

static char *sha256_b64url_(const char *s){
    unsigned char hash[32];
    mbedtls_md_context_t ctx; mbedtls_md_init(&ctx);
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_setup(&ctx, info, 0);
    mbedtls_md_starts(&ctx);
    mbedtls_md_update(&ctx, (const unsigned char*)s, strlen(s));
    mbedtls_md_finish(&ctx, hash);
    mbedtls_md_free(&ctx);
    return b64url_(hash, sizeof(hash));
}

/* ---------------------------
 *   Spotify Token-Flow
 * --------------------------- */
static esp_err_t http_post_xform_(const char *url, const char *body, char **out, int *status){
    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 10000 };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    esp_http_client_set_method(cli, HTTP_METHOD_POST);
    esp_http_client_set_header(cli, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(cli, body, strlen(body));

    esp_err_t err = esp_http_client_perform(cli);
    if (err != ESP_OK){ ESP_LOGE(TAG,"HTTP err %d", err); esp_http_client_cleanup(cli); return err; }

    int st = esp_http_client_get_status_code(cli);
    int len = esp_http_client_get_content_length(cli);
    char *buf = calloc(1, len>0? (len+1):1);
    if (len>0) esp_http_client_read_response(cli, buf, len);
    esp_http_client_cleanup(cli);
    *out = buf; if (status) *status = st;
    return ESP_OK;
}

static esp_err_t token_exchange_code_(const char *code, const char *redirect_uri, const char *client_id, const char *code_verifier){
    char *ru = url_encode_(redirect_uri);
    char *body=NULL;
    asprintf(&body, "grant_type=authorization_code&code=%s&redirect_uri=%s&client_id=%s&code_verifier=%s",
             code, ru, client_id, code_verifier);
    free(ru);

    char *resp=NULL; int st=0;
    esp_err_t err = http_post_xform_("https://accounts.spotify.com/api/token", body, &resp, &st);
    free(body);
    if (err != ESP_OK || st != 200){ ESP_LOGE(TAG,"token exch st=%d body=%s", st, resp?resp:""); free(resp); return ESP_FAIL; }

    cJSON *j = cJSON_Parse(resp);
    const char *access  = cJSON_GetObjectString(j, "access_token");
    const char *refresh = cJSON_GetObjectString(j, "refresh_token");
    int expires = cJSON_GetObjectItem(j,"expires_in") ? cJSON_GetObjectItem(j,"expires_in")->valueint : 3600;

    if (access) set_access_token_(access, now_ms() + ((int64_t)expires - 30) * 1000);

    if (refresh && *refresh) {
        // save refresh in NVS
        config_set_value(NVS_TYPE_STR, NVS_KEY_SP_REFRESH, refresh);
        wait_for_commit();
    }

    // Optional: /v1/me für Anzeige-Namen
    // (Kannst du hinzufügen, wenn du Cover/Name zeigen willst.)

    cJSON_Delete(j); free(resp);
    return ESP_OK;
}

static esp_err_t token_refresh_(void){
    char *cid   = config_alloc_get_str(NVS_KEY_SP_CLIENT_ID, "", NULL);
    char *rt    = config_alloc_get_str(NVS_KEY_SP_REFRESH, "", NULL);
    if (!cid || !*cid || !rt || !*rt){ if (cid) free(cid); if (rt) free(rt); return ESP_ERR_INVALID_STATE; }

    char *body=NULL; asprintf(&body,"grant_type=refresh_token&refresh_token=%s&client_id=%s", rt, cid);
    free(cid); free(rt);

    char *resp=NULL; int st=0;
    esp_err_t err = http_post_xform_("https://accounts.spotify.com/api/token", body, &resp, &st);
    free(body);
    if (err != ESP_OK || st != 200){ ESP_LOGE(TAG,"refresh st=%d body=%s", st, resp?resp:""); free(resp); return ESP_FAIL; }

    cJSON *j = cJSON_Parse(resp);
    const char *access = cJSON_GetObjectString(j, "access_token");
    int expires = cJSON_GetObjectItem(j,"expires_in") ? cJSON_GetObjectItem(j,"expires_in")->valueint : 3600;
    if (access) set_access_token_(access, now_ms() + ((int64_t)expires - 30) * 1000);

    const char *new_rt = cJSON_GetObjectString(j, "refresh_token");
    if (new_rt && *new_rt) { config_set_value(NVS_TYPE_STR, NVS_KEY_SP_REFRESH, new_rt); wait_for_commit(); }

    cJSON_Delete(j); free(resp);
    return ESP_OK;
}

esp_err_t spotify_refresh_if_needed(void){
    if (s_access_token[0] && now_ms() + 15000 < s_access_exp_ms) return ESP_OK;
    return token_refresh_();
}

void spotify_revoke_tokens(void){
    set_access_token_(NULL, 0);
    config_delete_key(NVS_KEY_SP_REFRESH);
    wait_for_commit();
}

/* ---------------------------
 *   HTML (eingebettet)
 * --------------------------- */
static const char *SPOTIFY_HTML = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>Spotify</title>
<style>
body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Ubuntu,Arial,sans-serif;margin:16px;}
section{border:1px solid #ddd;padding:12px;border-radius:12px;margin-bottom:16px;}
h1{margin:0 0 12px 0}
label{display:block;font-size:12px;margin-bottom:4px;color:#444}
input[type=text]{width:100%;padding:8px;border:1px solid #ccc;border-radius:8px}
.row{display:flex;gap:12px;flex-wrap:wrap}
.col{flex:1 1 260px}
button{padding:8px 12px;border:1px solid #444;border-radius:10px;background:#fff;cursor:pointer}
button.primary{background:#111;color:#fff;border-color:#111}
.status{padding:6px 10px;border-radius:8px;background:#f5f5f5;display:inline-block}
small,code{color:#666}
</style></head><body>
<h1>Spotify</h1>
<section>
  <div class="row">
    <div class="col">
      <label for="cid">Client ID</label>
      <input id="cid" type="text" placeholder="Spotify Client ID">
    </div>
    <div class="col">
      <label for="scopes">Scopes</label>
      <input id="scopes" type="text" value="user-modify-playback-state user-read-playback-state user-read-currently-playing">
    </div>
  </div>
  <p><span id="status" class="status">Lade…</span></p>
  <div class="row">
    <button id="save">Speichern</button>
    <button id="auth" class="primary">Mit Spotify verbinden</button>
    <button id="revoke">Abmelden</button>
  </div>
  <p><small>Redirect-URL in deiner Spotify-App eintragen: <code>http://DEVICE_IP/spotify/callback</code></small></p>
</section>
<script>
async function getJ(u){ const r=await fetch(u); return r.json(); }
async function postJ(u,b){ const r=await fetch(u,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)}); return r.json(); }

async function load(){
  const cfg = await getJ('/api/spotify/cfg');
  document.getElementById('cid').value = cfg.client_id||'';
  document.getElementById('scopes').value = cfg.scopes||'';
  document.getElementById('status').textContent = cfg.access_valid ? ('Angemeldet'+(cfg.user?(' als '+cfg.user):'')) : 'Abgemeldet';
}
document.getElementById('save').onclick = async ()=>{
  const res = await postJ('/api/spotify/save', {
    client_id: document.getElementById('cid').value.trim(),
    scopes: document.getElementById('scopes').value.trim()
  });
  if(!res.ok) alert('Fehler: '+(res.err||''));
  else alert('Gespeichert');
};
document.getElementById('auth').onclick = ()=>{ location.href='/spotify/authorize'; };
document.getElementById('revoke').onclick = async ()=>{ await fetch('/spotify/revoke',{method:'POST'}); location.reload(); };
load();
</script>
</body></html>
)HTML";

/* ---------------------------
 *   HTTP Utils
 * --------------------------- */
static esp_err_t read_body_json_(httpd_req_t *r, cJSON **out){
    int len = r->content_len;
    if (len <= 0 || len > 2048) return ESP_FAIL;
    char *buf = malloc(len+1); int off=0;
    while (off < len) {
        int cur = httpd_req_recv(r, buf+off, len-off);
        if (cur <= 0) { free(buf); return ESP_FAIL; }
        off += cur;
    }
    buf[len]=0;
    *out = cJSON_Parse(buf); free(buf);
    return *out ? ESP_OK : ESP_FAIL;
}

static bool build_redirect_uri(char *out, size_t n) {
    tcpip_adapter_ip_info_t ip;

    // Bevorzugt STA-IP
    if (tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip) == ESP_OK && ip.ip.addr != 0) {
        snprintf(out, n, "http://" IPSTR "/spotify/callback", IP2STR(&ip.ip));
        return true;
    }
    // Fallback: AP-IP
    if (tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_AP, &ip) == ESP_OK && ip.ip.addr != 0) {
        snprintf(out, n, "http://" IPSTR "/spotify/callback", IP2STR(&ip.ip));
        return true;
    }
    return false;
}



/* ---------------------------
 *   Routen-Handler
 * --------------------------- */
static esp_err_t h_page_(httpd_req_t *r){
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    return httpd_resp_send(r, SPOTIFY_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_cfg_(httpd_req_t *r){
    char *cid = config_alloc_get_str(NVS_KEY_SP_CLIENT_ID, "", NULL);
    char *sc  = config_alloc_get_str(NVS_KEY_SP_SCOPES, SP_DEF_SCOPES, NULL);
    char *usr = config_alloc_get_str(NVS_KEY_SP_USER, "", NULL);

    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "client_id", cid?cid:"");
    cJSON_AddStringToObject(j, "scopes", sc?sc:"");
    cJSON_AddBoolToObject  (j, "access_valid", spotify_get_access_token()!=NULL);
    cJSON_AddStringToObject(j, "user", usr?usr:"");

    char *out = cJSON_PrintUnformatted(j);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_send(r, out, HTTPD_RESP_USE_STRLEN);

    if (cid) free(cid); if (sc) free(sc); if (usr) free(usr);
    cJSON_free(out); cJSON_Delete(j);
    return ESP_OK;
}

static esp_err_t h_save_(httpd_req_t *r){
    cJSON *j=NULL; if (read_body_json_(r,&j)!=ESP_OK){ httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"json"); return ESP_OK; }
    const char *cid = cJSON_GetObjectString(j,"client_id");
    const char *sc  = cJSON_GetObjectString(j,"scopes");
    if (cid) config_set_value(NVS_TYPE_STR, NVS_KEY_SP_CLIENT_ID, cid);
    if (sc && *sc) config_set_value(NVS_TYPE_STR, NVS_KEY_SP_SCOPES, sc);
    wait_for_commit();
    cJSON_Delete(j);
    httpd_resp_set_type(r,"application/json");
    httpd_resp_sendstr(r,"{\"ok\":true}");
    return ESP_OK;
}

/* Redirect zu Spotify; legt Code-Verifier in HttpOnly-Cookie ab */
static esp_err_t h_authorize_(httpd_req_t *r){
    char *cid = config_alloc_get_str(NVS_KEY_SP_CLIENT_ID, "", NULL);
    if (!cid || !*cid){ if (cid) free(cid); httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"client id fehlt"); return ESP_OK; }

    // IP bestimmen (AP oder STA)
    tcpip_adapter_ip_info_t ipInfo; network_get_ip_info(&ipInfo);

   char redirect[64];
   if (!build_redirect_uri(redirect, sizeof(redirect))) {
       httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "no ip");
       return ESP_OK;
   }

    char *verifier  = random_verifier_();
    char *challenge = sha256_b64url_(verifier);

    char cookie[512]; snprintf(cookie, sizeof(cookie), "sp_code_verifier=%s; Path=/; Max-Age=300; HttpOnly", verifier);
    httpd_resp_set_hdr(r, "Set-Cookie", cookie);

    char *enc_redirect = url_encode_(redirect);
    char *scopes = config_alloc_get_str(NVS_KEY_SP_SCOPES, (char*)SP_DEF_SCOPES, NULL);
    char *enc_scopes   = url_encode_(scopes && *scopes ? scopes : SP_DEF_SCOPES);

    char *url=NULL; asprintf(&url,
      "https://accounts.spotify.com/authorize?response_type=code&client_id=%s&redirect_uri=%s&code_challenge_method=S256&code_challenge=%s&scope=%s&state=esp32",
      cid, enc_redirect, challenge, enc_scopes);

    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", url);
    httpd_resp_sendstr(r, "");

    free(cid); free(verifier); free(challenge); free(enc_redirect); if (scopes) free(scopes); free(enc_scopes); free(url);
    return ESP_OK;
}

/* Cookie-Parser (einfach) */
static char* cookie_get_(const char *cookie_hdr, const char *key){
    if (!cookie_hdr||!key) return NULL;
    const char *p = strstr(cookie_hdr, key);
    if (!p) return NULL;
    p += strlen(key);
    if (*p!='=') return NULL; p++;
    const char *e = strchr(p,';'); size_t n = e? (size_t)(e-p) : strlen(p);
    char *out = malloc(n+1); memcpy(out,p,n); out[n]=0; return out;
}

static esp_err_t h_callback_(httpd_req_t *r){
    // Query: ?code=...
    int qlen = httpd_req_get_url_query_len(r) + 1;
    if (qlen <= 1 || qlen > 256) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "qs");
    char qs[256]; httpd_req_get_url_query_str(r, qs, sizeof(qs));
    char code[160]; if (httpd_query_key_value(qs,"code",code,sizeof(code))!=ESP_OK) { httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"no code"); return ESP_OK; }

    // redirect rekonstruieren
    tcpip_adapter_ip_info_t ipInfo; network_get_ip_info(&ipInfo);
    char redirect[64]; snprintf(redirect, sizeof(redirect), "http://" IPSTR "/spotify/callback", IP2STR(&ipInfo.ip));

    // Cookie -> code_verifier
    size_t cl=0; httpd_req_get_hdr_value_len(r,"Cookie",&cl);
    char *cookie=NULL; if (cl){ cookie = malloc(cl+1); httpd_req_get_hdr_value_str(r,"Cookie",cookie,cl+1); }
    char *verifier = cookie_get_(cookie, "sp_code_verifier"); if (cookie) free(cookie);
    if (!verifier){ httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"no verifier"); return ESP_OK; }

    // Client ID
    char *cid = config_alloc_get_str(NVS_KEY_SP_CLIENT_ID, "", NULL);
    if (!cid || !*cid){ free(verifier); if (cid) free(cid); httpd_resp_send_err(r,HTTPD_400_BAD_REQUEST,"client id"); return ESP_OK; }

    if (token_exchange_code_(code, redirect, cid, verifier) == ESP_OK) {
        // Zurück zur UI
        httpd_resp_set_status(r,"302 Found");
        httpd_resp_set_hdr(r,"Location","/spotify");
        httpd_resp_sendstr(r,"");
    } else {
        httpd_resp_send_err(r,HTTPD_500_INTERNAL_SERVER_ERROR,"token");
    }
    free(verifier); free(cid);
    return ESP_OK;
}

static esp_err_t h_revoke_(httpd_req_t *r){
    spotify_revoke_tokens();
    httpd_resp_sendstr(r,"ok");
    return ESP_OK;
}

/* ---------------------------
 *   Registrierung
 * --------------------------- */
void spotify_web_register(httpd_handle_t server){
    httpd_uri_t u1 = {.uri="/spotify",             .method=HTTP_GET,  .handler=h_page_};
    httpd_uri_t u2 = {.uri="/api/spotify/cfg",     .method=HTTP_GET,  .handler=h_cfg_};
    httpd_uri_t u3 = {.uri="/api/spotify/save",    .method=HTTP_POST, .handler=h_save_};
    httpd_uri_t u4 = {.uri="/spotify/authorize",   .method=HTTP_GET,  .handler=h_authorize_};
    httpd_uri_t u5 = {.uri="/spotify/callback",    .method=HTTP_GET,  .handler=h_callback_};
    httpd_uri_t u6 = {.uri="/spotify/revoke",      .method=HTTP_POST, .handler=h_revoke_};

    httpd_register_uri_handler(server, &u1);
    httpd_register_uri_handler(server, &u2);
    httpd_register_uri_handler(server, &u3);
    httpd_register_uri_handler(server, &u4);
    httpd_register_uri_handler(server, &u5);
    httpd_register_uri_handler(server, &u6);

    ESP_LOGI(TAG, "Spotify web routes mounted");
}
