#include <smartmeter-rust.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/random/rand32.h>

#include "lorahack.h"
#include "main.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(main_lora, CONFIG_APP_LOG_LEVEL);

static const struct device *lora_dev;
static struct app_data *g_appdata;
static const struct smr_cipher *g_cipher;

static struct k_poll_signal lora_tx_signal = K_POLL_SIGNAL_INITIALIZER(lora_tx_signal);
static struct k_poll_event lora_tx_event =
	K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_SIGNAL, K_POLL_MODE_NOTIFY_ONLY, &lora_tx_signal);
static struct k_work_poll lora_tx_work;

static bool lora_is_channel_free(const int16_t rssi_threshold, const uint32_t timeout_ms)
{
	const uint32_t timeout_cyc = k_ms_to_cyc_floor32(timeout_ms);
	bool ret = true;

	if (!lora_in_sleep_mode()) {
		// maybe a send is still in progess?
		return false;
	}

	lora_enter_receiver_mode();

	const uint32_t start_time = k_cycle_get_32();

	while (k_cycle_get_32() - start_time < timeout_cyc) {
		const int16_t rssi = lora_read_rssi();
		if (rssi > rssi_threshold) {
			ret = false;
			break;
		}
	}

	lora_enter_sleep_mode();

	return ret;
}

static void sender_work_handler(struct k_work *const work)
{
	struct k_work_delayable *const dwork = k_work_delayable_from_work(work);
	struct app_data *const data = g_appdata;
	const int64_t interval = CONFIG_APP_SEND_INTERVAL * 1000;
	int ret;
	k_timeout_t next_timeout;

	// check if we're allowed to send again
	const int64_t waited = k_uptime_get() - data->last_send;
	if (waited < interval) {
		const int64_t towait = interval - waited;
		LOG_DBG("duty-cycled. wait %lldms", (long long)towait);

		next_timeout = K_MSEC(towait);
		goto reschedule;
	}

	static struct {
		uint8_t nonce[12];
		float ciphertext[2];
		uint8_t tag[16];
	} message;
	BUILD_ASSERT(sizeof(message) == 36);
	memset(&message, 0, sizeof(message));

	ret = sys_csrand_get(message.nonce, sizeof(message.nonce));
	if (ret) {
		LOG_ERR("failed to generate nonce: %d", ret);

		next_timeout = K_MSEC(5000);
		goto reschedule;
	}

	// the encryption happens in place
	// NOTE: when `num_samples` is 0 this will result in NaN. That's useful
	//       so the receiver can see a difference between reachability and UART
	//       issues.
	// TODO: convert endianess
	message.ciphertext[0] = data->active_power / data->num_samples;
	message.ciphertext[1] = data->active_energy;

	ret = smr_encrypt(g_cipher, message.ciphertext, message.tag, message.nonce,
			  sizeof(message.ciphertext));
	if (ret) {
		LOG_ERR("failed to encrypt: %d", ret);

		next_timeout = K_MSEC(5000);
		goto reschedule;
	}

	LOG_HEXDUMP_DBG(&message, sizeof(message), "ciphertext");

	if (!lora_is_channel_free(-85, 1)) {
		LOG_WRN("channel is busy, don't send");

		next_timeout = K_MSEC(5000);
		goto reschedule;
	}

	data->last_send = k_uptime_get();
	data->active_power = 0;
	data->num_samples = 0;

	ret = k_work_poll_submit(&lora_tx_work, &lora_tx_event, 1, K_MSEC(10000));
	if (ret != 0) {
		LOG_ERR("Failed to submit uart rx work polling: %d", ret);
		app_unrecoverable_error();
		return;
	}

	ret = lora_send_async(lora_dev, (void *)&message, sizeof(message), &lora_tx_signal);
	if (ret < 0) {
		LOG_ERR("LoRa send failed: %d", ret);

		ret = k_work_poll_cancel(&lora_tx_work);
		if (ret) {
			LOG_WRN("can't cancel lora tx work: %d", ret);
		}

		next_timeout = K_MSEC(5000);
		goto reschedule;
	}

	// as soon as TX is done it'll schedule this work
	return;

reschedule:
	ret = k_work_schedule(dwork, next_timeout);
	if (ret < 0) {
		LOG_ERR("can't schedule work: %d", ret);
		app_unrecoverable_error();
	}
}
static K_WORK_DELAYABLE_DEFINE(sender_work, sender_work_handler);

static void lora_tx_work_handler(struct k_work *work)
{
	LOG_INF("successfully sent data");

	const int ret = k_work_schedule(&sender_work, K_NO_WAIT);
	if (ret < 0) {
		LOG_ERR("can't schedule work: %d", ret);
		app_unrecoverable_error();
	}
}

int app_setup_lora(struct app_data *const data, const struct smr_cipher *const cipher,
		   const struct device *const dev)
{
	if (!device_is_ready(dev)) {
		LOG_ERR("%s Device not ready", dev->name);
		return -EAGAIN;
	}
	lora_dev = dev;
	g_appdata = data;
	g_cipher = cipher;

	struct lora_modem_config config = {
		.frequency = 868300000,
		.bandwidth = BW_250_KHZ,
		.datarate = SF_7,
		.preamble_len = 8,
		.coding_rate = CR_4_5,
		.tx_power = -2,
		.tx = true,
	};
	int ret = lora_config(lora_dev, &config);
	if (ret < 0) {
		LOG_ERR("LoRa config failed: %d", ret);
		return ret;
	}

	// this prevents sending excessively in case of reset loops
	data->last_send = k_uptime_get();

	k_work_poll_init(&lora_tx_work, lora_tx_work_handler);

	ret = k_work_schedule(&sender_work, K_NO_WAIT);
	if (ret < 0) {
		LOG_ERR("can't schedule work: %d", ret);
		return ret;
	}

	return 0;
}
