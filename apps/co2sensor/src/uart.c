#include <math.h>
#include <zephyr/modbus/modbus.h>

#include "main.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(main_uart, CONFIG_APP_LOG_LEVEL);

#define DEFAULT_MODBUS_NODE DT_CHOSEN(app_modbus)
BUILD_ASSERT(DT_NODE_HAS_STATUS(DEFAULT_MODBUS_NODE, okay), "No default modbus specified in DT");

static int client_iface;
static const struct modbus_iface_param client_param = {
	.mode = MODBUS_MODE_RTU,
	.rx_timeout = 50000,
	.serial = {
		.baud = 9600,
		.parity = UART_CFG_PARITY_NONE,
		.stop_bits_client = UART_CFG_STOP_BITS_2,
	},
};

static int init_modbus_client(void)
{
	const char iface_name[] = { DEVICE_DT_NAME(DEFAULT_MODBUS_NODE) };

	client_iface = modbus_iface_get_by_name(iface_name);

	return modbus_init_client(client_iface, client_param);
}

int app_setup_uart(void)
{
	int ret;

	ret = init_modbus_client();
	if (ret) {
		LOG_ERR("Modbus RTU client initialization failed: %d", ret);
		return ret;
	}

	for (;; k_sleep(K_SECONDS(5))) {
		uint8_t node = 0xFE;
		uint16_t regs[4];

		ret = modbus_read_input_regs(client_iface, node, 0x0000, regs, ARRAY_SIZE(regs));
		if (ret) {
			LOG_ERR("can't read registers %d", ret);
			continue;
		}

		uint16_t meterstatus = regs[0];
		uint16_t alarmstatus = regs[1];
		uint16_t outputstatus = regs[2];
		uint16_t spaceco2 = regs[3];

		LOG_INF("meter=0x%04x alarm=0x%04x output=0x%04x co2=%u",
			meterstatus,
			alarmstatus,
			outputstatus,
			spaceco2);

		app_bluetooth_send_data(meterstatus, alarmstatus, outputstatus, spaceco2);
	}

	return 0;
}
