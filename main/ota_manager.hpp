#ifndef OTA_MANAGER_HPP
#define OTA_MANAGER_HPP

void start_ota_update(const char* url);
void ota_mark_success();
void ota_force_rollback();

#endif