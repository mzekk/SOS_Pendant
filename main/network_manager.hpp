#ifndef NETWORK_MANAGER_HPP
#define NETWORK_MANAGER_HPP

void network_init();
void network_reset_provisioning();

void wifi_pause();
void wifi_resume();
extern bool wifi_is_paused;

void send_emergency_email(const char* subject);
void send_pushover_alert(const char* message);
void log_status_to_google_sheets();
void log_custom_event(const char* custom_state);

#endif