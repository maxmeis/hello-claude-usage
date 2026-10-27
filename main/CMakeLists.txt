if(CONFIG_CLAUDE_DISPLAY_DIAGNOSTIC)
    idf_component_register(
        SRCS "display_diagnostic_main.c" "display_diagnostic.c"
        INCLUDE_DIRS "."
        REQUIRES bsp esp_mm)
else()
idf_component_register(
    SRCS "app_main.c" "device_identity.c" "device_discovery.c" "factory_reset.c" "wifi_manager.c" "wifi_store.c" "oauth_probe.c" "probe_protocol.c" "refresh_control.c" "display_format.c" "word_clock.c" "display.c" "lv_port.c"
         "usage_heading_font.c" "usage_values_font.c" "usage_ui_font.c" "usage_status_font.c" "usage_meta_font.c"
    INCLUDE_DIRS "."
    REQUIRES bsp lvgl esp_wifi esp_event esp_netif nvs_flash esp_http_client
             esp_http_server mbedtls json esp_timer lwip esp_system esp_mm mdns)
endif()
