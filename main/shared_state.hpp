#ifndef SHARED_STATE_HPP
#define SHARED_STATE_HPP

#include <stdint.h>

enum SystemState {
    STATE_PROVISIONING,
    STATE_IDLE,
    STATE_PRE_ALARM,
    STATE_ALARM_TRIGGERED,
    STATE_LOW_BATTERY,
    STATE_REMINDER,
    STATE_OTA_UPDATING,
    STATE_OTA_SUCCESS,
    STATE_OTA_FAILED,
    STATE_DEBUG_MENU
};

struct SystemParameters {
    uint32_t tempo_prem_sos_ms;
    uint32_t timeout_pre_allarme_ms;
    uint32_t timeout_promemoria_ms;
    uint32_t int_sincronizzazione_ms;
    float soglia_caduta_g;
    float soglia_vuoto_g;
    uint32_t durata_batt_min;
    uint32_t batt_scarica_pct;
    uint32_t batt_ripresa_pct;
    uint32_t batt_debole_pct;
    uint32_t durata_lowpwr_stato;
    uint32_t spegnimento_schermo_ms;
    uint32_t spegnimento_schermo_batt_ms;
    bool abilita_caduta;
    bool abilita_promemoria;
    bool abilita_risveglio_mov;
    bool abilita_beep_ui;
    uint32_t timeout_wifi_prov_ms;
    uint32_t tempo_prem_prov_ms;
    bool abilita_veglia_sonno;
    float base_x_g;
    float base_y_g;
    float base_tolleranza_g;
    char ora_inizio_veglia[6];
    char ora_fine_veglia[6];
    char ora_inizio_sonno[6];
    char ora_fine_sonno[6];
    uint32_t timeout_immobilita_min;
    float soglia_mov_sonno_g;
    uint32_t max_mov_sonno;
    uint32_t display_norm_lum;
    uint32_t display_dim_lum;
};

extern SystemParameters sys_params;
extern SystemState currentState;
extern bool wifi_connected;
extern char next_reminder_time[16];
extern char next_reminder_text[64];
extern bool is_screen_off;
extern bool ota_available;
extern char ota_url_target[256];
extern const char* ota_error_str;

#endif