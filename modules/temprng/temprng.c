#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>

LOG_MODULE_REGISTER(stm32_temp, LOG_LEVEL_ERR);

#if DT_HAS_COMPAT_STATUS_OKAY(st_temp_rng)
#define DT_DRV_COMPAT st_temp_rng
#else
#error "No compatible devicetree node found"
#endif

struct temprng_data {
	const struct device *adc;
	const struct adc_channel_cfg adc_cfg;
	struct adc_sequence adc_seq;
	struct k_mutex mutex;
	int16_t sample_buffer;
	int16_t raw; /* raw adc Sensor value */
};

static int entropy_temprng_get_entropy(const struct device *dev, uint8_t *buffer, uint16_t length)
{
	struct temprng_data *data = dev->data;
	struct adc_sequence *sp = &data->adc_seq;
	int ret = 0;

	k_mutex_lock(&data->mutex, K_FOREVER);

	while (length) {
		uint32_t num = 0;

		for (size_t i = 0; i < sizeof(num) * 8; i++) {
			ret = adc_read(data->adc, sp);
			if (ret != 0) {
				LOG_ERR("adc_read failed: %d", ret);
				goto unlock;
			}

			if (data->sample_buffer & 1) {
				num |= (1 << i);
			}
		}

		uint32_t checksum = crc32_ieee((void *)&num, sizeof(num));

		size_t tocopy = MIN(length, sizeof(checksum));
		memcpy(buffer, &checksum, tocopy);

		buffer += tocopy;
		length -= tocopy;
	}

unlock:
	k_mutex_unlock(&data->mutex);

	return ret;
}

static const struct entropy_driver_api temprng_driver_api = {
	.get_entropy = entropy_temprng_get_entropy,
};

static int temprng_init(const struct device *dev)
{
	struct temprng_data *data = dev->data;
	struct adc_sequence *asp = &data->adc_seq;
	int ret;

	k_mutex_init(&data->mutex);

	if (!device_is_ready(data->adc)) {
		LOG_ERR("Device %s is not ready", data->adc->name);
		return -ENODEV;
	}

	*asp = (struct adc_sequence){
		.channels = BIT(data->adc_cfg.channel_id),
		.buffer = &data->sample_buffer,
		.buffer_size = sizeof(data->sample_buffer),
		.resolution = 12U,
	};

	ret = adc_channel_setup(data->adc, &data->adc_cfg);
	if (ret) {
		LOG_DBG("Setup AIN%u got %d", data->adc_cfg.channel_id, ret);
		return ret;
	}

	return 0;
}

static struct temprng_data temprng_dev_data = {
	.adc = DEVICE_DT_GET(DT_INST_IO_CHANNELS_CTLR(0)),
	.adc_cfg = { .gain = ADC_GAIN_1,
		     .reference = ADC_REF_INTERNAL,
		     .acquisition_time = ADC_ACQ_TIME_MAX,
		     .channel_id = DT_INST_IO_CHANNELS_INPUT(0),
		     .differential = 0 },
};

DEVICE_DT_INST_DEFINE(0, temprng_init, NULL, &temprng_dev_data, NULL, POST_KERNEL,
		      CONFIG_ENTROPY_INIT_PRIORITY, &temprng_driver_api);
