idf_component_register(SRCS "cpu_main.c" "render.c"
                       PRIV_REQUIRES console esp_timer esp_rom esp_hw_support
                       INCLUDE_DIRS ".")
