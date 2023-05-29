#include <stddef.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/random/rand32.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/types.h>

#include "main.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(main_bt, CONFIG_APP_LOG_LEVEL);

struct message {
	uint8_t nonce[12];
	float ciphertext[2];
	uint8_t tag[16];
} __packed;

struct mfg_data {
	uint8_t company_id[2];
	struct message message;
} __packed;

static const struct smr_cipher *g_cipher;
static struct bt_le_ext_adv *g_adv;

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

void app_bluetooth_send_data(float active_energy, float active_power)
{
	int ret;

	LOG_INF("Sending advertising data");

	if (!g_adv) {
		LOG_ERR("incomplete bluetooth initialization");
		return;
	}

	struct message message = {
		.ciphertext = {
			// TODO: convert endianess
			active_power,
			active_energy,
		},
	};

	ret = sys_csrand_get(message.nonce, sizeof(message.nonce));
	if (ret) {
		LOG_ERR("failed to generate nonce: %d", ret);
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
	if (ret) {
		LOG_ERR("Failed to start extended advertising set: %d", ret);
		return;
	}

	k_work_schedule(&disable_advertising_work, K_SECONDS(2));
}

int app_setup_bluetooth(struct app_data *data, const struct smr_cipher *cipher)
{
	int ret;

	g_cipher = cipher;

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
