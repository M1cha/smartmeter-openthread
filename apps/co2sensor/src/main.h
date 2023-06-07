#ifndef MAIN_H
#define MAIN_H

#include <smartmeter-rust.h>
#include <stdint.h>
#include <zephyr/device.h>

void app_unrecoverable_error(void);
int app_setup_bluetooth(const struct smr_cipher *cipher);
void app_bluetooth_send_data(uint16_t meterstatus, uint16_t alarmstatus, uint16_t outputstatus, uint16_t spaceco2);
int app_setup_uart(void);

#endif /* MAIN_H */
