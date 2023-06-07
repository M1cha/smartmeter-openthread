#include <smartmeter-rust.h>
#include <zephyr/device.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>

#ifdef CONFIG_USB_DEVICE_STACK
#include <zephyr/usb/usb_device.h>
#endif

#include "main.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(main, CONFIG_APP_LOG_LEVEL);

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

#ifdef CONFIG_USB_DEVICE_STACK
static void init_usb(void)
{
	const struct device *dev;
	int ret;

	dev = DEVICE_DT_GET_ONE(zephyr_cdc_acm_uart);
	if (!device_is_ready(dev)) {
		LOG_ERR("CDC ACM device not ready");
		return;
	}

	ret = usb_enable(NULL);
	if (ret != 0) {
		LOG_ERR("Failed to enable USB");
		return;
	}
}
#endif

void main(void)
{
	int ret;
	uint32_t smrrc;

#ifdef CONFIG_USB_DEVICE_STACK
	init_usb();
#endif

	// it's okay to store this on the stack because the cipher creates a copy
	uint8_t key[KEYSIZE];
	(void)(read_key);

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

	ret = app_setup_bluetooth(&cipher);
	if (ret) {
		LOG_ERR("failed to init bluetooth: %d", ret);
		app_unrecoverable_error();
		return;
	}

	ret = app_setup_uart();
	if (ret) {
		LOG_ERR("failed to init UART: %d", ret);
		app_unrecoverable_error();
		return;
	}
}
