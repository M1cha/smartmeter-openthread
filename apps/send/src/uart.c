#include <math.h>
#include <smartmeter-rust.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>

#include "main.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(main_uart, CONFIG_APP_LOG_LEVEL);

struct smr_context {
	uint8_t buf[520];
};

#define RX_BUFFER_SIZE CONFIG_APP_UART_ASYNC_RX_BUFFER_SIZE
#define RX_BUFFER_NUM CONFIG_APP_UART_ASYNC_RX_NUM_BUFFERS
static K_MEM_SLAB_DEFINE(uart_async_rx_slab, RX_BUFFER_SIZE, RX_BUFFER_NUM, 1);

static K_SEM_DEFINE(uart_rx_sem, 0, 1);
static struct k_poll_event uart_rx_event = K_POLL_EVENT_STATIC_INITIALIZER(
	K_POLL_TYPE_SEM_AVAILABLE, K_POLL_MODE_NOTIFY_ONLY, &uart_rx_sem, 0);
static struct k_work_poll uart_rx_work;

// NOTE: `RING_BUF_DECLARE` doesn't support static linkage
static uint8_t rx_rb_buf[CONFIG_APP_RINGBUF_SIZE];
static struct ring_buf rx_rb;

static void uart_async_callback(const struct device *const dev, struct uart_event *const evt,
				void *const data)
{
	uint32_t written;
	void *buf;
	int rc;

	ARG_UNUSED(data);

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
		written = ring_buf_put(&rx_rb, evt->data.rx.buf + evt->data.rx.offset,
				       evt->data.rx.len);
		if (written != evt->data.rx.len) {
			LOG_WRN("Received bytes dropped from ring buf");
		}
		/* Notify upper layer that new data has arrived */
		k_sem_give(&uart_rx_sem);
		break;
	default:
		break;
	}
}

static struct smr_context smlctx;
static void uart_rx_work_handler(struct k_work *work)
{
	int ret;
	uint32_t smlrc;

	ARG_UNUSED(work);

	if (k_sem_take(&uart_rx_sem, K_NO_WAIT) != 0) {
		// if it's NULL, it's the initial call from the main function
		if (work) {
			LOG_WRN("spurious uart work handler call");
		}
		goto resubmit;
	}

	smlrc = sml_poll(&smlctx);
	if (smlrc) {
		LOG_ERR("sml poll failed: %u", smlrc);
		app_unrecoverable_error();
		return;
	}

resubmit:
	ret = k_work_poll_submit(&uart_rx_work, &uart_rx_event, 1, K_FOREVER);
	if (ret != 0) {
		LOG_ERR("Failed to submit uart rx work polling: %d", ret);
		app_unrecoverable_error();
		return;
	}
}

static uint32_t sml_read_cb(void *const buf, const uintptr_t max_length,
			    uintptr_t *const out_length)
{
	const uint32_t bytes_read = ring_buf_get(&rx_rb, buf, max_length);
	*out_length = bytes_read;
	return 0;
}

static void sml_data_cb(void *const data_, const struct smr_callback_data *const cbdata)
{
	struct app_data *const data = data_;

	const float active_power =
		cbdata->active_power.value * pow(10.0, cbdata->active_power.scaler);
	const float active_energy =
		cbdata->active_energy.value * pow(10.0, cbdata->active_energy.scaler);

	// sum the data for average calculation
	data->active_power += active_power;
	data->num_samples++;

	data->active_energy = active_energy;

	LOG_DBG("got data: active_power=%llu*10^%d active_energy=%llu*10^%d",
		(unsigned long long)cbdata->active_power.value, (int)cbdata->active_power.scaler,
		(unsigned long long)cbdata->active_energy.value, (int)cbdata->active_energy.scaler);
	LOG_INF("power:%llu energy:%llu",
		(unsigned long long)(data->active_power / data->num_samples),
		(unsigned long long)data->active_energy);
}

int app_setup_uart(struct app_data *const data, const struct device *const dev)
{
	int ret;
	void *buf;

	if (!device_is_ready(dev)) {
		LOG_ERR("%s Device not ready", dev->name);
		return -EAGAIN;
	}

	ring_buf_init(&rx_rb, sizeof(rx_rb_buf), rx_rb_buf);

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

	LOG_INF("sml ctxsz = %lu", sml_ctxsz());

	const uint32_t smlrc = sml_init(&smlctx, sizeof(smlctx), data, sml_read_cb, sml_data_cb);
	if (smlrc) {
		LOG_ERR("sml init failed: %u", smlrc);
		return -1;
	}
	k_work_poll_init(&uart_rx_work, uart_rx_work_handler);
	uart_rx_work_handler(NULL);

	return 0;
}
