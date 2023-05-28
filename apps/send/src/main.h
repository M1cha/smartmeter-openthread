#ifndef MAIN_H
#define MAIN_H

#include <smartmeter-rust.h>
#include <stdint.h>
#include <zephyr/device.h>

struct app_data {
	float active_energy;
	float active_power;
	size_t num_samples;
	int64_t last_send;
};

void app_unrecoverable_error(void);
#ifdef CONFIG_BT
int app_setup_bluetooth(struct app_data *data, const struct smr_cipher *cipher);
void app_bluetooth_send_data(float active_energy, float active_power);
#endif
#ifdef CONFIG_LORA
int app_setup_lora(struct app_data *data, const struct smr_cipher *cipher,
		   const struct device *dev);
#endif
int app_setup_uart(struct app_data *data, const struct device *dev);

void app_uart_data_received(float active_energy, float active_power);

#endif /* MAIN_H */
