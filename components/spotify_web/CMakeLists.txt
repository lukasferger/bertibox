idf_component_register(
  SRCS "spotify_web.c"
  INCLUDE_DIRS "include"
  REQUIRES esp_http_server esp_http_client esp_timer esp_netif nvs_flash json mbedtls
)
