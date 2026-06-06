# Main component. Globs every source in src/ and pulls in the IDF
# components used by the firmware.
file(GLOB_RECURSE APP_SRCS "*.cpp" "*.c")

idf_component_register(
    SRCS ${APP_SRCS}
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_adc
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        mqtt
        esp_http_server
        app_update
        mbedtls
        esp_timer
        esp_hw_support
)
