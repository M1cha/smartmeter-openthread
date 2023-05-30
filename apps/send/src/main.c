#include <smartmeter-rust.h>
#include <zephyr/device.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>

#include "main.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(main, CONFIG_APP_LOG_LEVEL);

#define DEFAULT_UART_NODE DT_CHOSEN(app_uart)
BUILD_ASSERT(DT_NODE_HAS_STATUS(DEFAULT_UART_NODE, okay), "No default UART specified in DT");

#ifdef CONFIG_LORA
#define DEFAULT_RADIO_NODE DT_ALIAS(lora0)
BUILD_ASSERT(DT_NODE_HAS_STATUS(DEFAULT_RADIO_NODE, okay), "No default LoRa radio specified in DT");
#endif

struct smr_cipher {
	uint8_t buf[32];
};

void app_unrecoverable_error(void)
{
	LOG_ERR("unrecoverable app error. wait a bit and reboot");
	k_sleep(K_MSEC(10000));
	LOG_ERR("Reboot now ...");
	sys_reboot(SYS_REBOOT_COLD);
}

#ifdef CONFIG_SMARTMETER_RUST_LOGGER
static uint32_t logger_sink(const enum smr_loglevel level, const void *const buf,
			    const uintptr_t len)
{
	static uint8_t logbuf[1000];
	static size_t logbuf_used = 0;
	static bool overflow = false;

	if (buf == NULL) {
		if (overflow) {
			LOG_WRN("**truncated**");
		} else {
			switch (level) {
			case smr_loglevel_error:
				LOG_ERR("%.*s", logbuf_used, logbuf);
				break;

			case smr_loglevel_warn:
				LOG_WRN("%.*s", logbuf_used, logbuf);
				break;

			case smr_loglevel_info:
				LOG_INF("%.*s", logbuf_used, logbuf);
				break;

			case smr_loglevel_debug:
				LOG_DBG("%.*s", logbuf_used, logbuf);
				break;

			case smr_loglevel_trace:
				LOG_DBG("TRACE - %.*s", logbuf_used, logbuf);
				break;

			default:
				break;
			}
		}

		overflow = false;
		logbuf_used = 0;
		return 0;
	}

	if (logbuf_used + len >= sizeof(logbuf)) {
		overflow = true;
		return 0;
	}

	memcpy(logbuf + logbuf_used, buf, len);
	logbuf_used += len;
	logbuf[logbuf_used] = 0;

	return 0;
}
#endif /* CONFIG_SMARTMETER_RUST_LOGGER */

#define KEYSIZE 32

static int read_key(uint8_t key[KEYSIZE])
{
	int ret;
	const struct flash_area *area;
	const size_t key_size = KEYSIZE;

	ret = flash_area_open(FIXED_PARTITION_ID(keys), &area);
	if (ret) {
		LOG_ERR("failed to open flash area: %d", ret);
		return ret;
	}

	if (area->fa_size < key_size) {
		LOG_ERR("partition has %zu bytes only", area->fa_size);
		ret = -1;
		goto err_close;
	}

	const uint32_t align = flash_area_align(area);
	if (key_size % align) {
		LOG_ERR("flash area needs unsupported alignment of %u bytes", (unsigned)align);
		ret = -1;
		goto err_close;
	}

	ret = flash_area_read(area, 0, key, key_size);
	if (ret) {
		LOG_ERR("failed to read key: %d", ret);
		goto err_close;
	}

	flash_area_close(area);
	return 0;

err_close:
	flash_area_close(area);
	return ret;
}

void app_uart_data_received(float active_energy, float active_power)
{
	ARG_UNUSED(active_energy);
	ARG_UNUSED(active_power);

#ifdef CONFIG_BT
	app_bluetooth_send_data(active_energy, active_power);
#endif
}

#ifdef CONFIG_APP_SEND_TEST_EVENTS
static struct k_work_delayable test_work;

static void test(struct k_work *work)
{
	LOG_INF("send test event");
	app_uart_data_received(1.0, 2.0);
	k_work_schedule(&test_work, K_SECONDS(5));
}
static K_WORK_DELAYABLE_DEFINE(test_work, test);
#endif

void main(void)
{
	static struct app_data data_ = { 0 };

	int ret;
	uint32_t smrrc;
	struct app_data *const data = &data_;

#ifdef CONFIG_SMARTMETER_RUST_LOGGER
	smrrc = smr_init_logger(logger_sink);
	if (smrrc) {
		LOG_ERR("sml logger init failed: %u", smrrc);
		app_unrecoverable_error();
		return;
	}
#endif

	// it's okay to store this on the stack because the cipher creates a copy
	uint8_t key[KEYSIZE];
	ret = read_key(key);
	if (ret) {
		LOG_ERR("failed to read key: %d", ret);
		app_unrecoverable_error();
		return;
	}

	LOG_INF("smr cipher size = %lu", smr_cipher_size());

	static struct smr_cipher cipher;
	smrrc = smr_cipher_create(&cipher, sizeof(cipher), key);
	if (smrrc) {
		LOG_ERR("can't create cipher: %u", smrrc);
		app_unrecoverable_error();
		return;
	}

#ifdef CONFIG_LORA
	ret = app_setup_lora(data, &cipher, DEVICE_DT_GET(DEFAULT_RADIO_NODE));
	if (ret) {
		LOG_ERR("failed to init LORA: %d", ret);
		app_unrecoverable_error();
		return;
	}
#endif

#ifdef CONFIG_BT
	ret = app_setup_bluetooth(data, &cipher);
	if (ret) {
		LOG_ERR("failed to init bluetooth: %d", ret);
		app_unrecoverable_error();
		return;
	}
#endif

	ret = app_setup_uart(data, DEVICE_DT_GET(DEFAULT_UART_NODE));
	if (ret) {
		LOG_ERR("failed to init UART: %d", ret);
		app_unrecoverable_error();
		return;
	}

#ifdef CONFIG_APP_SEND_TEST_EVENTS
	k_work_schedule(&test_work, K_SECONDS(5));
#endif
}
