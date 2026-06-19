idf_component_register(SRCS "app_main.cpp" "network_manager.cpp" "ota_manager.cpp"
                    PRIV_REQUIRES spi_flash M5Unified nvs_flash esp_wifi esp_netif esp_event esp-tls mbedtls wifi_provisioning esp_http_client json esp_https_ota app_update
                    INCLUDE_DIRS ""
                    EMBED_FILES "Audio/Fall.wav" "Audio/Help.wav" "Audio/Reminder.wav")
