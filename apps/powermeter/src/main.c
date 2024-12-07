#include <smartmeter-rust.h>
#include <zephyr/device.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include "main.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(powermeter, CONFIG_APP_LOG_LEVEL);

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

int main(void)
{
	int ret;

	k_sleep(K_SECONDS(1));
	LOG_DBG("Init");

	settings_subsys_init();
	settings_load();

	mqttsndev_register_publish_callback(app_publish_callback);
	mqttsndev_init();

#ifdef CONFIG_SMARTMETER_RUST_LOGGER
	uint32_t smrrc = smr_init_logger(logger_sink);
	if (smrrc) {
		LOG_ERR("sml logger init failed: %u", smrrc);
		app_unrecoverable_error();
		return -1;
	}
#endif

	ret = app_setup_uart();
	if (ret) {
		LOG_ERR("failed to init UART: %d", ret);
		app_unrecoverable_error();
		return ret;
	}

	return 0;
}
