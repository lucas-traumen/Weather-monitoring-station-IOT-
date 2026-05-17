idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "supabase_root_ca.pem"
    REQUIRES
        ascon
        lora_e32
        esp_driver_uart
        esp_driver_gpio
        esp_timer
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_http_client
        esp-tls
        app_update
        bootloader_support
        esp_https_ota
)
