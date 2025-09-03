/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <smartmeter/mqttsndev.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/settings/settings.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(pms5003, CONFIG_APP_LOG_LEVEL);

#define DEFAULT_UART_NODE DT_CHOSEN(app_pms5003)
BUILD_ASSERT(DT_NODE_HAS_STATUS(DEFAULT_UART_NODE, okay), "No default uart specified in DT");
static const struct device *const uart_dev = DEVICE_DT_GET(DEFAULT_UART_NODE);

static uint8_t raw_message[32];
static size_t raw_message_size;
static uint16_t current_channels[13];

static void start_rx(void) {
	static uint8_t rx_buf[32];
	int ret;

	ret = uart_rx_enable(uart_dev, rx_buf, sizeof(rx_buf),
			     1e7);
	if (ret) {
		LOG_ERR("Failed to start RX: %d", ret);
		return;
	}
}

static int parse_packet(struct net_buf_simple *const buf, uint16_t channels[13]) {
	if (buf->len != 32) {
		LOG_DBG("packet is %zu instead of 32 bytes", buf->len);
		return -EINVAL;
	}

	uint16_t checksum_calculated = 0;
	for (size_t i=0; i<30; i+= 1) {
		checksum_calculated += buf->data[i];
	}

	const uint16_t magic = net_buf_simple_pull_be16(buf);
	if (magic != 0x424D) {
		LOG_DBG("unexpected magic: 0x%04X", magic);
		return -EINVAL;
	}

	const uint16_t length = net_buf_simple_pull_be16(buf);
	if (length != 28) {
		LOG_DBG("unexpected length: %u", length);
		return -EINVAL;
	}

	for (size_t i=0; i<13; i+=1) {
		channels[i] = net_buf_simple_pull_be16(buf);
		LOG_DBG("channel %zu: %u", i, channels[i]);
	}

	const uint16_t checksum_received = net_buf_simple_pull_be16(buf);
	if (checksum_received != checksum_calculated) {
		LOG_DBG("unexpected checksum. received=0x%04X calculated=0x%04X", checksum_received, checksum_calculated);
		return -EINVAL;
	}

	return 0;
}

static void rx_work_handler(struct k_work *const work)
{
	ARG_UNUSED(work);

	static uint16_t channels[ARRAY_SIZE(current_channels)];
	int ret;
	struct net_buf_simple buf;

	net_buf_simple_init_with_data(&buf, raw_message, raw_message_size);

	LOG_HEXDUMP_DBG(raw_message, raw_message_size, "message");

	ret = parse_packet(&buf, channels);
	if (ret) {
		LOG_ERR("Failed to parse packet: %d", ret);
		goto out;
	}

	memcpy(current_channels, channels, sizeof(current_channels));

	mqttsndev_schedule_publish_callback();

out:
	start_rx();
}
static K_WORK_DEFINE(rx_work, rx_work_handler);

static void uart_callback(const struct device *const dev, struct uart_event *const event,
			  void *const user_data)
{
	ARG_UNUSED(user_data);

	static int stop_reason;
	int ret;

	__ASSERT_NO_MSG(dev == uart_dev);

	switch (event->type) {
	case UART_RX_RDY: {
		struct uart_event_rx *const rx_event = &event->data.rx;

		LOG_DBG("RX data ready");

		memcpy(
			raw_message, &rx_event->buf[rx_event->offset], rx_event->len);
		raw_message_size = rx_event->len;

		ret = uart_rx_disable(dev);
		if (ret < 0) {
			LOG_ERR("Failed to disable RX: %d", ret);
		}

		break;
	};

	case UART_RX_STOPPED:
		if (event->data.rx_stop.reason != 0) {
			LOG_ERR("RX error: %d", event->data.rx_stop.reason);
		}
		stop_reason = event->data.rx_stop.reason;

		break;

	case UART_RX_DISABLED:
		LOG_DBG("RX disabled");

		if (stop_reason == 0) {
			ret = k_work_submit(&rx_work);
			if (ret < 0) {
				LOG_ERR("Failed to submit work: %d", ret);
			}
		} else {
			start_rx();
		}

		break;

	default:
		break;
	}
}

static int publish_callback(struct mqtt_sn_client*const client) {
	static struct mqtt_sn_data topics[] = {
		MQTT_SN_DATA_STRING_LITERAL("/pm1.0_std"),
		MQTT_SN_DATA_STRING_LITERAL("/pm2.5_std"),
		MQTT_SN_DATA_STRING_LITERAL("/pm10.0_std"),
		MQTT_SN_DATA_STRING_LITERAL("/pm1.0_env"),
		MQTT_SN_DATA_STRING_LITERAL("/pm2.5_env"),
		MQTT_SN_DATA_STRING_LITERAL("/pm10.0_env"),
		MQTT_SN_DATA_STRING_LITERAL("/particles_0.3"),
		MQTT_SN_DATA_STRING_LITERAL("/particles_0.5"),
		MQTT_SN_DATA_STRING_LITERAL("/particles_1.0"),
		MQTT_SN_DATA_STRING_LITERAL("/particles_2.5"),
		MQTT_SN_DATA_STRING_LITERAL("/particles_5.0"),
		MQTT_SN_DATA_STRING_LITERAL("/particles_10.0"),
		MQTT_SN_DATA_STRING_LITERAL("/reserved"),
	};

	int ret;

	LOG_INF("Publish");

	for(size_t index = 0; index < ARRAY_SIZE(topics); index += 1) {
		ret = mqtt_sn_publish_fmt(client, MQTT_SN_QOS_0, &topics[index], false, "%u", current_channels[index]);
		if (ret) {
			LOG_ERR("Failed to publish topic=%zu: %d", index, ret);
			return ret;
		}
	}


	return 0;
}

int main(void)
{
	int ret;

	LOG_DBG("Init");

	settings_subsys_init();
	settings_load();

	mqttsndev_register_publish_callback(publish_callback);
	mqttsndev_init();

	ret = uart_callback_set(uart_dev, uart_callback, NULL);
	if (ret) {
		LOG_ERR("Failed to set UART driver callback: %d", ret);
		return ret;
	}

	start_rx();

	return 0;
}
