idf_component_register(SRCS "rgb_invaders_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_driver_gpio esp_driver_i2c nvs_flash)
