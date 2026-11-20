idf_component_register(SRCS "power_main.c" "power_test.c"
                       PRIV_REQUIRES console esp_wifi esp_netif esp_event nvs_flash lwip esp_pm esp_timer
                                     esp_hw_support esp_driver_usb_serial_jtag
                       INCLUDE_DIRS ".")
