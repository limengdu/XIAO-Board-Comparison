idf_component_register(SRCS "canfd_main.c" "bus_test.c"
                       PRIV_REQUIRES console esp_driver_twai esp_timer esp_rom esp_hw_support
                       INCLUDE_DIRS ".")
