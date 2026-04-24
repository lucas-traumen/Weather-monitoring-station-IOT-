idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES ascon lora_e32 esp_driver_uart esp_driver_gpio esp_timer
)