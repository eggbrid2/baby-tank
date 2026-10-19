idf_component_register(
    SRCS "baby_tank_ota.c"
    INCLUDE_DIRS "include"
    REQUIRES app_update esp_https_ota esp_http_client mbedtls
)
