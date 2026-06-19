#include "network_manager.hpp"
#include "config.hpp"
#include "shared_state.hpp"
#include "ota_manager.hpp"
#include "M5Unified.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_tls.h"
#include "mbedtls/base64.h"
#include "esp_crt_bundle.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_softap.h"
#include "esp_http_client.h"
#include "esp_sntp.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include <string.h>

static const char *TAG = "NETWORK";

static char http_response_buf[2048] = {0};
static int http_response_len = 0;

bool wifi_is_paused = false;

static bool firmware_validated = false;

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "Wi-Fi started, attempting to connect...");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected = false;
        if (!wifi_is_paused) {
            esp_wifi_connect(); // Auto-reconnect ONLY when not intentionally paused!
            ESP_LOGW(TAG, "Wi-Fi disconnected. Retrying...");
        } else {
            ESP_LOGI(TAG, "Wi-Fi disconnected gracefully for sleep mode.");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP Address: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_connected = true;
    }
}

static void wifi_prov_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {
            case WIFI_PROV_CRED_FAIL:
                ESP_LOGW(TAG, "Provisioning failed! Retrying...");
                wifi_prov_mgr_reset_sm_state_on_failure();
                break;
            case WIFI_PROV_CRED_SUCCESS:
                ESP_LOGI(TAG, "Provisioning successful");
                currentState = STATE_IDLE;
                M5.Lcd.fillScreen(TFT_BLACK); 
                break;
            default: break;
        }
    }
}

void load_system_parameters() {
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
        // Clean up old legacy parameter blocks to permanently free up NVS space
        nvs_erase_key(my_handle, "sys_params");
        nvs_erase_key(my_handle, "sys_cfg_v2");

        size_t required_size = sizeof(SystemParameters);
        SystemParameters temp_params;
        esp_err_t err = nvs_get_blob(my_handle, "sys_cfg_v3", &temp_params, &required_size);
        if (err == ESP_OK && required_size == sizeof(SystemParameters)) {
            sys_params = temp_params;
            ESP_LOGI(TAG, "System parameters successfully loaded from NVS.");
        } else {
            ESP_LOGI(TAG, "NVS Params empty/mismatched. Formatting with Defaults.");
            nvs_set_blob(my_handle, "sys_cfg_v3", &sys_params, sizeof(SystemParameters));
            nvs_commit(my_handle);
        }
        nvs_close(my_handle);
    }
}

void network_init() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &wifi_prov_event_handler, NULL, NULL);

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wifi_init_cfg);

    wifi_prov_mgr_config_t prov_config = {};
    prov_config.scheme = wifi_prov_scheme_softap;
    prov_config.scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE;
    wifi_prov_mgr_init(prov_config);

    bool provisioned = false;
    wifi_prov_mgr_is_provisioned(&provisioned);

    if (!provisioned) {
        ESP_LOGI(TAG, "Starting Wi-Fi provisioning (SoftAP)...");
        currentState = STATE_PROVISIONING;
        wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, "1234567", "PROV_M5STICK", NULL);
    } else {
        ESP_LOGI(TAG, "Already provisioned, starting normal Wi-Fi...");
        currentState = STATE_IDLE;
        wifi_prov_mgr_deinit();
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
        esp_wifi_set_ps(WIFI_PS_MAX_MODEM); // Enable aggressive Wi-Fi power saving
    }

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    setenv("TZ", TIMEZONE, 1);
    tzset();
    
    load_system_parameters();
}

void network_reset_provisioning() {
    wifi_prov_mgr_config_t prov_config = {};
    prov_config.scheme = wifi_prov_scheme_softap;
    prov_config.scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE;
    wifi_prov_mgr_init(prov_config);
    wifi_prov_mgr_reset_provisioning();
    esp_wifi_restore();
}

void wifi_pause() {
    if (!wifi_is_paused && currentState != STATE_PROVISIONING) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        wifi_connected = false;
        wifi_is_paused = true;
        ESP_LOGI(TAG, "Wi-Fi RF Radio Paused (Power Saving)");
    }
}

void wifi_resume() {
    if (wifi_is_paused && currentState != STATE_PROVISIONING) {
        esp_wifi_start();
        wifi_is_paused = false;
    }
}

static void send_email_task(void *pvParameters) {
    const char* subject = (const char*)pvParameters;

    int retries = 150; // Wait up to 15 seconds for a cold router reconnect
    while (!wifi_connected && retries-- > 0) vTaskDelay(pdMS_TO_TICKS(100));
    if (!wifi_connected) { 
        if ((currentState == STATE_IDLE || currentState == STATE_LOW_BATTERY) && is_screen_off) wifi_pause();
        vTaskDelete(NULL); return; 
    }

    ESP_LOGI(TAG, "Starting SMTP connection to Google...");
    
    esp_tls_cfg_t cfg = {};
    cfg.crt_bundle_attach = esp_crt_bundle_attach; 
    cfg.timeout_ms = 5000;
    
    esp_tls_t *tls = esp_tls_init();
    if (esp_tls_conn_new_sync("smtp.gmail.com", strlen("smtp.gmail.com"), 465, &cfg, tls) <= 0) {
        ESP_LOGE(TAG, "TLS Connection failed!");
        esp_tls_conn_destroy(tls);
        vTaskDelete(NULL);
        return;
    }

    char rx_buf[512];
    auto send_cmd = [&](const char* cmd) {
        if (cmd) esp_tls_conn_write(tls, cmd, strlen(cmd));
        vTaskDelay(pdMS_TO_TICKS(500)); 
        int ret = esp_tls_conn_read(tls, rx_buf, sizeof(rx_buf)-1);
        if(ret > 0) { rx_buf[ret] = 0; ESP_LOGI(TAG, "SMTP RX: %s", rx_buf); }
    };

    send_cmd(NULL); 
    send_cmd("EHLO esp32s3\r\n");
    send_cmd("AUTH LOGIN\r\n");
    
    unsigned char b64_user[128]; size_t len;
    mbedtls_base64_encode(b64_user, sizeof(b64_user), &len, (const unsigned char*)SENDER_EMAIL, strlen(SENDER_EMAIL));
    b64_user[len] = '\0'; 
    char user_cmd[130]; snprintf(user_cmd, sizeof(user_cmd), "%s\r\n", b64_user);
    send_cmd(user_cmd); 
    
    unsigned char b64_pass[128];
    mbedtls_base64_encode(b64_pass, sizeof(b64_pass), &len, (const unsigned char*)SENDER_PASS, strlen(SENDER_PASS));
    b64_pass[len] = '\0'; 
    char pass_cmd[130]; snprintf(pass_cmd, sizeof(pass_cmd), "%s\r\n", b64_pass);
    send_cmd(pass_cmd); 
    
    char mail_from[128]; snprintf(mail_from, sizeof(mail_from), "MAIL FROM:<%s>\r\n", SENDER_EMAIL);
    send_cmd(mail_from);
    char rcpt_to[128]; snprintf(rcpt_to, sizeof(rcpt_to), "RCPT TO:<%s>\r\n", RECIPIENT_EMAIL);
    send_cmd(rcpt_to);
    char rcpt_to2[128]; snprintf(rcpt_to2, sizeof(rcpt_to2), "RCPT TO:<%s>\r\n", RECIPIENT_EMAIL_2);
    send_cmd(rcpt_to2);
    
    send_cmd("DATA\r\n");
    char data_body[512];
    snprintf(data_body, sizeof(data_body), "Subject: %s\r\nTo: %s, %s\r\n\r\nEmergency System Alert!\r\nBattery Level: %ld%%\r\n.\r\n", 
             subject, RECIPIENT_EMAIL, RECIPIENT_EMAIL_2, (long)M5.Power.getBatteryLevel());
    send_cmd(data_body);
    
    send_cmd("QUIT\r\n");
    esp_tls_conn_destroy(tls);
    ESP_LOGI(TAG, "Email Dispatched!");
    vTaskDelete(NULL);
}

void send_emergency_email(const char* subject) {
    xTaskCreate(send_email_task, "email_task", 8192, (void*)subject, 5, NULL);
}

static void send_pushover_task(void *pvParameters) {
    const char* message = (const char*)pvParameters;
    
    int retries = 150; // Wait up to 15 seconds for a cold router reconnect
    while (!wifi_connected && retries-- > 0) vTaskDelay(pdMS_TO_TICKS(100));
    if (!wifi_connected) { 
        if ((currentState == STATE_IDLE || currentState == STATE_LOW_BATTERY) && is_screen_off) wifi_pause();
        vTaskDelete(NULL); return; 
    }

    // Priority 2 requires retry (seconds) and expire (seconds) parameters
    char post_data[512];
    snprintf(post_data, sizeof(post_data),
             "token=%s&user=%s&title=SOS+Pendant+Alert&message=%s&priority=2&retry=30&expire=3600&sound=EmergencyAlarm",
             PUSHOVER_APP_TOKEN, PUSHOVER_USER_KEY, message);

    esp_http_client_config_t config = {};
    config.url = "https://api.pushover.net/1/messages.json";
    config.method = HTTP_METHOD_POST;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    ESP_LOGI(TAG, "Sending High-Priority Pushover Alert...");
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) ESP_LOGI(TAG, "Pushover Alert sent! Status: %d", esp_http_client_get_status_code(client));
    else ESP_LOGE(TAG, "Pushover failed: %s", esp_err_to_name(err));
    
    esp_http_client_cleanup(client);
    vTaskDelete(NULL);
}

void send_pushover_alert(const char* message) {
    xTaskCreate(send_pushover_task, "pushover_task", 8192, (void*)message, 5, NULL);
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    switch(evt->event_id) {
        case HTTP_EVENT_REDIRECT:
            http_response_len = 0; memset(http_response_buf, 0, sizeof(http_response_buf)); break;
        case HTTP_EVENT_ON_DATA:
            if (esp_http_client_get_status_code(evt->client) == 200 && (http_response_len + evt->data_len < sizeof(http_response_buf))) {
                memcpy(http_response_buf + http_response_len, evt->data, evt->data_len);
                http_response_len += evt->data_len; http_response_buf[http_response_len] = '\0';
            }
            break;
        default: break;
    }
    return ESP_OK;
}

void parse_and_save_parameters(cJSON *params) {
    bool updated = false;

    auto get_int = [&](const char* key, uint32_t &val) {
        cJSON *item = cJSON_GetObjectItem(params, key);
        if (item && cJSON_IsNumber(item) && val != (uint32_t)item->valueint) { val = item->valueint; updated = true; }
    };
    auto get_float = [&](const char* key, float &val) {
        cJSON *item = cJSON_GetObjectItem(params, key);
        if (item && cJSON_IsNumber(item) && val != (float)item->valuedouble) { val = (float)item->valuedouble; updated = true; }
    };
    auto get_bool = [&](const char* key, bool &val) {
        cJSON *item = cJSON_GetObjectItem(params, key);
        if (item) {
            bool new_val = val;
            if (cJSON_IsBool(item)) new_val = cJSON_IsTrue(item);
            else if (cJSON_IsNumber(item)) new_val = (item->valueint > 0);
            if (val != new_val) { val = new_val; updated = true; }
        }
    };
    auto get_string = [&](const char* key, char* dest, size_t max_len) {
        cJSON *item = cJSON_GetObjectItem(params, key);
        if (item && cJSON_IsString(item) && strcmp(dest, item->valuestring) != 0) {
            strncpy(dest, item->valuestring, max_len - 1); dest[max_len - 1] = '\0'; updated = true;
        }
    };

    get_int("TEMPO_PREM_SOS_MS", sys_params.tempo_prem_sos_ms);
    get_int("TIMEOUT_PRE_ALLARME_MS", sys_params.timeout_pre_allarme_ms);
    get_int("TIMEOUT_PROMEMORIA_MS", sys_params.timeout_promemoria_ms);
    get_int("INT_SINCRONIZZAZIONE_MS", sys_params.int_sincronizzazione_ms);
    get_float("SOGLIA_CADUTA_G", sys_params.soglia_caduta_g);
    get_float("SOGLIA_VUOTO_G", sys_params.soglia_vuoto_g);
    get_int("DURATA_BATT_MINUTI", sys_params.durata_batt_min);
    get_int("BATT_SCARICA_PCT", sys_params.batt_scarica_pct);
    get_int("BATT_RIPRESA_PCT", sys_params.batt_ripresa_pct);
    get_int("BATT_DEBOLE_PCT", sys_params.batt_debole_pct);
    get_int("DURATA_LOWPWR_STATO", sys_params.durata_lowpwr_stato);
    get_int("SPEGNIMENTO_SCHERMO_MS", sys_params.spegnimento_schermo_ms);
    get_int("SPEGNIMENTO_SCHERMO_BATT_MS", sys_params.spegnimento_schermo_batt_ms);
    get_bool("ABILITA_CADUTA", sys_params.abilita_caduta);
    get_bool("ABILITA_PROMEMORIA", sys_params.abilita_promemoria);
    get_bool("ABILITA_RISVEGLIO_MOV", sys_params.abilita_risveglio_mov);
    get_bool("ABILITA_BEEP_UI", sys_params.abilita_beep_ui);
    get_int("TIMEOUT_WIFI_PROV_MS", sys_params.timeout_wifi_prov_ms);
    get_int("TEMPO_PREM_PROV_MS", sys_params.tempo_prem_prov_ms);
    get_bool("ABILITA_VEGLIA_SONNO", sys_params.abilita_veglia_sonno);
    get_float("BASE_X_G", sys_params.base_x_g);
    get_float("BASE_Y_G", sys_params.base_y_g);
    get_float("BASE_TOLLERANZA_G", sys_params.base_tolleranza_g);
    get_string("ORA_INIZIO_VEGLIA", sys_params.ora_inizio_veglia, 6);
    get_string("ORA_FINE_VEGLIA", sys_params.ora_fine_veglia, 6);
    get_string("ORA_INIZIO_SONNO", sys_params.ora_inizio_sonno, 6);
    get_string("ORA_FINE_SONNO", sys_params.ora_fine_sonno, 6);
    get_int("TIMEOUT_IMMOBILITA_MIN", sys_params.timeout_immobilita_min);
    get_float("SOGLIA_MOV_SONNO_G", sys_params.soglia_mov_sonno_g);
    get_int("MAX_MOV_SONNO", sys_params.max_mov_sonno);
    get_int("DISPLAY_NORM_LUM", sys_params.display_norm_lum);
    get_int("DISPLAY_DIM_LUM", sys_params.display_dim_lum);

    // Parse OTA URL silently without triggering an NVS commit
    cJSON *ota_item = cJSON_GetObjectItem(params, "ota_url");
    if (ota_item && cJSON_IsString(ota_item) && strlen(ota_item->valuestring) > 10) {
        strncpy(ota_url_target, ota_item->valuestring, sizeof(ota_url_target)-1);
        ota_available = true;
    }

    if (updated) {
        nvs_handle_t my_handle;
        if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
            nvs_set_blob(my_handle, "sys_cfg_v3", &sys_params, sizeof(SystemParameters));
            nvs_commit(my_handle);
            nvs_close(my_handle);
            ESP_LOGI(TAG, "Parameters updated remotely & saved to NVS!");
        }
    }
}

static void log_custom_event_task(void *pvParameters) {
    const char* custom_state = (const char*)pvParameters;

    int retries = 150; // Wait up to 15 seconds for a cold router reconnect
    while (!wifi_connected && retries-- > 0) vTaskDelay(pdMS_TO_TICKS(100));
    if (!wifi_connected) { 
        ESP_LOGE(TAG, "Wi-Fi reconnect timed out! Aborting datalog.");
        if ((currentState == STATE_IDLE || currentState == STATE_LOW_BATTERY) && is_screen_off) wifi_pause();
        vTaskDelete(NULL); return; 
    }

    http_response_len = 0;
    memset(http_response_buf, 0, sizeof(http_response_buf));

    const esp_app_desc_t *app_desc = esp_app_get_description();
    int16_t current_voltage = M5.Power.getBatteryVoltage(); // Read the raw voltage just before sending!
    char url[512];
    snprintf(url, sizeof(url), "%s?battery=%ld&voltage=%d&state=%s&version=%s", GOOGLE_WEB_APP_URL, (long)M5.Power.getBatteryLevel(), current_voltage, custom_state, app_desc->version);

    esp_http_client_config_t config = {};
    config.url = url; config.method = HTTP_METHOD_GET; config.crt_bundle_attach = esp_crt_bundle_attach; 
    config.max_redirection_count = 5; config.event_handler = http_event_handler;
    config.timeout_ms = 15000; // Allow Google Apps Script time to cold-start and respond
    config.buffer_size = 2048; // Increase RX buffer to handle massive Google HTTP headers

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);

    if ((err == ESP_OK || err == ESP_ERR_HTTP_INCOMPLETE_DATA) && (status_code == 302 || status_code == 301)) {
        esp_http_client_set_redirection(client); http_response_len = 0; memset(http_response_buf, 0, sizeof(http_response_buf));
        err = esp_http_client_perform(client); status_code = esp_http_client_get_status_code(client);
    }

    if (err == ESP_OK || err == ESP_ERR_HTTP_INCOMPLETE_DATA) {
        ESP_LOGI(TAG, "Datalog successful! HTTP Status: %d", status_code);

        // STRICT VALIDATION: Only mark the OTA update as permanently successful 
        // AFTER we have proven the Wi-Fi and HTTPS stack are fully operational!
        if (!firmware_validated) {
            ota_mark_success();
            firmware_validated = true;
        }

        cJSON *json = cJSON_Parse(http_response_buf);
        if (json) {
            cJSON *time_ptr = cJSON_GetObjectItem(json, "reminder_time");
            cJSON *text_ptr = cJSON_GetObjectItem(json, "reminder_text");
            if (time_ptr && text_ptr && cJSON_IsString(time_ptr) && cJSON_IsString(text_ptr)) {
                strncpy(next_reminder_time, time_ptr->valuestring, sizeof(next_reminder_time)-1);
                next_reminder_time[sizeof(next_reminder_time)-1] = '\0';
                strncpy(next_reminder_text, text_ptr->valuestring, sizeof(next_reminder_text)-1);
                next_reminder_text[sizeof(next_reminder_text)-1] = '\0';
            }
            
            cJSON *params = cJSON_GetObjectItem(json, "params");
            if (params) {
                parse_and_save_parameters(params);
            }
            cJSON_Delete(json);
        }
    } else {
        ESP_LOGE(TAG, "Datalog failed: %s (Status: %d)", esp_err_to_name(err), status_code);
    }
    esp_http_client_cleanup(client);

    if ((currentState == STATE_IDLE || currentState == STATE_LOW_BATTERY) && is_screen_off) wifi_pause(); // Sleep Wi-Fi after logging
    vTaskDelete(NULL);
}

void log_status_to_google_sheets() {
    if (currentState != STATE_PROVISIONING) {
        const char* state_str = "UNKNOWN";
        if (currentState == STATE_IDLE) state_str = "IDLE";
        else if (currentState == STATE_PRE_ALARM) state_str = "PRE-ALARM";
        else if (currentState == STATE_ALARM_TRIGGERED) state_str = "TRIGGERED";
        else if (currentState == STATE_LOW_BATTERY) state_str = "LOW-BATT";
        else if (currentState == STATE_REMINDER) state_str = "REMINDER";
        xTaskCreate(log_custom_event_task, "log_task", 8192, (void*)state_str, 4, NULL);
    }
}

void log_custom_event(const char* custom_state) {
    xTaskCreate(log_custom_event_task, "log_event", 8192, (void*)custom_state, 4, NULL);
}