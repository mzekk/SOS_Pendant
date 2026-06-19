---
google_account: cmazit61@gmail.com
date_created: 2026-05-28 13:22
tags:
  - ESP32-S3
  - embedded
  - FreeRTOS
  - Elderly-Safety
  - Firmware
  - ESP-IDF
status: draft
hardware_target: M5Stack StickS3
---
The emergency pendant is developed on the [**M5Stack StickS3**](https://docs.m5stack.com/en/core/StickS3)) (powered by the **ESP32-S3** chip) using the **ESP-IDF** (Espressif IoT Development Framework). It provides the low-level control needed for power optimization, precise timing, and robust FreeRTOS task scheduling.

While there is no single "out-of-the-box" ESP-IDF example that combines all your SOS features, you can build a highly modular starting point by combining official Espressif components. 

Below is the recommended project architecture, mapping of ESP-IDF examples, and a bootstrap implementation for your multi-function button logic as specified in [[Device Features]].

---

### 1. Recommended ESP-IDF Project Architecture

To implement the state machine and hardware drivers cleanly, structure your ESP-IDF project as follows:

```text
sos_pendant/
├── CMakeLists.txt
├── main/
│   ├── CMakeLists.txt
│   ├── main.c                 # System initialization & State Machine
│   ├── button_handler.c       # Multi-function button logic
│   ├── imu_monitor.c          # Fall detection (I2C BMI270/MPU6886)
│   ├── power_monitor.c        # PMIC (AXP2101) battery management
│   └── wifi_client.c          # Wi-Fi & HTTPS/SMTP alert dispatch
└── components/
    └── axp2101/               # Custom component for the M5StickS3 PMIC
```

---

### 2. Mapping ESP-IDF Examples to Your Requirements

You can bootstrap your development by copying code from these official ESP-IDF v5.x sample projects (found in your ESP-IDF installation directory under `examples/`):

- **Wi-Fi Connectivity & Datalogging:**
  - *Reference Example:* `wifi/getting_started/station`
  - *Use Case:* Establishes the connection to the local home router.
- **HTTPS Alert Dispatch (Email API):**
  - *Reference Example:* `protocols/https_request`
  - *Use Case:* Dispatches the pre-alarm, snoozed, and triggered alarm emails via a secure REST API (e.g., SendGrid or Webhooks) instead of raw SMTP, which is easier to secure on-chip.
- **Two-Way Voice (PDM Microphone):**
  - *Reference Example:* `peripherals/i2s/i2s_pdm`
  - *Use Case:* Configures the onboard SPM1423 PDM microphone on the StickS3 to capture audio during an active alarm.
- **Low-Power & Battery Monitoring (I2C PMIC):**
  - *Reference Example:* `peripherals/i2c/i2c_tools`
  - *Use Case:* Communicates with the **AXP2101 PMIC** over I2C (typically Address `0x34` on M5Stack S3 devices) to monitor battery voltage, charging status, and trigger low-power sleep states.

---

### 3. Bootstrap Code: Multi-Function Button Handler

According to [[Device Features]], your button must distinguish between:
- **Snooze:** Pressed between $200\text{ ms}$ and $1000\text{ ms}$.
- **Help Request:** Pressed and held for $>3\text{ seconds}$ ($3000\text{ ms}$).

Instead of writing raw GPIO interrupt debouncing from scratch, use the official **ESP-IDF Button Component** from the Espressif Component Registry. 

Add this to your `main/idf_component.yml` file:
```yaml
dependencies:
  espressif/button: "^3.2.0"
```

Here is the C implementation (`button_handler.c`) to initialize and handle these specific timing thresholds:

```c
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iot_button.h"
#include "esp_log.h"

static const char *TAG = "BUTTON_HANDLER";

// M5StickS3 Button A (Front Button) is mapped to GPIO 0 (Active Low)
#define BOARD_BUTTON_GPIO    0 

// Define the system states matching your state machine
typedef enum {
    STATE_IDLE,
    STATE_PRE_ALARM,
    STATE_ALARM_TRIGGERED
} system_state_t;

volatile system_state_t current_state = STATE_IDLE;

static void button_press_end_cb(void *arg, void *usr_data)
{
    button_handle_t btn = (button_handle_t)arg;
    uint32_t press_duration_ms = iot_button_get_ticks_time(btn);
    
    ESP_LOGI(TAG, "Button released. Press duration: %lu ms", press_duration_ms);

    // 1. Help Request Trigger: Pressed and held for > 3000 ms
    if (press_duration_ms >= 3000) {
        ESP_LOGW(TAG, "EMERGENCY TRIGGERED: Help button held for > 3s!");
        current_state = STATE_PRE_ALARM;
        // TODO: Trigger pre_alarm state transition, start IMU/buzzer alert
    } 
    // 2. Pre-Alarm Snooze: Pressed between 200 ms and 1000 ms
    else if (press_duration_ms >= 200 && press_duration_ms <= 1000) {
        if (current_state == STATE_PRE_ALARM) {
            ESP_LOGI(TAG, "Snooze detected! Pre-alarm cancelled.");
            current_state = STATE_IDLE;
            // TODO: Dispatch snoozed pre-alarm email notification
        } else {
            ESP_LOGI(TAG, "Normal short press. Displaying system status...");
            // TODO: Wake up LCD screen and show status
        }
    }
}

void init_system_button(void)
{
    button_config_t gpio_btn_cfg = {
        .type = BUTTON_TYPE_GPIO,
        .long_press_time = 3000, // 3 seconds threshold
        .short_press_time = 200, // 200 ms threshold
        .gpio_button_config = {
            .gpio_num = BOARD_BUTTON_GPIO,
            .active_level = 0, // Active Low (0V when pressed)
        },
    };

    button_handle_t btn_handle = iot_button_create(&gpio_btn_cfg);
    if (btn_handle == NULL) {
        ESP_LOGE(TAG, "Failed to initialize button!");
        return;
    }

    // Register callback for when the button is released
    iot_button_register_cb(btn_handle, BUTTON_PRESS_END, button_press_end_cb, NULL);
    ESP_LOGI(TAG, "M5StickS3 Button initialized successfully.");
}
```

---

### 4. Hardware-Specific Parameters for M5StickS3

When configuring your drivers in ESP-IDF, use these exact hardware parameters for the StickS3:

- **I2C Bus Configuration:**
  - **SDA Pin:** GPIO 11
  - **SCL Pin:** GPIO 12
  - **IMU (BMI270) Address:** `0x68` (or `0x69` depending on hardware revision)
  - **PMIC (AXP2101) Address:** `0x34`
- **Onboard LCD (GC9107):**
  - **SPI Host:** SPI2_HOST
  - **MOSI:** GPIO 21
  - **SCLK:** GPIO 17
  - **CS:** GPIO 15
  - **DC:** GPIO 14
  - **RST:** GPIO 42
  - **Backlight (PWM):** GPIO 38