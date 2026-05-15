idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        ascon
        lora_e32
        esp_driver_gpio
        esp_driver_uart
        esp_timer
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
)
