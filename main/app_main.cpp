#include "M5Unified.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_pm.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "driver/i2c.h"
#include <math.h>
#include <string.h>
#include <time.h>
#include "config.hpp"
#include "shared_state.hpp"
#include "network_manager.hpp"
#include "ota_manager.hpp"
#include "esp_app_desc.h"

static const char *TAG = "SOS_PHASE1";

// Instantiate the default parameters! (Order strictly matches struct in shared_state.hpp)
SystemParameters sys_params = {
    3000,    // tempo_prem_sos_ms
    20000,   // timeout_pre_allarme_ms
    30000,   // timeout_promemoria_ms
    300000,  // int_sincronizzazione_ms
    2.5f,    // soglia_caduta_g
    0.4f,    // soglia_vuoto_g
    1200,    // durata_batt_min (e.g. 20 hours default)
    10,      // batt_scarica_pct (Critically Low battery %)
    15,      // batt_ripresa_pct (Recovery %)
    30,      // batt_debole_pct (Survival mode %)
    200,     // durata_lowpwr_stato (Idle polling in ms)
    20000,   // spegnimento_schermo_ms
    5000,    // spegnimento_schermo_batt_ms
    true,    // abilita_caduta
    true,    // abilita_promemoria
    true,    // abilita_risveglio_mov
    true,    // abilita_beep_ui
    300000,  // timeout_wifi_prov_ms
    10000,   // tempo_prem_prov_ms
    false,   // abilita_veglia_sonno
    0.0f,    // base_x_g
    0.0f,    // base_y_g
    0.2f,    // base_tolleranza_g
    "08:00", // ora_inizio_veglia
    "22:00", // ora_fine_veglia
    "23:00", // ora_inizio_sonno
    "07:00", // ora_fine_sonno
    60,      // timeout_immobilita_min
    1.5f,    // soglia_mov_sonno_g
    5,       // max_mov_sonno
    70,      // display_norm_lum (70% default)
    1       // display_dim_lum (1% default)
};

SystemState currentState = STATE_PROVISIONING;
SystemState previousState = STATE_PROVISIONING;

bool ota_available = false;
char ota_url_target[256] = "";

const char* ota_error_str = "";

// Variables for manual button timing tracking
static uint32_t btn_press_start = 0;
static bool btn_is_pressed = false;
static bool emergency_handled = false;

#ifdef DEBUG_SYSTEM
static bool side_btn_is_pressed = false;
static uint32_t side_btn_press_start = 0;
static bool side_btn_handled = false;
static uint8_t debug_menu_index = 0;
static int8_t debug_menu_active_option = -1;
static uint32_t debug_last_interaction = 0;
#endif

// Power management and Reset UI variables
static uint32_t last_activity_time = 0;
bool is_screen_off = false;
static bool is_screen_dimmed = false;
static bool show_reset_reason = true;
static esp_reset_reason_t reset_reason;
static esp_sleep_wakeup_cause_t wakeup_reason;

// Global Sprite Buffer for 100% Flicker-Free Double Buffered Rendering
M5Canvas canvas(&M5.Lcd);

static esp_pm_lock_handle_t active_state_lock = NULL;
static bool is_active_state_locked = false;

// Fall detection variables
static bool freefall_detected = false;
static uint32_t freefall_time = 0;
static uint32_t pre_alarm_time = 0;

bool wifi_connected = false;
static bool low_battery_notified = false;
static bool survival_mode_active = false;

static uint32_t last_log_time = 0;

// Reminder Settings
char next_reminder_time[16] = "NONE";
char next_reminder_text[64] = "NONE";
static char last_triggered_reminder[16] = "";
static uint32_t reminder_start_time = 0;
static uint32_t last_beep_time = 0;
static uint32_t offline_start_time = 0;

// Embedded Audio Files 
extern "C" const uint8_t fall_wav_start[] asm("_binary_Fall_wav_start");
extern "C" const uint8_t fall_wav_end[]   asm("_binary_Fall_wav_end");
extern "C" const uint8_t help_wav_start[] asm("_binary_Help_wav_start");
extern "C" const uint8_t help_wav_end[]   asm("_binary_Help_wav_end");
extern "C" const uint8_t reminder_wav_start[] asm("_binary_Reminder_wav_start");
extern "C" const uint8_t reminder_wav_end[]   asm("_binary_Reminder_wav_end");
// Note: Use, ElevenLabs.io, Valeria Voice to generate MP3 Files, then convert to WAV with Audacity (16-bit PCM, 16kHz mono) for best compatibility with M5Speaker.

// Helper function to decode the reset reason
const char* get_reset_reason_str(esp_reset_reason_t r) {
    switch(r) {
        case ESP_RST_POWERON: return "Accensione";
        case ESP_RST_EXT: return "Pin Esterno";
        case ESP_RST_SW: return "Software";
        case ESP_RST_PANIC: return "Crash";
        case ESP_RST_INT_WDT: return "Int WDT";
        case ESP_RST_TASK_WDT: return "Task WDT";
        case ESP_RST_WDT: return "Altro WDT";
        case ESP_RST_DEEPSLEEP: return "Sospensione";
        case ESP_RST_BROWNOUT: return "Calo Tens.";
        default: return "Sconosciuto";
    }
}

// Helper function to decode why the ESP32 woke from Deep/Light Sleep
const char* get_wakeup_cause_str(esp_sleep_wakeup_cause_t w) {
    switch(w) {
        case ESP_SLEEP_WAKEUP_UNDEFINED: return "N/A";
        case ESP_SLEEP_WAKEUP_EXT0: return "EXT0 (IMU)";
        case ESP_SLEEP_WAKEUP_EXT1: return "EXT1 (Btn)";
        case ESP_SLEEP_WAKEUP_TIMER: return "Timer";
        default: return "Altro";
    }
}

void manage_active_state_lock(bool lock) {
#if CONFIG_PM_ENABLE
    if (active_state_lock != NULL) {
        if (lock && !is_active_state_locked) {
            esp_pm_lock_acquire(active_state_lock);
            is_active_state_locked = true;
        } else if (!lock && is_active_state_locked) {
            esp_pm_lock_release(active_state_lock);
            is_active_state_locked = false;
        }
    }
#endif
}

void init_custom_ext_imu_pins() {
    // GPIO4 & GPIO6: Always HIGH, High-Drive strength (~40mA max)
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << 4) | (1ULL << 6);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);
    
    gpio_set_level(GPIO_NUM_4, 1);
    gpio_set_level(GPIO_NUM_6, 1);
    gpio_set_drive_capability(GPIO_NUM_4, GPIO_DRIVE_CAP_3); 
    gpio_set_drive_capability(GPIO_NUM_6, GPIO_DRIVE_CAP_3);

    // GPIO7: Always, LOW High-Drive strength (~40mA max)
    gpio_set_direction(GPIO_NUM_7, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_7, 0);
    gpio_set_drive_capability(GPIO_NUM_7, GPIO_DRIVE_CAP_3);

    // GPIO2: Always HIGH
    gpio_set_direction(GPIO_NUM_2, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_2, 1);

    // GPIO3: IMU_IRQ, weak pull-up
    gpio_set_direction(GPIO_NUM_3, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_NUM_3, GPIO_PULLUP_ONLY);
    
    ESP_LOGI(TAG, "Custom External IMU Hardware Pins Initialized.");
}

void init_custom_ext_imu_i2c() {
    /*
    // TEMPORARILY DISABLED UNTIL HARDWARE ARRIVES
    // Use M5Unified's built-in external I2C wrapper to prevent ESP-IDF driver conflicts!
    M5.Ex_I2C.begin(I2C_NUM_1, 44, 43, 400000);
    ESP_LOGI(TAG, "Custom External IMU I2C Initialized on Pins 44 (SDA) and 43 (SCL)");
    */
}

// --- Custom PMIC I2C Wrappers ---
bool pmic_set_led(bool enable) {
    if (enable) return M5.In_I2C.bitOn(0x6E, 0x06, 1 << 4, 100000);
    else        return M5.In_I2C.bitOff(0x6E, 0x06, 1 << 4, 100000);
}

bool pmic_set_ldo_enable(bool enable) {
    if (enable) return M5.In_I2C.bitOn(0x6E, 0x06, 1 << 2, 100000);
    else        return M5.In_I2C.bitOff(0x6E, 0x06, 1 << 2, 100000);
}

bool pmic_set_ldo_power_hold(bool enable) {
    if (enable) return M5.In_I2C.bitOn(0x6E, 0x07, 1 << 5, 100000);
    else        return M5.In_I2C.bitOff(0x6E, 0x07, 1 << 5, 100000);
}

bool pmic_set_dcdc_enable(bool enable) {
    if (enable) return M5.In_I2C.bitOn(0x6E, 0x06, 1 << 1, 100000);
    else        return M5.In_I2C.bitOff(0x6E, 0x06, 1 << 1, 100000);
}

bool pmic_set_i2c_sleep(uint8_t seconds) {
    uint8_t cfg = M5.In_I2C.readRegister8(0x6E, 0x09, 100000);
    cfg = (cfg & ~0x0F) | (seconds & 0x0F);
    return M5.In_I2C.writeRegister8(0x6E, 0x09, cfg, 100000);
}

void enter_deep_sleep_mode() {
    ESP_LOGI(TAG, "Executing Pre-Sleep Isolation Sequence...");
    
    // 1. Indicator Suppression & Bus Dissociation
    bool led_ok = pmic_set_led(false); 

    // 2. PMIC L1 Sleep Preparation (Keep RTC LDO alive)
    bool ldo_en_ok = pmic_set_ldo_enable(true);
    bool ldo_hold_ok = pmic_set_ldo_power_hold(true);

    ESP_LOGI(TAG, "PMIC I2C Setup - LED: %s | LDO_EN: %s | LDO_HOLD: %s", 
             led_ok ? "OK" : "FAIL", ldo_en_ok ? "OK" : "FAIL", ldo_hold_ok ? "OK" : "FAIL");

    // 3. IO Leakage Prevention
    gpio_set_direction(GPIO_NUM_43, GPIO_MODE_DISABLE); // High-Z state to block ESD diode leakage
    gpio_set_direction(GPIO_NUM_44, GPIO_MODE_DISABLE); // High-Z state to block ESD diode leakage

    // 4. Configure the active RTC wakeup pins (LSM6DSOX & Main Button)
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL); // Kill lingering FreeRTOS Light Sleep timers!

    // Anchor IMU INT1 pin HIGH in the RTC domain to prevent floating LOW while disconnected
    rtc_gpio_pullup_en(GPIO_NUM_3);
    rtc_gpio_pulldown_dis(GPIO_NUM_3);
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_3, 0); 
    
    // Anchor Blue Button pin HIGH in the RTC domain
    rtc_gpio_pullup_en(GPIO_NUM_11);
    rtc_gpio_pulldown_dis(GPIO_NUM_11);
    esp_sleep_enable_ext1_wakeup(1ULL << GPIO_NUM_11, ESP_EXT1_WAKEUP_ANY_LOW);

    // 5. Force immediate system Deep Sleep state
    ESP_LOGI(TAG, "Entering Hardware Deep Sleep...");
    vTaskDelay(pdMS_TO_TICKS(100)); // Flush logs over USB BEFORE killing 5V!
    
    // 6. Final Power Cuts (Doing this earlier kills the USB COM port instantly!)
    M5.Power.setExtOutput(false);
    pmic_set_dcdc_enable(false);

    // Put PMIC into Auto-Sleep (Sleeps 2 seconds after ESP32 goes silent)
    pmic_set_i2c_sleep(2);
    
    esp_deep_sleep_start();

}

extern "C" void app_main(void) {
    // Read why the ESP32 restarted
    reset_reason = esp_reset_reason();
    wakeup_reason = esp_sleep_get_wakeup_cause();

    // 1. Initialize all M5 hardware (IMU, Display, Power, etc.)
    auto cfg = M5.config();
    M5.begin(cfg);
    
    // --- PMIC Wakeup Sequence ---
    // If we woke from Deep Sleep, the PMIC I2C is asleep. We MUST send a dummy START to wake it!
    M5.In_I2C.start(0x6E, false, 100000);
    M5.In_I2C.stop();
    vTaskDelay(pdMS_TO_TICKS(10));

    // Re-enable DCDC 5V rail and disable auto-sleep so the PMIC stays awake
    pmic_set_dcdc_enable(true);
    pmic_set_i2c_sleep(0);
    vTaskDelay(pdMS_TO_TICKS(50)); // Allow 5V rail to stabilize
    
    // The screen lost 5V power during Deep Sleep. We MUST re-initialize it!
    M5.Display.begin();

    // Initialize the custom breakout board pins
    init_custom_ext_imu_pins();
    init_custom_ext_imu_i2c();

    // 1.1 Turn off external 5V Boost Converter Rail (Saves ~5-10mA baseline!)
    M5.Power.setExtOutput(false);
    pmic_set_led(false); // Suppress LED on boot via wrapper

    // 1.5 Configure Automatic Light Sleep
#if CONFIG_PM_ENABLE
    esp_pm_config_t pm_config = {};
    pm_config.max_freq_mhz = 160;
    pm_config.min_freq_mhz = 10;
    pm_config.light_sleep_enable = true;
    
    ESP_ERROR_CHECK(esp_pm_configure(&pm_config));

    // Lock APB freq at max to prevent PWM jitter & USB dropout. This implicitly prevents Light Sleep.
    esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "ActiveState", &active_state_lock);
    manage_active_state_lock(true); // Screen is initially ON
#endif

    // 2. Configure Speaker (Lower volume prevents brownout resets!)
    M5.Speaker.setVolume(180); 

    // 3. Configure Display UI
    M5.Lcd.setRotation(3); // Horizontal orientation for the StickS3
    M5.Lcd.setTextSize(2);
    M5.Lcd.fillScreen(TFT_BLACK);

    // Allocate the sprite memory (240x135) to match the M5Stick-S3 display
    canvas.createSprite(M5.Lcd.width(), M5.Lcd.height());

    // 4. Initialize Network, Wi-Fi Provisioning, and SNTP Clock
    network_init();

    // Failsafe in case NVS loaded a 0 value for brightness
    if (sys_params.display_norm_lum == 0) sys_params.display_norm_lum = 50;
    if (sys_params.display_dim_lum == 0) sys_params.display_dim_lum = 10;

    // Set configured normal brightness now that NVS params are loaded
    M5.Lcd.setBrightness((sys_params.display_norm_lum * 255) / 100);

#ifdef DEBUG_SYSTEM
    // Configure G12 as a debug switch (Active Low, internal pull-up)
    gpio_reset_pin(GPIO_NUM_12);
    gpio_set_direction(GPIO_NUM_12, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_NUM_12, GPIO_PULLUP_ONLY);
#endif

    // --- The "Hold to Reset Wi-Fi" Recovery Feature ---
    M5.update();
    if (M5.BtnA.isPressed()) {
        ESP_LOGW(TAG, "Button held on boot! Erasing Wi-Fi credentials...");
        M5.Speaker.tone(1000, 500); // Audible confirmation of wipe
        network_reset_provisioning();
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }

    last_activity_time = M5.millis();
    ESP_LOGI(TAG, "Phase 1: UI & Button Logic Started");

    // Rate-limiting for PMIC I2C calls
    uint32_t last_power_check = 0;
    long current_battery = 100;
    int16_t current_voltage = 4200;
    bool is_plugged_in = false;
    bool was_plugged_in = false;
    uint32_t polling_delay = 50;

    while (true) {
        if (currentState != previousState) {
            previousState = currentState;
        }

        if (wifi_connected) offline_start_time = M5.millis();
        bool is_prolonged_offline = (!wifi_connected && (M5.millis() - offline_start_time > sys_params.timeout_wifi_prov_ms));

        // Update all M5Stack hardware states (Crucial for button & IMU reads)
        M5.update();

        // --- IMU Reading ---
        float accelX = 0, accelY = 0, accelZ = 0;
        float accel_mag = 1.0f;
        bool robust_shake = false;

        // Suspend IMU polling entirely during Survival Mode to preserve core battery power
        if (!survival_mode_active) {
            M5.Imu.getAccel(&accelX, &accelY, &accelZ);
            accel_mag = sqrt(accelX*accelX + accelY*accelY + accelZ*accelZ);
            robust_shake = sys_params.abilita_risveglio_mov && (accel_mag > sys_params.soglia_caduta_g);
        }

        // --- Fall Detection Algorithm ---
        if (!survival_mode_active && currentState != STATE_PROVISIONING && sys_params.abilita_caduta && wifi_connected) {
            // Step 1: Detect freefall (Weightlessness < 0.4G)
            if (accel_mag < sys_params.soglia_vuoto_g) {
                freefall_detected = true;
                freefall_time = M5.millis();
            }

            // Step 2: Detect impact shortly after freefall
            if (freefall_detected) {
                // If more than 1.5 seconds pass without an impact, reset (it was just a dip or sensor noise)
                if (M5.millis() - freefall_time > 1500) {
                    freefall_detected = false;
                } 
                // If we hit a high G-force right after a freefall, it's a true fall!
                else if (accel_mag > sys_params.soglia_caduta_g && currentState == STATE_IDLE) {
                wifi_resume(); // WAKE UP RADIO IMMEDIATELY!
                    currentState = STATE_PRE_ALARM;
                    pre_alarm_time = M5.millis();
                    ESP_LOGW(TAG, "EMERGENCY: Fall Detected!");
                    freefall_detected = false; // Reset to avoid double-triggering
                    send_emergency_email("PRE-ALARM: Fall Detected!");

                    M5.Speaker.setVolume(255); // Max volume for voice
                    M5.Speaker.playWav(fall_wav_start, fall_wav_end - fall_wav_start, 1); // Play 1 time
                    bool cancelled_early = false;
                    while(M5.Speaker.isPlaying()) { 
                        M5.update(); 
                        if (M5.BtnA.wasPressed()) { cancelled_early = true; M5.Speaker.stop(); break; }
                        vTaskDelay(1); 
                    }
                    M5.Speaker.setVolume(180); // Restore safe volume for beeps

                    if (cancelled_early) {
                        currentState = STATE_IDLE;
                        if (sys_params.abilita_beep_ui) M5.Speaker.tone(3000, 200);
                        ESP_LOGI(TAG, "Pre-Alarm Snoozed during prompt!");
                        send_emergency_email("CANCELLED: User snoozed the alarm.");
                    }
                }
            }
        }

        // --- Low Battery Management ---
        if (M5.millis() - last_power_check > 2000) {
            last_power_check = M5.millis();
            current_battery = (long)M5.Power.getBatteryLevel();
            current_voltage = M5.Power.getBatteryVoltage();
            is_plugged_in = (M5.Power.getVBUSVoltage() > 4000); // More reliable than isCharging()

            // --- Survival Mode Hysteresis ---
            if (!survival_mode_active && current_battery <= sys_params.batt_debole_pct) {
                survival_mode_active = true;
                ESP_LOGW(TAG, "Battery critical! Entering SURVIVAL MODE (IMU Off).");
            } else if (survival_mode_active && current_battery >= sys_params.batt_debole_pct + 3) {
                survival_mode_active = false;
            }
        }

        // --- Cradle Docked Detection ---
        if (is_plugged_in && !was_plugged_in) {
            ESP_LOGI(TAG, "Device placed in cradle!");
            log_custom_event("CRADLE_DOCKED");
        }
        was_plugged_in = is_plugged_in;

        // --- Side Button (G12) Debug Logic ---
#ifdef DEBUG_SYSTEM
        bool side_btn_reading = (gpio_get_level(GPIO_NUM_12) == 0);
        if (side_btn_reading && !side_btn_is_pressed) {
            side_btn_is_pressed = true;
            side_btn_press_start = M5.millis();
            side_btn_handled = false;
            debug_last_interaction = M5.millis();
            last_activity_time = M5.millis();
        } else if (side_btn_reading && side_btn_is_pressed) {
            uint32_t dur = M5.millis() - side_btn_press_start;
            if (currentState != STATE_DEBUG_MENU && dur > 3000 && !side_btn_handled) {
                currentState = STATE_DEBUG_MENU;
                debug_menu_index = 0;
                debug_menu_active_option = -1;
                debug_last_interaction = M5.millis();
                side_btn_handled = true;
                if (is_screen_off || is_screen_dimmed) {
                    if (is_screen_off) M5.Lcd.wakeup();
                    is_screen_off = false;
                    is_screen_dimmed = false;
                    manage_active_state_lock(true);
                }
                M5.Lcd.setBrightness((sys_params.display_norm_lum * 255) / 100);
                M5.Speaker.tone(2000, 200);
            } else if (currentState == STATE_DEBUG_MENU) {
                if (debug_menu_active_option != -1 && dur > 3000 && !side_btn_handled) {
                    debug_menu_active_option = -1; // Return to main menu
                    debug_last_interaction = M5.millis();
                    side_btn_handled = true;
                    M5.Lcd.fillScreen(TFT_BLACK);
                    M5.Speaker.tone(1500, 200);
                } else if (debug_menu_active_option == -1 && dur > 500 && !side_btn_handled) {
                    debug_menu_active_option = debug_menu_index;
                    debug_last_interaction = M5.millis();
                    side_btn_handled = true;
                    M5.Lcd.fillScreen(TFT_BLACK);
                    M5.Speaker.tone(3000, 150);
                    if (debug_menu_active_option == 5) { // Option 6: Exit
                        currentState = STATE_IDLE;
                        debug_menu_active_option = -1;
                        last_activity_time = M5.millis();
                    } else if (debug_menu_active_option == 1) { // Option 2: Force DeepSleep
                        M5.Speaker.tone(1000, 500);
                        canvas.fillScreen(TFT_BLACK);
                        canvas.setCursor(0, 40); canvas.setTextColor(TFT_RED); canvas.print(" DEEP SLEEP...");
                        canvas.pushSprite(0, 0); // Push warning to screen before CPU dies
                        
                        // Wait for the user to completely release the SIDE button to prevent bouncing!
                        while (gpio_get_level(GPIO_NUM_12) == 0) {
                            vTaskDelay(pdMS_TO_TICKS(10));
                        }
                        vTaskDelay(pdMS_TO_TICKS(500)); // Add a small debounce buffer

                        // Trigger advanced Deep Sleep Isolation routine defined by hardware blueprints
                        enter_deep_sleep_mode();
                    }
                }
            }
        } else if (!side_btn_reading && side_btn_is_pressed) {
            side_btn_is_pressed = false;
        }
#endif

        // --- OTA Update Trigger ---
        // Only start OTA if plugged in, battery is safe, and we are idle
        if (ota_available && is_plugged_in && current_battery > 20 && currentState == STATE_IDLE) {
            ESP_LOGI(TAG, "OTA conditions met! Locking state machine.");
            currentState = STATE_OTA_UPDATING;
            ota_available = false; // Reset to avoid looping
            start_ota_update(ota_url_target);
        }

    if (currentState == STATE_IDLE && current_battery <= sys_params.batt_scarica_pct) {
            currentState = STATE_LOW_BATTERY;
            if (!low_battery_notified && wifi_connected) {
                send_emergency_email("WARNING: Pendant Battery is Critically Low!");
                low_battery_notified = true; // Prevent spamming emails
            }
    } else if (currentState == STATE_LOW_BATTERY) {
        // Hysteresis: wait until it charges back up to recovery threshold before clearing the warning
        // Safety: Ensure recovery threshold is strictly greater than discharge threshold to prevent endless email oscillation!
        uint32_t safe_recovery = (sys_params.batt_ripresa_pct > sys_params.batt_scarica_pct) ? sys_params.batt_ripresa_pct : (sys_params.batt_scarica_pct + 5);
        if (current_battery >= safe_recovery) {
            currentState = STATE_IDLE;
            low_battery_notified = false;
        }
        }

        // --- Power Saving Logic ---
        // If plugged in, NEVER sleep Wi-Fi. Wake it up and switch screen to dim if it was sleeping.
        if (is_plugged_in) {
            if (wifi_is_paused) wifi_resume();
            if (is_screen_off) {
                M5.Lcd.wakeup();
                M5.Lcd.setBrightness((sys_params.display_dim_lum * 255) / 100);
                is_screen_off = false;
                is_screen_dimmed = true;
                manage_active_state_lock(true); // Lock out frequency scaling to keep screen and USB stable
            }
        }

        if (M5.BtnA.isPressed() || robust_shake) {
            last_activity_time = M5.millis();
            if (is_screen_off || is_screen_dimmed) {
                // Wake up Wi-Fi FIRST to let the large inrush current from the radio stabilize
                // before turning on the screen backlight. This prevents a voltage sag that causes the display to flicker.
                if (wifi_is_paused) {
                    wifi_resume();
                    vTaskDelay(pdMS_TO_TICKS(50)); // Allow power to stabilize
                }

                if (is_screen_off) M5.Lcd.wakeup(); // Now, wake up the screen
                M5.Lcd.setBrightness((sys_params.display_norm_lum * 255) / 100);
                is_screen_off = false;
                is_screen_dimmed = false;
                manage_active_state_lock(true);
            }
        }

#ifdef DEBUG_SYSTEM
        // Debug Menu Inactivity Timeout
        if (currentState == STATE_DEBUG_MENU) {
            if (M5.millis() - debug_last_interaction > 10000) {
                if (debug_menu_active_option != -1) {
                    debug_menu_active_option = -1;
                    debug_last_interaction = M5.millis();
                    M5.Lcd.fillScreen(TFT_BLACK);
                } else {
                    currentState = STATE_IDLE;
                    last_activity_time = M5.millis();
                }
            }
        }
#endif

        // Dynamic screen timeout: 5s if low battery, 20s if normal
        uint32_t screen_timeout = (currentState == STATE_LOW_BATTERY) ? sys_params.spegnimento_schermo_batt_ms : sys_params.spegnimento_schermo_ms;
        if (!is_screen_off && (M5.millis() - last_activity_time > screen_timeout)) {
            if (currentState != STATE_PROVISIONING && currentState != STATE_DEBUG_MENU) {
                if (is_plugged_in && !is_screen_dimmed) {
                    // Dim screen instead of sleeping
                    M5.Lcd.setBrightness((sys_params.display_dim_lum * 255) / 100);
                    is_screen_dimmed = true;
                } else if (!is_plugged_in) {
                    M5.Lcd.setBrightness(0);
                    M5.Lcd.sleep();
                    is_screen_off = true;
                    is_screen_dimmed = false;
                    manage_active_state_lock(false); // Screen is off, allow FreeRTOS scaling and sleep!
                if (currentState == STATE_IDLE || currentState == STATE_LOW_BATTERY) {
                    wifi_pause(); // Put Wi-Fi to sleep when screen sleeps
                }
                }
            }
        }

        // --- Periodic Datalogging ---
        if (last_log_time == 0 || M5.millis() - last_log_time > sys_params.int_sincronizzazione_ms) {
            last_log_time = M5.millis();
            wifi_resume(); // Wake up radio for background sync
            log_status_to_google_sheets();
        }

        // --- Reminder & Triggered Alarm Active Logic ---
        if (currentState == STATE_REMINDER) {
            if (M5.millis() - last_beep_time >= 5000) {
                last_beep_time = M5.millis();
                if (sys_params.abilita_beep_ui) M5.Speaker.tone(3000, 500);
            }
            if (M5.millis() - reminder_start_time >= sys_params.timeout_promemoria_ms) {
                currentState = STATE_IDLE;
                ESP_LOGW(TAG, "Reminder Timed Out! No Ack.");
                log_custom_event("REM_MISSED");
            }
        } else if (currentState == STATE_ALARM_TRIGGERED) {
            if (M5.millis() - last_beep_time >= 5000) {
                last_beep_time = M5.millis();
                if (sys_params.abilita_beep_ui) M5.Speaker.tone(4000, 1000); // Continuous 1-sec siren every 5s until reset
            }
        }

        // --- Alarm Timeout Logic (20 Seconds without Snooze) ---
        if (currentState == STATE_PRE_ALARM && (M5.millis() - pre_alarm_time > sys_params.timeout_pre_allarme_ms)) {
            currentState = STATE_ALARM_TRIGGERED;
            if (is_screen_off || is_screen_dimmed) { 
                if (is_screen_off) M5.Lcd.wakeup(); 
                M5.Lcd.setBrightness((sys_params.display_norm_lum * 255) / 100); 
                is_screen_off = false; 
                is_screen_dimmed = false; 
                manage_active_state_lock(true);
            }
            last_activity_time = M5.millis();
            M5.Speaker.setVolume(255); // Max volume for voice
            M5.Speaker.playWav(help_wav_start, help_wav_end - help_wav_start, 1); // Play 1 time
            while(M5.Speaker.isPlaying()) { M5.update(); vTaskDelay(1); } // Wait for prompt to finish
            M5.Speaker.setVolume(180); // Restore safe volume for beeps
            ESP_LOGW(TAG, "EMERGENCY: Unacknowledged Alarm Fully Triggered!");
            send_emergency_email("URGENT ALARM: User did not respond to Pre-Alarm!");
            send_pushover_alert("URGENT ALARM: Fall or Help Request Unacknowledged! Immediate assistance required.");
        }

        // --- Button Logic (Matching the Spec) ---
        if (M5.BtnA.isPressed()) {
            if (!btn_is_pressed) {
                btn_is_pressed = true;
                btn_press_start = M5.millis();
                emergency_handled = false;
                show_reset_reason = false; // Clear reset text on first press

#ifdef DEBUG_SYSTEM
                    if (currentState == STATE_DEBUG_MENU) {
                        debug_last_interaction = M5.millis();
                        last_activity_time = M5.millis();
                        emergency_handled = true; // Prevent normal SOS logic
                        
                        if (debug_menu_active_option == -1) {
                            debug_menu_index = (debug_menu_index + 1) % 6; // Cycle options
                            M5.Speaker.tone(2500, 50);
                        } else {
                            if (debug_menu_active_option == 0) {
                                if (wifi_is_paused) wifi_resume();
                                log_status_to_google_sheets();
                                debug_menu_active_option = -1;
                                M5.Lcd.fillScreen(TFT_BLACK);
                            } else if (debug_menu_active_option == 1) {
                                // --- TEMPORARY DEEP SLEEP TEST (Commented out original Prov logic) ---
                                // M5.Speaker.tone(1000, 500);
                                // network_reset_provisioning();
                                // vTaskDelay(pdMS_TO_TICKS(2000));
                                // esp_restart();
                            } else if (debug_menu_active_option == 2) {
                                if (wifi_is_paused) wifi_resume();
                                currentState = STATE_PRE_ALARM;
                                pre_alarm_time = M5.millis();
                                M5.Speaker.setVolume(255);
                                M5.Speaker.playWav(fall_wav_start, fall_wav_end - fall_wav_start, 1);
                                while(M5.Speaker.isPlaying()) { M5.update(); vTaskDelay(1); }
                                M5.Speaker.setVolume(180);
                                send_emergency_email("PRE-ALARM: Fall Detected! (DEBUG FORCED)");
                                debug_menu_active_option = -1;
                            } else if (debug_menu_active_option == 3) {
                                M5.Speaker.tone(1000, 500);
                                M5.Lcd.fillScreen(TFT_BLACK);
                                M5.Lcd.setCursor(0, 40); M5.Lcd.setTextColor(TFT_RED); M5.Lcd.println(" ROLLBACK...");
                                ota_force_rollback();
                            } else if (debug_menu_active_option == 4) {
                                debug_menu_active_option = -1; // Return to menu
                                M5.Lcd.fillScreen(TFT_BLACK);
                                M5.Speaker.tone(1500, 200);
                            }
                        }
                    }
#endif
            }
                else if (currentState != STATE_PROVISIONING && currentState != STATE_DEBUG_MENU) {
                uint32_t press_duration = M5.millis() - btn_press_start;

                // Normal Trigger Logic
                if (!emergency_handled) {
                    if (!wifi_connected && press_duration >= sys_params.tempo_prem_prov_ms) {
                        ESP_LOGW(TAG, "Entering Wi-Fi Provisioning...");
                        M5.Speaker.tone(1000, 500);
                        network_reset_provisioning();
                        vTaskDelay(pdMS_TO_TICKS(2000));
                        esp_restart();
                        emergency_handled = true;
                } else if (currentState == STATE_ALARM_TRIGGERED && press_duration >= 3000) {
                    // Manual Reset from triggered alarm
                    currentState = STATE_IDLE;
                    emergency_handled = true;
                    M5.Speaker.tone(2000, 500);
                    ESP_LOGI(TAG, "Alarm Manually Reset!");
                    send_emergency_email("RESOLVED: Alarm was manually reset.");
                } else if (currentState != STATE_ALARM_TRIGGERED && press_duration >= sys_params.tempo_prem_sos_ms) {
                    wifi_resume(); // WAKE UP RADIO IMMEDIATELY!
                        currentState = STATE_PRE_ALARM;
                        pre_alarm_time = M5.millis();
                        ESP_LOGW(TAG, "EMERGENCY: Help button held!");
                        emergency_handled = true;
                        send_emergency_email("PRE-ALARM: Help Button Pressed!");

                        M5.Speaker.setVolume(255); // Max volume for voice
                        M5.Speaker.playWav(help_wav_start, help_wav_end - help_wav_start, 1); // Play Help 1 time
                        bool cancelled_early = false;
                        while(M5.Speaker.isPlaying()) { 
                            M5.update(); 
                            if (M5.BtnA.wasReleased()) {} // Register release
                            if (M5.BtnA.wasPressed()) { cancelled_early = true; M5.Speaker.stop(); break; }
                            vTaskDelay(1); 
                        }
                        M5.Speaker.setVolume(180); // Restore safe volume for beeps

                        if (cancelled_early) {
                            currentState = STATE_IDLE;
                            if (sys_params.abilita_beep_ui) M5.Speaker.tone(3000, 200);
                            ESP_LOGI(TAG, "Pre-Alarm Snoozed during prompt!");
                            send_emergency_email("CANCELLED: User snoozed the alarm.");
                        }
                    }
                }
            }
        } else {
            if (btn_is_pressed) {
                btn_is_pressed = false;
                uint32_t duration = M5.millis() - btn_press_start;
                
                // Only process short presses if the SOS wasn't already triggered
                if (!emergency_handled && currentState != STATE_PROVISIONING && currentState != STATE_DEBUG_MENU) {
                    if (!wifi_connected) {
                        if (sys_params.abilita_beep_ui) M5.Speaker.tone(1000, 150);
                        ESP_LOGI(TAG, "Offline status check");
                    } else if (currentState == STATE_REMINDER) {
                        if (duration >= 1000 && duration < 3000) { // Require > 1 sec press to Acknowledge
                            currentState = STATE_IDLE;
                            if (sys_params.abilita_beep_ui) M5.Speaker.tone(4000, 150); // Acknowledge beep
                            ESP_LOGI(TAG, "Reminder Acknowledged!");
                            log_custom_event("REM_ACKED");
                        }
                    } else if (duration >= 50 && duration <= 2000) {
                        // Snooze Pre-Alarm: 50ms to 2000ms (Widened for reliable tapping)
                        if (currentState == STATE_PRE_ALARM) {
                            currentState = STATE_IDLE;
                            if (sys_params.abilita_beep_ui) M5.Speaker.tone(3000, 200); // Higher pitch for audibility
                            ESP_LOGI(TAG, "Pre-Alarm Snoozed!");
                            send_emergency_email("CANCELLED: User snoozed the alarm.");
                        } else {
                            // Regular short press in IDLE -> Status check
                            if (sys_params.abilita_beep_ui) M5.Speaker.tone(2000, 150);
                            ESP_LOGI(TAG, "Status check requested");
                        }
                    }
                }
            }
        }

        // --- Time Sync & Reminder Check (Must run even if screen is off!) ---
        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);
        
        if (timeinfo.tm_year > (2020 - 1900)) {
            // --- Reminder Time Check ---
            if (currentState == STATE_IDLE && sys_params.abilita_promemoria && strcmp(next_reminder_time, "NONE") != 0) {
                char current_time_str[16];
                snprintf(current_time_str, sizeof(current_time_str), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
                
                if (strcmp(current_time_str, next_reminder_time) == 0 && strcmp(last_triggered_reminder, next_reminder_time) != 0) {
                    currentState = STATE_REMINDER;
                    strncpy(last_triggered_reminder, next_reminder_time, sizeof(last_triggered_reminder)-1);
                    reminder_start_time = M5.millis();
                    last_beep_time = M5.millis();
                    M5.Speaker.setVolume(255); // Max volume for voice
                    M5.Speaker.playWav(reminder_wav_start, reminder_wav_end - reminder_wav_start, 1); // Play 1 time
                    bool acked_early = false;
                    while(M5.Speaker.isPlaying()) { 
                        M5.update(); 
                        if (M5.BtnA.pressedFor(1000)) { acked_early = true; M5.Speaker.stop(); break; }
                        vTaskDelay(1); 
                    }
                    M5.Speaker.setVolume(180); // Restore safe volume for beeps
                    if (is_screen_off || is_screen_dimmed) { 
                        if (is_screen_off) M5.Lcd.wakeup(); 
                        M5.Lcd.setBrightness((sys_params.display_norm_lum * 255) / 100); 
                        is_screen_off = false; 
                        is_screen_dimmed = false; 
                        manage_active_state_lock(true);
                    }
                    last_activity_time = M5.millis();
                    
                    if (acked_early) {
                        currentState = STATE_IDLE;
                        if (sys_params.abilita_beep_ui) M5.Speaker.tone(4000, 150);
                        ESP_LOGI(TAG, "Reminder Acknowledged early!");
                        log_custom_event("REM_ACKED");
                    }
                }
            }
        }

        // --- UI Rendering (Only update if screen is awake) ---
        if (!is_screen_off) {
            canvas.fillScreen(TFT_BLACK); // 1. Wipe the invisible RAM buffer clean!
            canvas.setTextWrap(false); // Prevents text wrapping from causing screen scrolling/flickering!
            
            if (currentState == STATE_PROVISIONING) {
                canvas.setFont(&fonts::DejaVu18);
                canvas.setTextColor(TFT_YELLOW, TFT_BLACK);
                canvas.setCursor(0, 10); canvas.print("MODALITA' SETUP        ");
                canvas.setTextColor(TFT_WHITE, TFT_BLACK);
                canvas.setCursor(0, 40); canvas.print("App: ESP SoftAP        ");
                canvas.setCursor(0, 70); canvas.print("Rete: PROV_M5STICK     ");
                canvas.setCursor(0, 100); canvas.print("Pass: 1234567          ");
            } else if (is_prolonged_offline) {
                canvas.setFont(&fonts::DejaVu18);
                canvas.setTextColor(TFT_YELLOW, TFT_BLACK);
                canvas.setCursor(0, 10); canvas.print("WIFI PERSO!            ");
                canvas.setTextColor(TFT_WHITE, TFT_BLACK);
                canvas.setCursor(0, 40); canvas.print("Tieni premuto 10s      ");
                canvas.setCursor(0, 70); canvas.print("per Setup Wi-Fi.       ");
                canvas.setTextColor(TFT_RED, TFT_BLACK);
                canvas.setCursor(0, 110); canvas.print("SOS DISABILITATO.      ");
            } else if (currentState == STATE_REMINDER) {
                canvas.setFont(&fonts::DejaVu18);
                canvas.setTextColor(TFT_CYAN, TFT_BLACK);
                canvas.setCursor(0, 20); canvas.print("PROMEMORIA:            ");
                canvas.setTextColor(TFT_WHITE, TFT_BLACK);
                canvas.setCursor(0, 60); canvas.print(next_reminder_text);
            } else if (currentState == STATE_OTA_UPDATING) {
                canvas.setFont(&fonts::DejaVu18);
                canvas.setTextColor(TFT_YELLOW, TFT_BLACK);
                canvas.setCursor(0, 20); canvas.print("AGGIORNAMENTO          ");
                canvas.setCursor(0, 50); canvas.print("FIRMWARE...            ");
                canvas.setTextColor(TFT_WHITE, TFT_BLACK);
                canvas.setCursor(0, 90); canvas.print("NON STACCARE           ");
                canvas.setCursor(0, 115); canvas.print("DALLA BASE!            ");
            } else if (currentState == STATE_OTA_SUCCESS) {
                canvas.setFont(&fonts::DejaVu18);
                canvas.setTextColor(TFT_GREEN, TFT_BLACK);
                canvas.setCursor(0, 30); canvas.print("AGGIORNAMENTO          ");
                canvas.setCursor(0, 60); canvas.print("COMPLETATO!            ");
                canvas.setTextColor(TFT_WHITE, TFT_BLACK);
                canvas.setCursor(0, 100); canvas.print("Riavvio...             ");
            } else if (currentState == STATE_OTA_FAILED) {
                canvas.setFont(&fonts::DejaVu18);
                canvas.setTextColor(TFT_RED, TFT_BLACK);
                canvas.setCursor(0, 20); canvas.print("OTA FALLITO!           ");
                canvas.setCursor(0, 60); canvas.printf("ERR: %-15s", ota_error_str);
#ifdef DEBUG_SYSTEM
            } else if (currentState == STATE_DEBUG_MENU) {
                canvas.setFont(&fonts::DejaVu18); // Use the requested larger smooth font
                canvas.setTextSize(1);
                
                if (debug_menu_active_option == -1) {
                    canvas.setCursor(0, 0); canvas.setTextColor(TFT_YELLOW, TFT_BLACK); canvas.print("--- DEBUG MENU ---               ");
                    canvas.setTextColor(debug_menu_index == 0 ? TFT_BLACK : TFT_WHITE, debug_menu_index == 0 ? TFT_WHITE : TFT_BLACK);
                    canvas.setCursor(0, 20); canvas.print("1. Sync Network                  ");
                    canvas.setTextColor(debug_menu_index == 1 ? TFT_BLACK : TFT_WHITE, debug_menu_index == 1 ? TFT_WHITE : TFT_BLACK);
                    // canvas.setCursor(0, 40); canvas.print("2. Force Prov.                   "); // Original
                    canvas.setCursor(0, 40); canvas.print("2. Force DeepSleep               "); // Temp Test
                    canvas.setTextColor(debug_menu_index == 2 ? TFT_BLACK : TFT_WHITE, debug_menu_index == 2 ? TFT_WHITE : TFT_BLACK);
                    canvas.setCursor(0, 60); canvas.print("3. Force Fall                    ");
                    canvas.setTextColor(debug_menu_index == 3 ? TFT_BLACK : TFT_WHITE, debug_menu_index == 3 ? TFT_WHITE : TFT_BLACK);
                    canvas.setCursor(0, 80); canvas.print("4. Rollback OTA                  ");
                    canvas.setTextColor(debug_menu_index == 4 ? TFT_BLACK : TFT_WHITE, debug_menu_index == 4 ? TFT_WHITE : TFT_BLACK);
                    canvas.setCursor(0, 100); canvas.print("5. System Info                   ");
                    canvas.setTextColor(debug_menu_index == 5 ? TFT_BLACK : TFT_WHITE, debug_menu_index == 5 ? TFT_WHITE : TFT_BLACK);
                    canvas.setCursor(0, 120); canvas.print("6. Exit                          ");
                } else {
                    
                    if (debug_menu_active_option != 4) {
                        canvas.setCursor(0, 0); canvas.setTextColor(TFT_WHITE, TFT_BLACK); canvas.print("Esegui Opzione?                  ");
                        
                        canvas.setCursor(0, 20);
                        canvas.setTextColor(TFT_ORANGE, TFT_BLACK);
                        if (debug_menu_active_option == 0) canvas.print("[ Sync Network ]                 ");
                        // else if (debug_menu_active_option == 1) canvas.print("[ Force Prov. ]                  "); // Original
                        else if (debug_menu_active_option == 1) canvas.print("[ Force DeepSleep ]              "); // Temp Test
                        else if (debug_menu_active_option == 2) canvas.print("[ Force Fall ]                   ");
                        else if (debug_menu_active_option == 3) canvas.print("[ Rollback OTA ]                 ");

                        canvas.setTextColor(TFT_CYAN, TFT_BLACK);
                        canvas.setCursor(0, 60); canvas.print("Premi Blu -> OK                  ");
                        canvas.setCursor(0, 80); canvas.print("Lat > 3s  -> ESC                 ");
                    } else {
                        // --- LIVE SYSTEM INFO SCREEN ---
                        canvas.setCursor(0, 0); canvas.setTextColor(TFT_YELLOW, TFT_BLACK); canvas.print("--- SYSTEM INFO ---");
                        canvas.setTextColor(TFT_WHITE, TFT_BLACK);
                        const esp_app_desc_t *app_desc = esp_app_get_description();
                        canvas.setCursor(0, 16); canvas.printf("V:%s B:%ld%%%s %dmV          ", app_desc->version, current_battery, is_plugged_in ? "+C" : "", current_voltage);
                        canvas.setCursor(0, 32); canvas.printf("RST: %-15s             ", get_reset_reason_str(reset_reason));
                        canvas.setCursor(0, 48); canvas.printf("WAK: %-15s             ", get_wakeup_cause_str(wakeup_reason));
                        canvas.setCursor(0, 64); canvas.printf("WIF: %-15s             ", wifi_connected ? "CONN" : (wifi_is_paused ? "SLEP" : "DISC"));
                        canvas.setCursor(0, 80); canvas.printf("XYZ: %.1f,%.1f,%.1f              ", accelX, accelY, accelZ);
                        canvas.setTextColor(TFT_CYAN, TFT_BLACK);
                        canvas.setCursor(0, 115); canvas.print("Lat > 3s -> ESC                  ");
                    }
                }
#endif
            } else {
                // --- MAIN USER INTERFACE ---
                canvas.setFont(&fonts::DejaVu18); // Use large crisp font for 240x135 display!
                canvas.setTextSize(1);

                canvas.setCursor(0, 10);
                if (currentState == STATE_IDLE) {
                    canvas.setTextColor(TFT_GREEN, TFT_BLACK);
                    canvas.print("STATO: ATTIVO             ");
                } else if (currentState == STATE_PRE_ALARM) {
                    canvas.setTextColor(TFT_ORANGE, TFT_BLACK);
                    canvas.print("STATO: PRE-ALLARME        ");
                } else if (currentState == STATE_ALARM_TRIGGERED) {
                    canvas.setTextColor(TFT_RED, TFT_BLACK);
                    canvas.print("STATO: ALLARME!           ");
                } else if (currentState == STATE_LOW_BATTERY) {
                    canvas.setTextColor(TFT_RED, TFT_BLACK);
                    canvas.print("STATO: BATT BASSA         ");
                }

                canvas.setCursor(0, 45);
                canvas.setTextColor(TFT_WHITE, TFT_BLACK);
                canvas.printf("Batt: %ld%% %s             ", current_battery, is_plugged_in ? "[In Carica]" : "           ");
                
                canvas.setCursor(0, 80);
                if (timeinfo.tm_year > (2020 - 1900)) {
                    canvas.printf("Ora: %02d:%02d:%02d               ", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
                } else {
                    canvas.print("Ora: Sincro...               ");
                }
                
                canvas.setCursor(0, 115);
                if (!wifi_connected && !wifi_is_paused) {
                    canvas.setTextColor(TFT_RED, TFT_BLACK);
                    canvas.print("WIFI: DISCONNESSO            ");
                } else {
                    canvas.print("                             "); 
                }
            }
            
            // 2. Blast the finished drawing to the screen perfectly instantly!
            canvas.pushSprite(0, 0);
        }

        // Yield to FreeRTOS to prevent watchdog resets
        polling_delay = 50; // Active UI polling
        if (is_screen_off) {
            polling_delay = survival_mode_active ? 1000 : sys_params.durata_lowpwr_stato;
        }
        vTaskDelay(pdMS_TO_TICKS(polling_delay));
    }
}