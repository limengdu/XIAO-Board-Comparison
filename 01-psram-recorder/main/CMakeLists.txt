idf_component_register(SRCS "recorder_main.c" "recorder.c"
                       PRIV_REQUIRES console esp_timer esp_rom esp_hw_support heap
                       INCLUDE_DIRS ".")
