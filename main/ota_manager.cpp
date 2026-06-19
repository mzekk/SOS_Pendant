#include "ota_manager.hpp"
#include "shared_state.hpp"
#include "M5Unified.h"
#include "esp_log.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "OTA_MANAGER";

void ota_mark_success() {
    // Mark the current firmware app as valid to prevent rollback on next reboot
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "Firmware marked as VALID. Rollback cancelled.");
}

void ota_force_rollback() {
    ESP_LOGW(TAG, "Manual rollback requested! Swapping boot partition...");
    const esp_partition_t* update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition != NULL) {
        esp_ota_set_boot_partition(update_partition);
        esp_restart();
    } else {
        ESP_LOGE(TAG, "No valid rollback partition found!");
    }
}

static void ota_task(void *pvParameter) {
    const char* url = (const char*)pvParameter;
    ESP_LOGI(TAG, "Starting OTA update from: %s", url);

    esp_http_client_config_t config = {};
    config.url = url;
    config.crt_bundle_attach = esp_crt_bundle_attach; // Validate SSL certs!
    config.keep_alive_enable = true;
    config.max_redirection_count = 5; // Required to follow GitHub Release redirects!
    config.buffer_size = 4096; // CRUCIAL: GitHub S3 redirect URLs are huge!
    config.buffer_size_tx = 4096; // CRUCIAL: Must also increase TX buffer to send the massive redirect URL request!
    
    esp_https_ota_config_t ota_config = {};
    ota_config.http_config = &config;

    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA Update successful! Rebooting...");
        currentState = STATE_OTA_SUCCESS;
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA Update failed: %s", esp_err_to_name(ret));
        ota_error_str = esp_err_to_name(ret);
        currentState = STATE_OTA_FAILED;
        vTaskDelay(pdMS_TO_TICKS(5000));
        // The system will revert state and try again on the next polling cycle
        currentState = STATE_IDLE; 
    }
    vTaskDelete(NULL);
}

void start_ota_update(const char* url) {
    xTaskCreate(ota_task, "ota_task", 8192, (void*)url, 5, NULL);
}