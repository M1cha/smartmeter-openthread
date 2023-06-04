#include <stddef.h>
#include <stdlib.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/types.h>

#ifdef CONFIG_USB_DEVICE_STACK
#include <zephyr/usb/usb_device.h>
#endif

#ifdef CONFIG_WIFI
#include <zephyr/net/wifi_mgmt.h>
#endif

#include "main.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(receive_bt2wifi, CONFIG_APP_LOG_LEVEL);

#ifdef CONFIG_WIFI
struct wifi_settings {
	uint8_t ssid[WIFI_SSID_MAX_LEN];
	uint8_t ssid_length;

	uint8_t psk[WIFI_PSK_MAX_LEN];
	uint8_t psk_length;

	uint8_t band;
	uint8_t channel;
	enum wifi_security_type security;
	enum wifi_mfp_options mfp;
	int timeout;
};

static void wifi_connect_cb(struct k_work *work);

static struct net_mgmt_event_callback mgmt_cb;

static struct wifi_settings g_tmp_settings = { 0 };

static uint8_t g_ssid[WIFI_SSID_MAX_LEN];
static uint8_t g_psk[WIFI_PSK_MAX_LEN];
static struct wifi_connect_req_params g_wifi_connect_params;
static bool g_wifi_connect_params_valid = false;

static K_WORK_DELAYABLE_DEFINE(wifi_connect_work, wifi_connect_cb);
#endif

static bool data_cb(struct bt_data *data, void *user_data)
{
	const bt_addr_le_t *addr = user_data;

	(void)(addr);

	LOG_INF("AD type %u", data->type);
	LOG_HEXDUMP_DBG(data->data, data->data_len, "value");

#ifdef CONFIG_WIFI
	main_api_send(addr, data->data, data->data_len);
#endif

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

#ifdef CONFIG_WIFI
static void wifi_connect(void)
{
	struct net_if *iface = net_if_get_default();

	if (!g_wifi_connect_params_valid) {
		LOG_ERR("no wifi settings available");
		return;
	}

	LOG_HEXDUMP_DBG(g_wifi_connect_params.ssid, g_wifi_connect_params.ssid_length, "ssid");
	//LOG_HEXDUMP_DBG(g_wifi_connect_params.psk, g_wifi_connect_params.psk_length, "psk");

	LOG_DBG("ssid_len=%zu psk_len=%zu band=%u channel=%u security=%u mfp=%u timeout=%d",
		g_wifi_connect_params.ssid_length, g_wifi_connect_params.psk_length,
		g_wifi_connect_params.band, g_wifi_connect_params.channel,
		g_wifi_connect_params.security, g_wifi_connect_params.mfp,
		g_wifi_connect_params.timeout);

	int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &g_wifi_connect_params,
			   sizeof(g_wifi_connect_params));
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
}

static void wifi_apply_settings(const struct wifi_settings *settings)
{
	memcpy(g_ssid, settings->ssid, settings->ssid_length);
	memcpy(g_psk, settings->psk, settings->psk_length);
	g_wifi_connect_params = (struct wifi_connect_req_params){
		.ssid = g_ssid,
		.ssid_length = settings->ssid_length,

		.psk = g_psk,
		.psk_length = settings->psk_length,

		.band = settings->band,
		.channel = settings->channel,
		.security = settings->security,
		.mfp = settings->mfp,
		.timeout = settings->timeout,
	};
	g_wifi_connect_params_valid = true;
}
#endif

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

static int handle_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;
	int rc;

	(void)(next);
	(void)(rc);

#ifdef CONFIG_WIFI
	if (settings_name_steq(name, "wifi", &next) && !next) {
		if (len != sizeof(g_tmp_settings)) {
			return -EINVAL;
		}

		rc = read_cb(cb_arg, &g_tmp_settings, sizeof(g_tmp_settings));
		if (rc < 0) {
			return rc;
		}

		if (g_tmp_settings.ssid_length > sizeof(g_tmp_settings.ssid) ||
		    g_tmp_settings.psk_length > sizeof(g_tmp_settings.psk)) {
			return -EINVAL;
		}

		LOG_INF("loaded wifi settings.");
		wifi_apply_settings(&g_tmp_settings);

		return 0;
	}
#endif

	return -ENOENT;
}
SETTINGS_STATIC_HANDLER_DEFINE(app_bt, "app", NULL, handle_set, NULL, NULL);

#ifdef CONFIG_WIFI
static int wifi_args_to_settings(size_t argc, char *argv[], struct wifi_settings *params)
{
	char *endptr;
	int idx = 1;

	if (argc < 1) {
		return -EINVAL;
	}

	/* SSID */
	const char *ssid = argv[0];
	size_t ssid_length = strlen(ssid);
	if (ssid_length > sizeof(params->ssid)) {
		return -EINVAL;
	}

	memcpy(params->ssid, ssid, ssid_length);
	params->ssid_length = ssid_length;

	/* channel (optional) */
	if ((idx < argc) && (strlen(argv[idx]) <= 3)) {
		params->channel = strtol(argv[idx], &endptr, 10);
		if (*endptr != '\0') {
			return -EINVAL;
		}

		if (params->channel == 0u) {
			params->channel = WIFI_CHANNEL_ANY;
		}

		idx++;
	} else {
		params->channel = WIFI_CHANNEL_ANY;
	}

	/* psk (optional) */
	if (idx < argc) {
		const char *psk = argv[idx];
		size_t psk_length = strlen(psk);
		if (psk_length > sizeof(params->psk)) {
			return -EINVAL;
		}

		memcpy(params->psk, psk, psk_length);
		params->psk_length = psk_length;

		params->security = WIFI_SECURITY_TYPE_PSK;
		idx++;

		/* security type (optional) */
		if (idx < argc) {
			unsigned int security = strtol(argv[idx], &endptr, 10);

			if (security <= WIFI_SECURITY_TYPE_MAX) {
				params->security = security;
			}
			idx++;
		}
	} else {
		params->security = WIFI_SECURITY_TYPE_NONE;
	}

	/* MFP (optional) */
	params->mfp = WIFI_MFP_OPTIONAL;
	if (idx < argc) {
		unsigned int mfp = strtol(argv[idx], &endptr, 10);

		if (mfp <= WIFI_MFP_REQUIRED) {
			params->mfp = mfp;
		}
		idx++;
	}

	return 0;
}

static int cmd_set_wifi(const struct shell *sh, size_t argc, char *argv[])
{
	if (wifi_args_to_settings(argc - 1, &argv[1], &g_tmp_settings)) {
		shell_help(sh);
		return -ENOEXEC;
	}

	int ret = settings_save_one("app/wifi", &g_tmp_settings, sizeof(g_tmp_settings));
	if (ret) {
		LOG_ERR("failed to save wifi settings: %d", ret);
		return ret;
	}

	shell_fprintf(sh, SHELL_NORMAL, "wifi settings stored\n");

	wifi_apply_settings(&g_tmp_settings);
	k_work_schedule(&wifi_connect_work, K_SECONDS(1));

	return 0;
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(app_commands,
#ifdef CONFIG_WIFI
			       SHELL_CMD(set_wifi, NULL,
					 "Set Wi-Fi credentials"
					 "\"<SSID>\"\n<channel number (optional), "
					 "0 means all>\n"
					 "<PSK (optional: valid only for secure SSIDs)>\n"
					 "<Security type (optional: valid only for secure SSIDs)>\n"
					 "0:None, 1:PSK, 2:PSK-256, 3:SAE\n"
					 "<MFP (optional): 0:Disable, 1:Optional, 2:Required",
					 cmd_set_wifi),
#endif
			       SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(app, &app_commands, "App commands", NULL);

void main(void)
{
	int ret;

#ifdef CONFIG_USB_DEVICE_STACK
	init_usb();
#endif

	LOG_INF("start main app");

	ret = settings_subsys_init();
	if (ret) {
		LOG_ERR("failed to init settings subsys: %d", ret);
		return;
	}

	ret = settings_load();
	if (ret) {
		LOG_ERR("failed to load settings: %d", ret);
		return;
	}

#ifdef CONFIG_WIFI
	LOG_INF("wait for wifi iface ...");

	struct net_if *iface = net_if_get_default();
	while (!net_if_is_up(iface)) {
		k_sleep(K_MSEC(500));
	}
	LOG_INF("default interface is now up");

	wifi_init();
	wifi_connect();

	ret = main_api_init();
	if (ret) {
		LOG_ERR("failed to init main API: %d", ret);
		return;
	}
#endif

	struct bt_le_scan_param scan_param = {
		.type = BT_HCI_LE_SCAN_PASSIVE,
		.options = BT_LE_SCAN_OPT_CODED | BT_LE_SCAN_OPT_NO_1M,
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
