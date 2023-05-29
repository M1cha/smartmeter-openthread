#include <stddef.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/types.h>

#include "main.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(receive_bt2wifi, CONFIG_APP_LOG_LEVEL);

#define CONFIG_TAGOIO_HTTP_WIFI_SSID "my-wifi"
#define CONFIG_TAGOIO_HTTP_WIFI_PSK "0123456789"

static void wifi_connect_cb(struct k_work *work);

static struct net_mgmt_event_callback mgmt_cb;
static struct wifi_connect_req_params wifi_connect_params = {
	.ssid = CONFIG_TAGOIO_HTTP_WIFI_SSID,
	.ssid_length = 0,
	.psk = CONFIG_TAGOIO_HTTP_WIFI_PSK,
	.psk_length = 0,
	.channel = 0,
	.security = WIFI_SECURITY_TYPE_PSK,
};
static K_WORK_DELAYABLE_DEFINE(wifi_connect_work, wifi_connect_cb);

static bool data_cb(struct bt_data *data, void *user_data)
{
	const bt_addr_le_t *addr = user_data;

	LOG_INF("AD type %u", data->type);
	LOG_HEXDUMP_DBG(data->data, data->data_len, "value");

	main_api_send(addr, data->data, data->data_len);

	return true;
}
static void scan_cb(const bt_addr_le_t *addr, int8_t rssi, uint8_t adv_type,
		    struct net_buf_simple *buf)
{
	if (adv_type != BT_GAP_ADV_TYPE_EXT_ADV) {
		return;
	}

	char le_addr[BT_ADDR_LE_STR_LEN];
	bt_addr_le_to_str(addr, le_addr, sizeof(le_addr));

	LOG_INF("[DEVICE]: %s, AD evt type %u, RSSI %i", le_addr, adv_type, rssi);
	bt_data_parse(buf, data_cb, (void *)addr);
}

static void wifi_connect(void)
{
	struct net_if *iface = net_if_get_default();

	int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &wifi_connect_params,
			   sizeof(wifi_connect_params));
	if (ret) {
		// the iface may not be online, yet
		LOG_ERR("wifi connect request failed: %d", ret);

		if (ret != -EALREADY) {
			k_work_schedule(&wifi_connect_work, K_SECONDS(1));
		}
		return;
	}

	LOG_INF("wifi connect request sent");
}

static void wifi_connect_cb(struct k_work *work)
{
	wifi_connect();
}

static void schedule_wifi_connect(void)
{
	k_work_schedule(&wifi_connect_work, K_SECONDS(10));
}

static void handle_wifi_connect_result(struct net_mgmt_event_callback *cb)
{
	const struct wifi_status *status = (const struct wifi_status *)cb->info;

	if (status->status) {
		LOG_ERR("event: connection request failed: %d", status->status);
		schedule_wifi_connect();
	} else {
		LOG_INF("event: wifi connected");
	}
}

static void handle_wifi_disconnect_result(struct net_mgmt_event_callback *cb)
{
	const struct wifi_status *status = (const struct wifi_status *)cb->info;

	LOG_INF("Disconnected. status: %d", status->status);
	schedule_wifi_connect();
}

static void wifi_mgmt_event_handler(struct net_mgmt_event_callback *cb, uint32_t mgmt_event,
				    struct net_if *iface)
{
	switch (mgmt_event) {
	case NET_EVENT_WIFI_CONNECT_RESULT:
		handle_wifi_connect_result(cb);
		break;
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		handle_wifi_disconnect_result(cb);
		break;
	default:
		break;
	}
}

static void wifi_init(void)
{
	net_mgmt_init_event_callback(&mgmt_cb, wifi_mgmt_event_handler,
				     NET_EVENT_WIFI_CONNECT_RESULT |
					     NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_add_event_callback(&mgmt_cb);

	wifi_connect_params.ssid_length = strlen(CONFIG_TAGOIO_HTTP_WIFI_SSID);
	wifi_connect_params.psk_length = strlen(CONFIG_TAGOIO_HTTP_WIFI_PSK);
}

void main(void)
{
	struct net_if *iface = net_if_get_default();
	while (!net_if_is_up(iface)) {
		k_sleep(K_MSEC(500));
	}
	LOG_INF("default interface is now up");

	wifi_init();
	wifi_connect();

	int ret = main_api_init();
	if (ret) {
		LOG_ERR("failed to init main API: %d", ret);
		return;
	}

	struct bt_le_scan_param scan_param = {
		.type = BT_HCI_LE_SCAN_PASSIVE,
		.options = BT_LE_SCAN_OPT_NONE,
		.interval = BT_GAP_SCAN_FAST_INTERVAL,
		.window = BT_GAP_SCAN_FAST_WINDOW,
	};

	LOG_INF("Starting Scanner/Advertiser Demo");

	/* Initialize the Bluetooth Subsystem */
	ret = bt_enable(NULL);
	if (ret) {
		LOG_ERR("Bluetooth init failed: %d", ret);
		return;
	}

	LOG_INF("Bluetooth initialized");

	ret = bt_le_scan_start(&scan_param, scan_cb);
	if (ret) {
		LOG_ERR("Starting scanning failed: %d", ret);
		return;
	}
}
