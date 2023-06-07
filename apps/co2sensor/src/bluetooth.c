#include <stddef.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/types.h>

#include "main.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(main_bt, CONFIG_APP_LOG_LEVEL);

#define NONCE_SAVE_STEPS 1024

struct message {
	uint8_t nonce[12];
	uint16_t ciphertext[4];
	uint8_t tag[16];
} __packed;

struct mfg_data {
	uint8_t company_id[2];
	struct message message;
} __packed;

static const struct smr_cipher *g_cipher;
static struct bt_le_ext_adv *g_adv;
static struct smr_u128 g_nonce;

static struct mfg_data mfg_data = {
	.company_id = { 0xff, 0xff },
};
BUILD_ASSERT(sizeof(mfg_data) == 38);

static const struct bt_data ad[] = {
	BT_DATA(BT_DATA_MANUFACTURER_DATA, &mfg_data, sizeof(mfg_data)),
};

static void disable_advertising(struct k_work *work)
{
	int ret = bt_le_ext_adv_stop(g_adv);
	if (ret) {
		LOG_ERR("Advertising failed to stop (delayed): %d", ret);
		return;
	}
}
static K_WORK_DELAYABLE_DEFINE(disable_advertising_work, disable_advertising);

void app_bluetooth_send_data(uint16_t meterstatus, uint16_t alarmstatus, uint16_t outputstatus, uint16_t spaceco2)
{
	int ret;

	LOG_INF("Sending advertising data");

	if (!g_adv) {
		LOG_ERR("incomplete bluetooth initialization");
		return;
	}

	if (smr_u128_inc(&g_nonce)) {
		LOG_ERR("can't increment nonce anymore");
		return;
	}

	if (!smr_u128_has_rem(&g_nonce, NONCE_SAVE_STEPS)) {
		ret = settings_save_one("app_bt/nonce", &g_nonce, sizeof(g_nonce));
		if (ret) {
			LOG_ERR("failed to save nonce: %d", ret);
			return;
		}
		LOG_HEXDUMP_INF(&g_nonce, sizeof(g_nonce), "saved nonce");
	}

	struct message message = {
		.ciphertext = {
			// TODO: convert endianess
			meterstatus,
			alarmstatus,
			outputstatus,
			spaceco2
		},
	};

	if (smr_u128_to_nonce(message.nonce, sizeof(message.nonce), &g_nonce)) {
		LOG_ERR("nonce doesn't fit into u96 anymore");
		return;
	}

	ret = smr_encrypt(g_cipher, message.ciphertext, message.tag, message.nonce,
			  sizeof(message.ciphertext));
	if (ret) {
		LOG_ERR("failed to encrypt: %d", ret);
		return;
	}
	LOG_HEXDUMP_DBG(&message, sizeof(message), "ciphertext");

	mfg_data.message = message;

	ret = bt_le_ext_adv_set_data(g_adv, ad, ARRAY_SIZE(ad), NULL, 0);
	if (ret) {
		LOG_ERR("Failed to set advertising data for set: %d", ret);
		return;
	}

	ret = bt_le_ext_adv_start(g_adv, BT_LE_EXT_ADV_START_DEFAULT);
	if (ret && ret != -EALREADY) {
		LOG_ERR("Failed to start extended advertising set: %d", ret);
		return;
	}

	k_work_reschedule(&disable_advertising_work, K_SECONDS(2));
}

static int handle_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;
	int rc;

	if (settings_name_steq(name, "nonce", &next) && !next) {
		struct smr_u128 new_nonce;

		if (len != sizeof(new_nonce)) {
			return -EINVAL;
		}

		rc = read_cb(cb_arg, &new_nonce, sizeof(new_nonce));
		if (rc < 0) {
			return rc;
		}

		LOG_HEXDUMP_INF(&new_nonce, sizeof(new_nonce), "loaded nonce");
		g_nonce = new_nonce;
		return 0;
	}

	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(app_bt, "app_bt", NULL, handle_set, NULL, NULL);

int app_setup_bluetooth(const struct smr_cipher *cipher)
{
	int ret;

	g_cipher = cipher;

	ret = settings_subsys_init();
	if (ret) {
		LOG_ERR("failed to init settings subsys: %d", ret);
		return ret;
	}

	ret = settings_load();
	if (ret) {
		LOG_ERR("failed to load settings: %d", ret);
		return ret;
	}

	// skip the nonces we might hav already used but not saved
	if (smr_u128_add_u32(&g_nonce, NONCE_SAVE_STEPS)) {
		return -EINVAL;
	}

	ret = settings_save_one("app_bt/nonce", &g_nonce, sizeof(g_nonce));
	if (ret) {
		LOG_ERR("failed to save nonce: %d", ret);
		return ret;
	}

	LOG_INF("Starting Broadcaster");

	ret = bt_enable(NULL);
	if (ret) {
		LOG_ERR("Bluetooth init failed: %d", ret);
		return ret;
	}

	struct bt_le_adv_param adv_param = {
		.id = BT_ID_DEFAULT,
		.sid = 0U,
		.secondary_max_skip = 0U,
		.options = BT_LE_ADV_OPT_EXT_ADV | BT_LE_ADV_OPT_USE_IDENTITY | BT_LE_ADV_OPT_CODED,
		.interval_min = BT_GAP_ADV_SLOW_INT_MIN,
		.interval_max = BT_GAP_ADV_SLOW_INT_MAX,
		.peer = NULL,
	};
	ret = bt_le_ext_adv_create(&adv_param, NULL, &g_adv);
	if (ret) {
		LOG_ERR("Failed to create advertising set: %d", ret);
		return ret;
	}

	LOG_INF("Bluetooth initialized");

	return 0;
}
