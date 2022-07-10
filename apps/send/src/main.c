/*
 * Copyright (c) 2019 Manivannan Sadhasivam
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <math.h>
#include <mbedtls/bignum.h>
#include <mbedtls/chachapoly.h>
#include <smartmeter-rust.h>
#include <zephyr/device.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/util.h>
#include <zephyr/zephyr.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(lora_send, CONFIG_APP_LOG_LEVEL);

#define DEFAULT_RADIO_NODE DT_ALIAS(lora0)
BUILD_ASSERT(DT_NODE_HAS_STATUS(DEFAULT_RADIO_NODE, okay), "No default LoRa radio specified in DT");

#define DEFAULT_UART_NODE DT_NODELABEL(usart1)
BUILD_ASSERT(DT_NODE_HAS_STATUS(DEFAULT_UART_NODE, okay), "No default UART radio specified in DT");

#define RX_BUFFER_SIZE CONFIG_APP_UART_ASYNC_RX_BUFFER_SIZE
#define RX_BUFFER_NUM CONFIG_APP_UART_ASYNC_RX_NUM_BUFFERS
static K_MEM_SLAB_DEFINE(uart_async_rx_slab, RX_BUFFER_SIZE, RX_BUFFER_NUM, 1);

static K_THREAD_STACK_DEFINE(workqueue_stack, CONFIG_APP_WORKQUEUE_STACK_SIZE);
static struct k_work_q workqueue;

struct smr_context {
	uint8_t buf[512];
};

struct app_data {
	/* ring buffer char buffer */
	uint8_t rx_rb_buf[CONFIG_APP_RINGBUF_SIZE];

	/* ring buffer */
	struct ring_buf rx_rb;

	/* rx semaphore */
	struct k_sem rx_sem;

	float energy;
	float power;
	size_t num_samples;
	int64_t last_send;
};

static struct app_data *g_appdata;

static void uart_async_callback(const struct device *dev, struct uart_event *evt, void *data_)
{
	struct app_data *data = data_;
	uint32_t written;
	void *buf;
	int rc;

	switch (evt->type) {
	case UART_RX_BUF_REQUEST:
		/* Allocate next RX buffer for UART driver */
		rc = k_mem_slab_alloc(&uart_async_rx_slab, (void **)&buf, K_NO_WAIT);
		if (rc < 0) {
			/* Major problems, UART_RX_BUF_RELEASED event is not being generated, or
			 * CONFIG_MODEM_IFACE_UART_ASYNC_RX_NUM_BUFFERS is not large enough.
			 */
			LOG_ERR("RX buffer starvation");
			break;
		}
		/* Provide the buffer to the UART driver */
		uart_rx_buf_rsp(dev, buf, RX_BUFFER_SIZE);
		break;
	case UART_RX_BUF_RELEASED:
		/* UART driver is done with memory, free it */
		k_mem_slab_free(&uart_async_rx_slab, (void **)&evt->data.rx_buf.buf);
		break;
	case UART_RX_RDY:
		/* Place received data on the ring buffer */
		written = ring_buf_put(&data->rx_rb, evt->data.rx.buf + evt->data.rx.offset,
				       evt->data.rx.len);
		if (written != evt->data.rx.len) {
			LOG_WRN("Received bytes dropped from ring buf");
		}
		/* Notify upper layer that new data has arrived */
		k_sem_give(&data->rx_sem);
		break;
	default:
		break;
	}
}

static int setup_uart(struct app_data *data)
{
	const struct device *dev = DEVICE_DT_GET(DEFAULT_UART_NODE);
	int ret;
	void *buf;

	if (!device_is_ready(dev)) {
		LOG_ERR("%s Device not ready", dev->name);
		return -EAGAIN;
	}

	ring_buf_init(&data->rx_rb, sizeof(data->rx_rb_buf), data->rx_rb_buf);
	k_sem_init(&data->rx_sem, 0, 1);

	/* Configure async UART callback */
	ret = uart_callback_set(dev, uart_async_callback, data);
	if (ret < 0) {
		LOG_ERR("Failed to set UART callback");
		return ret;
	}

	/* Enable reception permanently on the interface */
	k_mem_slab_alloc(&uart_async_rx_slab, (void **)&buf, K_FOREVER);
	ret = uart_rx_enable(dev, buf, RX_BUFFER_SIZE, CONFIG_APP_UART_ASYNC_RX_TIMEOUT_US);
	if (ret < 0) {
		LOG_ERR("Failed to enable UART RX");
		return ret;
	}

	return 0;
}

static void update_values(struct app_data *const data, const float energy, const float power)
{
	data->energy = energy;

	// sum the data for average calculation
	data->power += power;
	data->num_samples++;

	LOG_INF("energy:%llu power:%llu", (unsigned long long)data->energy,
		(unsigned long long)(data->power / data->num_samples));
}

static void handle_lorasend(struct k_work *const work)
{
	int ret;
	struct app_data *const data = g_appdata;

	LOG_INF("send lora message");

	// TODO: use `sys_csrand_get` to not kill flash
	mbedtls_mpi nonce;
	mbedtls_mpi_init(&nonce);

	ret = mbedtls_mpi_lset(&nonce, 1);
	if (ret) {
		LOG_ERR("failed to init nonce: %d", ret);
		return;
	}

	// TODO: load key from partition
	const uint8_t key[32];
	float plaintext[] = {
		data->energy,
		data->power,
	};
	struct {
		uint8_t nonce[12];
		uint8_t ciphertext[sizeof(plaintext)];
		uint8_t tag[16];
	} message;
	BUILD_ASSERT(sizeof(message) == 36);

	ret = mbedtls_mpi_write_binary(&nonce, message.nonce, sizeof(message.nonce));
	if (ret) {
		LOG_ERR("failed to write nonce to binary buffer: %d", ret);
		return;
	}

	mbedtls_chachapoly_context ctx;
	mbedtls_chachapoly_init(&ctx);

	ret = mbedtls_chachapoly_setkey(&ctx, key);
	if (ret) {
		LOG_ERR("failed to set key: %d", ret);
		return;
	}

	ret = mbedtls_chachapoly_encrypt_and_tag(&ctx, sizeof(plaintext), message.nonce, NULL, 0,
						 (const void *)plaintext, message.ciphertext,
						 message.tag);
	if (ret) {
		LOG_ERR("failed to encrypt: %d", ret);
		return;
	}

	mbedtls_chachapoly_free(&ctx);

	LOG_HEXDUMP_INF(&message, sizeof(message), "encrypted message");

	ret = mbedtls_mpi_add_int(&nonce, &nonce, 1);
	if (ret) {
		LOG_ERR("can't increment nonce");
		return;
	}

	// TODO: listen before talk
	// TODO: send

	data->last_send = k_uptime_get();
}
K_WORK_DEFINE(work_lorasend, handle_lorasend);

static uint32_t sml_read_cb(void *buf, uintptr_t max_length, uintptr_t *out_length)
{
	uint32_t bytes_read = ring_buf_get(&g_appdata->rx_rb, buf, max_length);
	*out_length = bytes_read;
	return 0;
}

static void sml_data_cb(const struct smr_callback_data *data)
{
	LOG_INF("got data: active_power=%llu*10^%d active_energy=%llu*10^%d",
		(unsigned long long)data->active_power.value, (int)data->active_power.scaler,
		(unsigned long long)data->active_energy.value, (int)data->active_energy.scaler);
}

#ifdef CONFIG_SMARTMETER_RUST_LOGGER
static uint32_t logger_sink(enum smr_loglevel level, const void *buf, uintptr_t len)
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
				LOG_ERR("%.*s", logbuf_used, log_strdup(logbuf));
				break;

			case smr_loglevel_warn:
				LOG_WRN("%.*s", logbuf_used, log_strdup(logbuf));
				break;

			case smr_loglevel_info:
				LOG_INF("%.*s", logbuf_used, log_strdup(logbuf));
				break;

			case smr_loglevel_debug:
				LOG_DBG("%.*s", logbuf_used, log_strdup(logbuf));
				break;

			case smr_loglevel_trace:
				LOG_DBG("TRACE - %.*s", logbuf_used, log_strdup(logbuf));
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

void main(void)
{
	static struct app_data data_ = { 0 };
	static struct smr_context smlctx;

	const struct device *const lora_dev = DEVICE_DT_GET(DEFAULT_RADIO_NODE);
	struct lora_modem_config config;
	int ret;
	struct app_data *const data = &data_;
	uint32_t smlrc;

	(void)(config);
	(void)(lora_dev);
	(void)(update_values);

	g_appdata = data;

	LOG_INF("ctxsz = %lu", sml_ctxsz());

#ifdef CONFIG_SMARTMETER_RUST_LOGGER
	smlrc = smr_init_logger(logger_sink);
	if (smlrc) {
		LOG_ERR("sml logger init failed: %u", smlrc);
		return;
	}
#endif

	smlrc = sml_init(&smlctx, sizeof(smlctx), sml_read_cb, sml_data_cb);
	if (smlrc) {
		LOG_ERR("sml init failed: %u", smlrc);
		return;
	}

	const struct k_work_queue_config cfg = {
		.name = "appworkq",
		.no_yield = false,
	};
	k_work_queue_start(&workqueue, workqueue_stack, K_KERNEL_STACK_SIZEOF(workqueue_stack),
			   CONFIG_APP_WORKQUEUE_PRIORITY, &cfg);

	// this prevents sending excessively in case of reset loops
	//data->last_send = k_uptime_get();

	ret = setup_uart(data);
	if (ret) {
		LOG_ERR("failed to init UART: %d", ret);
		return;
	}

	for (;;) {
		smlrc = sml_poll(&smlctx);
		if (smlrc) {
			LOG_ERR("sml poll failed: %u", smlrc);
			return;
		}
		k_sem_take(&data->rx_sem, K_FOREVER);
	}
}
