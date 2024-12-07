#include <math.h>
#include <smartmeter-rust.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>

#include "main.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(powermeter_uart, CONFIG_APP_LOG_LEVEL);

struct smr_context {
	uint8_t buf[520];
};

#define RX_BUFFER_SIZE CONFIG_APP_UART_ASYNC_RX_BUFFER_SIZE
#define RX_BUFFER_NUM CONFIG_APP_UART_ASYNC_RX_NUM_BUFFERS
K_MEM_SLAB_DEFINE_STATIC(uart_async_rx_slab, RX_BUFFER_SIZE, RX_BUFFER_NUM, 4);

#define DEFAULT_UART_NODE DT_CHOSEN(app_uart)
BUILD_ASSERT(DT_NODE_HAS_STATUS(DEFAULT_UART_NODE, okay), "No default UART specified in DT");
static const struct device *const uart_dev = DEVICE_DT_GET(DEFAULT_UART_NODE);

static K_SEM_DEFINE(uart_rx_sem, 0, 1);
static struct k_poll_event uart_rx_event = K_POLL_EVENT_STATIC_INITIALIZER(
	K_POLL_TYPE_SEM_AVAILABLE, K_POLL_MODE_NOTIFY_ONLY, &uart_rx_sem, 0);
static struct k_work_poll uart_rx_work;

// NOTE: `RING_BUF_DECLARE` doesn't support static linkage
static uint8_t rx_rb_buf[CONFIG_APP_RINGBUF_SIZE];
static struct ring_buf rx_rb;

static float active_energy;
static float active_power;
static size_t num_samples;

static void schedule_startrx_work(void);

static void startrx_work_handler(struct k_work *work)
{
	void *buf;
	k_mem_slab_alloc(&uart_async_rx_slab, (void **)&buf, K_FOREVER);

	int ret = uart_rx_enable(uart_dev, buf, RX_BUFFER_SIZE, CONFIG_APP_UART_ASYNC_RX_TIMEOUT_US);
	if (ret < 0) {
		LOG_ERR("Failed to enable UART RX");
		k_mem_slab_free(&uart_async_rx_slab, buf);
		schedule_startrx_work();
	}
}
static K_WORK_DELAYABLE_DEFINE(startrx_work, startrx_work_handler);

static void schedule_startrx_work(void)
{
	int ret = k_work_schedule(&startrx_work, K_MSEC(CONFIG_APP_UART_ASYNC_RX_RETRY_TIMEOUT_MS));
	if (ret < 0) {
		LOG_ERR("Can't schedule work: %d", ret);
	}
}

static void uart_async_callback(const struct device *const dev, struct uart_event *const evt,
				void *const data)
{
	uint32_t written;
	void *buf;
	int rc;

	ARG_UNUSED(data);

	LOG_DBG("UART CB: evt=%d", evt->type);

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
		k_mem_slab_free(&uart_async_rx_slab, evt->data.rx_buf.buf);
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
	case UART_RX_DISABLED:
		LOG_ERR("RX disabled: %d", evt->data.rx_stop.reason);
		break;
	case UART_RX_STOPPED:
		LOG_ERR("RX stopped");
		schedule_startrx_work();
		break;
	default:
		break;
	}
}

static struct smr_context __aligned(8) smlctx;
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

int app_publish_callback(struct mqtt_sn_client*const client) {
	static struct mqtt_sn_data topic_active_power = MQTT_SN_DATA_STRING_LITERAL("/active_power");
	static struct mqtt_sn_data topic_active_energy = MQTT_SN_DATA_STRING_LITERAL("/active_energy");

	int ret;

	LOG_INF("Publish");

	ret = mqtt_sn_publish_fmt(client, MQTT_SN_QOS_0, &topic_active_power, false, "%f", (double)active_power / num_samples);
	if (ret) {
		return ret;
	}

	ret = mqtt_sn_publish_fmt(client, MQTT_SN_QOS_0, &topic_active_energy, false, "%f", (double)active_energy);
	if (ret) {
		return ret;
	}

	active_power = 0;
	num_samples = 0;

	return 0;
}

static void sml_data_cb(void *const user_data, const struct smr_callback_data *const cbdata)
{
	ARG_UNUSED(user_data);

	const float new_active_power =
		cbdata->active_power.value * pow(10.0, cbdata->active_power.scaler);
	const float new_active_energy =
		cbdata->active_energy.value * pow(10.0, cbdata->active_energy.scaler);

	// sum the data for average calculation
	active_power += new_active_power;
	num_samples++;

	active_energy = new_active_energy;

	LOG_DBG("got data: active_power=%llu*10^%d active_energy=%llu*10^%d",
		(unsigned long long)cbdata->active_power.value, (int)cbdata->active_power.scaler,
		(unsigned long long)cbdata->active_energy.value, (int)cbdata->active_energy.scaler);
	LOG_INF("power:%llu energy:%llu",
		(unsigned long long)(active_power / num_samples),
		(unsigned long long)active_energy);

	mqttsndev_schedule_publish_callback();
}

int app_setup_uart(void)
{
	int ret;

	if (!device_is_ready(uart_dev)) {
		LOG_ERR("%s Device not ready", uart_dev->name);
		return -EAGAIN;
	}

	ring_buf_init(&rx_rb, sizeof(rx_rb_buf), rx_rb_buf);

	/* Configure async UART callback */
	ret = uart_callback_set(uart_dev, uart_async_callback, NULL);
	if (ret < 0) {
		LOG_ERR("Failed to set UART callback");
		return ret;
	}

	schedule_startrx_work();
	LOG_INF("sml ctxsz = %lu", sml_ctxsz());

	const uint32_t smlrc = sml_init(&smlctx, sizeof(smlctx), NULL, sml_read_cb, sml_data_cb);
	if (smlrc) {
		LOG_ERR("sml init failed: %u", smlrc);
		return -1;
	}
	k_work_poll_init(&uart_rx_work, uart_rx_work_handler);
	uart_rx_work_handler(NULL);

	return 0;
}
