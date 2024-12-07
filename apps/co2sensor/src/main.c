/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <smartmeter/mqttsndev.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/modbus/modbus.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(co2sensor, CONFIG_APP_LOG_LEVEL);

#define DEFAULT_MODBUS_NODE DT_CHOSEN(app_modbus)
BUILD_ASSERT(DT_NODE_HAS_STATUS(DEFAULT_MODBUS_NODE, okay), "No default modbus specified in DT");

static uint16_t meterstatus;
static uint16_t alarmstatus;
static uint16_t outputstatus;
static uint16_t spaceco2;

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


static int publish_callback(struct mqtt_sn_client*const client) {
	static struct mqtt_sn_data topic_meterstatus = MQTT_SN_DATA_STRING_LITERAL("/meterstatus");
	static struct mqtt_sn_data topic_alarmstatus = MQTT_SN_DATA_STRING_LITERAL("/alarmstatus");
	static struct mqtt_sn_data topic_outputstatus = MQTT_SN_DATA_STRING_LITERAL("/outputstatus");
	static struct mqtt_sn_data topic_spaceco2 = MQTT_SN_DATA_STRING_LITERAL("/spaceco2");

	int ret;

	LOG_INF("Publish");

	ret = mqtt_sn_publish_fmt(client, MQTT_SN_QOS_0, &topic_meterstatus, false, "%u", meterstatus);
	if (ret) {
		return ret;
	}

	ret = mqtt_sn_publish_fmt(client, MQTT_SN_QOS_0, &topic_alarmstatus, false, "%u", alarmstatus);
	if (ret) {
		return ret;
	}

	ret = mqtt_sn_publish_fmt(client, MQTT_SN_QOS_0, &topic_outputstatus, false, "%u", outputstatus);
	if (ret) {
		return ret;
	}

	ret = mqtt_sn_publish_fmt(client, MQTT_SN_QOS_0, &topic_spaceco2, false, "%u", spaceco2);
	if (ret) {
		return ret;
	}

	return 0;
}

int main(void)
{
	int ret;

	LOG_DBG("Init");

	settings_subsys_init();
	settings_load();

	ret = init_modbus_client();
	if (ret) {
		LOG_ERR("Modbus RTU client initialization failed: %d", ret);
		return ret;
	}

	mqttsndev_register_publish_callback(publish_callback);
	mqttsndev_init();

	for (;; k_sleep(K_SECONDS(5))) {
		uint8_t node = 0xFE;
		uint16_t regs[4];

		ret = modbus_read_input_regs(client_iface, node, 0x0000, regs, ARRAY_SIZE(regs));
		if (ret) {
			LOG_ERR("can't read registers %d", ret);
			continue;
		}

		meterstatus = regs[0];
		alarmstatus = regs[1];
		outputstatus = regs[2];
		spaceco2 = regs[3];

		LOG_INF("meter=0x%04x alarm=0x%04x output=0x%04x co2=%u",
			meterstatus,
			alarmstatus,
			outputstatus,
			spaceco2);

		mqttsndev_schedule_publish_callback();
	}

	return 0;
}
