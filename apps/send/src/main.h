#ifndef MAIN_H
#define MAIN_H

#include <stdint.h>

struct app_data {
	float active_energy;
	float active_power;
	size_t num_samples;
	int64_t last_send;
};

void app_unrecoverable_error(void);
#ifdef CONFIG_LORA
int app_setup_lora(struct app_data *data, const struct smr_cipher *cipher,
		   const struct device *dev);
#endif
int app_setup_uart(struct app_data *data, const struct device *dev);

#endif /* MAIN_H */
