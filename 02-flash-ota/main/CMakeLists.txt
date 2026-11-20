idf_component_register(SRCS "app_main.c" "app_info.c" "net.c" "storage.c" "web.c"
                       PRIV_REQUIRES console nvs_flash esp_wifi esp_netif esp_event esp_http_server
                                     app_update bootloader_support esp_partition spi_flash fatfs
                                     wear_levelling esp_app_format
                       INCLUDE_DIRS ".")
