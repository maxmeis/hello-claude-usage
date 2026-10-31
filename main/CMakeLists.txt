if(CONFIG_CLAUDE_DISPLAY_DIAGNOSTIC)
    idf_component_register(
        SRCS "display_diagnostic_main.c" "display_diagnostic.c"
        INCLUDE_DIRS "."
        REQUIRES bsp esp_mm)
else()
idf_component_register(
    SRCS "app_main.c" "app_registry.c" "screensaver_registry.c" "ui_shell.c" "usb_management.c" "usage_provider.c" "provider_http.c" "codex_protocol.c" "codex_adapter.c" "box_config.c" "box_config_store.c" "device_identity.c" "device_discovery.c" "factory_reset.c" "wifi_manager.c" "wifi_tx_policy.c" "wifi_failure.c" "wifi_store.c" "oauth_probe.c" "probe_protocol.c" "refresh_control.c" "display_format.c" "word_clock.c" "display.c" "usage_app_ui.c" "lv_port.c"
         "usage_heading_font.c" "usage_hero_font.c" "usage_values_font.c" "usage_ui_font.c" "usage_status_font.c" "usage_meta_font.c"
    INCLUDE_DIRS "."
    REQUIRES bsp lvgl esp_wifi esp_event esp_netif nvs_flash esp_http_client
             esp_http_server esp_app_format mbedtls json esp_timer lwip esp_system esp_mm mdns)
if(CONFIG_HELLO_BOX_USB_TEST_HELPER)
    target_sources(${COMPONENT_LIB} PRIVATE "wifi_usb_test.c")
endif()
endif()
