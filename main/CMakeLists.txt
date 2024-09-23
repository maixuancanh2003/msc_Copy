idf_component_register(SRCS "main.c" 
                      INCLUDE_DIRS "." "../components/tinyusb/src"
                      PRIV_REQUIRES freertos esp_wifi esp_http_server nvs_flash fatfs esp_netif usb fatfs esp_driver_gpio esp_timer led_strip driver)
