---
google_account: cmazit61@gmail.com
date_created: 2026-05-28 13:22
---
# Low-Power Architecture Blueprint: SOS Safety Device

## 1. System Constraints & Hardware Topology

- **Power Source:** Single-cell Li-Ion Battery ($250\text{ mAh}$)
    
- **Host Microcontroller:** ESP32-S3 (M5StickS3 Core Module)
    
- **Primary PMIC:** PY32 Co-processor (M5PM1 Architecture)
    
- **External IMU Sensor:** STMicroelectronics LSM6DSOX (Dedicated Bus)
    
- **Target Sleep Budget:** $< 35\,\mu\text{A}$ baseline total system draw
    

### Pin Mapping Matrix

```
[LSM6DSOX IMU]                 [ESP32-S3 Host]
  ├── SDA ───────────────────────► GPIO43 (Standard I2C Data)
  ├── SCL ───────────────────────► GPIO44 (Standard I2C Clock)
  └── INT1 (IRQ) ────────────────► GPIO3  (RTC IO Mux Domain)

[PY32 PMIC]                    [ESP32-S3 Host]
  ├── Internal I2C Bus ──────────► Standard Control Path
  └── Main Button ───────────────► GPIO11 (Wake Source)
```

## 2. Power Rail Management & Initialization

### PMIC Low-Power State Transitions

To enforce microampere-level efficiency, the firmware must explicitly command the PY32 PMIC via low-level register abstractions (`M5PM1` library) to kill non-essential power rails prior to entering sleep states. `M5Unified` handles macro power configurations but **lacks** the structural granularity required for deep sleep gating.

C++

```
// Required Library Execution Sequence
#include <M5Unified.h>  // Must be initialized first for I2C allocation
#include <M5PM1.h>      // Provides low-level register access

M5PM1 pm1;

void initLowPowerHardware() {
    auto cfg = M5.config();
    M5.begin(cfg);
    pm1.begin(&M5.InI2C, M5PM1_DEFAULT_ADDR); // Bind to shared internal I2C bus
}
```

### Pre-Sleep Power Gating Routine

Before calling the host sleep instruction, the system must forcefully run the following sequence to isolate leakage:

1. **Indicator Suppression:** Call `pm1.setLedEnLevel(false);` to turn off the hardware-tied green status/charge LED.
    
2. **Bus Dissociation:** Execute `pm1.setBoostEnable(false);` and `pm1.setDcdcEnable(false);` to kill the 5V DCDC boost converters, completely powering down the LCD panel backlight, Grove bus, and internal HAT rails.
    
3. **Core Preservation:** Call `pm1.ldoSetPowerHold(true);` to preserve _only_ the low-current LDO supplying the ESP32-S3 RTC domain and the LSM6DSOX VDD rail.
    
4. **Co-Processor Idle:** Call `pm1.setI2cSleepTime(10);` to place the PY32 PMIC internal I2C interface into auto-sleep mode after 10ms of bus silence.
    

## 3. Sleep & Wakeup Configurations

### Pin Wakeup Capabilities (ESP32-S3 Validation)

- **GPIO3 Verification:** Fully qualified RTC IO pin. It resides within the ESP32-S3 revised `EXT0`/`EXT1` wake-capable hardware range (**GPIO0 to GPIO21**). It does _not_ follow legacy ESP32 restrictions.
    
- **Trigger Protocol:** Configured as `EXT0` active-high or active-low edge/level trigger matching the LSM6DSOX native interrupt latch output.
    

### IO Leakage Prevention (Crucial)

During Deep Sleep, GPIO43 (SDA) and GPIO44 (SCL) drop to high impedance ($Hi-Z$). Because the external LSM6DSOX board maintains active pull-up paths to its preserved power rail, current will silently bleed into the ESP32-S3's unpowered digital ESD protection diodes.

**Enforced Mitigation Sequence:**

C++

```
// 1. Delete the active master I2C driver to release pins cleanly
i2c_driver_delete(I2C_NUM_0); 

// 2. Isolate digital bus lines to physically block ESD diode current leakage
gpio_isolate(GPIO_NUM_43);
gpio_isolate(GPIO_NUM_44);

// 3. Configure the active RTC wakeup pins
esp_sleep_enable_ext0_wakeup(GPIO_NUM_3, 1);   // LSM6DSOX Hardware Interrupt
esp_sleep_enable_ext1_wakeup(1ULL << GPIO_NUM_11, ESP_EXT1_WAKEUP_ANY_LOW); // PMIC Main Button

// 4. Force immediate system Deep Sleep state
esp_deep_sleep_start();
```

## 4. Offloaded Fall Detection Logic (LSM6DSOX MLC)

- **Host Role:** The ESP32-S3 remains in Deep Sleep ($\approx 15\,\mu\text{A}$) during standard carry and locomotion.
    
- **Sensor Role:** Fall classification is handled entirely within the LSM6DSOX **Machine Learning Core (MLC)** running an embedded decision tree at $\approx 4\,\mu\text{A}$.
    
- **Interrupt Condition:** The host processor is woken up _only_ when the MLC outputs a verified positive classification match for a human fall sequence, entirely eliminating false wakeups from non-fall transient impacts.