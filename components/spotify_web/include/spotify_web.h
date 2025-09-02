#pragma once
#include "esp_http_server.h"
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Registriert alle Spotify-Webrouten (UI + API). */
void spotify_web_register(httpd_handle_t server);

/** Stellt sicher, dass ein gültiges Access-Token vorhanden ist (refresh bei Bedarf). */
esp_err_t spotify_refresh_if_needed(void);

/** Liefert das aktuelle Access-Token (oder NULL/leer). Gültig bis zum nächsten Ablauf/Refresh. */
const char* spotify_get_access_token(void);

/** Optional: Force-Revoke (löscht Tokens aus NVS und RAM). */
void spotify_revoke_tokens(void);

#ifdef __cplusplus
}
#endif
