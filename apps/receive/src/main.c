// TODO: reboot on hardfault (watchdog?)

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/zephyr.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(lora_receive, CONFIG_APP_LOG_LEVEL);

#define DEFAULT_RADIO_NODE DT_ALIAS(lora0)
BUILD_ASSERT(DT_NODE_HAS_STATUS(DEFAULT_RADIO_NODE, okay), "No default LoRa radio specified in DT");

#define DEFAULT_UART_NODE DT_CHOSEN(app_uart)
BUILD_ASSERT(DT_NODE_HAS_COMPAT(DEFAULT_UART_NODE, zephyr_cdc_acm_uart),
	     "Console device is not ACM CDC UART device");

static uint8_t tx_rb_buf[CONFIG_APP_RINGBUF_SIZE];
static struct ring_buf tx_rb;

void app_unrecoverable_error(void)
{
	LOG_ERR("unrecoverable app error. wait a bit and reboot");
	k_sleep(K_MSEC(10000));
	LOG_ERR("Reboot now ...");
	sys_reboot(SYS_REBOOT_COLD);
}

static void interrupt_handler(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (uart_irq_tx_ready(dev)) {
			uint8_t buffer[64];
			int rb_len;
			int send_len;

			rb_len = ring_buf_get(&tx_rb, buffer, sizeof(buffer));
			if (!rb_len) {
				LOG_DBG("Ring buffer empty, disable TX IRQ");
				uart_irq_tx_disable(dev);
				continue;
			}

			send_len = uart_fifo_fill(dev, buffer, rb_len);
			if (send_len < rb_len) {
				LOG_ERR("Drop %d fill bytes", rb_len - send_len);
			}

			LOG_DBG("ringbuf -> tty fifo %d bytes", send_len);
		}
	}
}

static int setup_uart(const struct device *const dev)
{
	int ret;

	ring_buf_init(&tx_rb, sizeof(tx_rb_buf), tx_rb_buf);

	if (!device_is_ready(dev)) {
		LOG_ERR("%s Device not ready", dev->name);
		return -ENODEV;
	}

	ret = usb_enable(NULL);
	if (ret) {
		LOG_ERR("failed to enable usb: %d", ret);
		return ret;
	}

	LOG_INF("Wait for DTR");
	for (;;) {
		uint32_t dtr;
		uart_line_ctrl_get(dev, UART_LINE_CTRL_DTR, &dtr);
		if (dtr) {
			break;
		}

		/* Give CPU resources to low priority threads. */
		k_sleep(K_MSEC(100));
	}
	LOG_INF("DTR set");

	uart_irq_callback_set(dev, interrupt_handler);

	return 0;
}

/// Based on: https://en.wikipedia.org/wiki/Consistent_Overhead_Byte_Stuffing#Implementation
static int cobs_encode(void *const dst_, size_t dstlen, const void *const src, size_t srclen,
		       size_t *pwritten)
{
	uint8_t *const dst = dst_;

	uint8_t *encode = dst;
	uint8_t *codep = encode++;
	uint8_t code = 1;

	for (const uint8_t *byte = src; srclen--; ++byte) {
		// Byte not zero, write it
		if (*byte) {
			if (encode - dst >= dstlen) {
				return -1;
			}
			*encode++ = *byte;
			code++;
		}

		// Input is zero or block completed, restart
		if (!*byte || code == 0xff) {
			if (encode - dst >= dstlen) {
				return -1;
			}

			*codep = code;
			code = 1;
			codep = encode;

			if (!*byte || srclen) {
				encode++;
			}
		}
	}

	// Write final code value
	*codep = code;

	if (encode - dst >= dstlen) {
		return -1;
	}

	*encode++ = 0;

	*pwritten = encode - dst;
	return 0;
}

static void uart_write(const struct device *const dev, const void *data, size_t datalen)
{
	static uint8_t encoded[100];
	size_t encoded_len = 0;

	const int ret = cobs_encode(encoded, sizeof(encoded), data, datalen, &encoded_len);
	if (ret) {
		LOG_ERR("failed to cobs-encode: %d", ret);
		return;
	}
	LOG_HEXDUMP_INF(encoded, encoded_len, "encoded");

	const uint32_t rb_len = ring_buf_put(&tx_rb, encoded, encoded_len);
	if (rb_len < encoded_len) {
		LOG_ERR("Drop %u encoded bytes", encoded_len - rb_len);
	}

	LOG_DBG("tty fifo -> ringbuf %d bytes", rb_len);
	if (rb_len) {
		uart_irq_tx_enable(dev);
	}
}

static int setup_lora(const struct device *const dev)
{
	if (!device_is_ready(dev)) {
		LOG_ERR("%s Device not ready", dev->name);
		return -ENODEV;
	}

	struct lora_modem_config config = {
		.frequency = 868300000,
		.bandwidth = BW_250_KHZ,
		.datarate = SF_7,
		.preamble_len = 8,
		.coding_rate = CR_4_5,
		.tx_power = -4,
		.tx = false,
	};

	const int ret = lora_config(dev, &config);
	if (ret < 0) {
		LOG_ERR("LoRa config failed: %d", ret);
		return ret;
	}

	return 0;
}

void main(void)
{
	const struct device *const lora_dev = DEVICE_DT_GET(DEFAULT_RADIO_NODE);
	const struct device *const uart_dev = DEVICE_DT_GET(DEFAULT_UART_NODE);

	int ret;

	ret = setup_uart(uart_dev);
	if (ret) {
		LOG_ERR("failed to setup uart: %d", ret);
		app_unrecoverable_error();
		return;
	}

	ret = setup_lora(lora_dev);
	if (ret) {
		LOG_ERR("failed to setup lora: %d", ret);
		app_unrecoverable_error();
		return;
	}

	LOG_INF("start receiving");
	for (;;) {
		static uint8_t data[100];
		int16_t rssi;
		int8_t snr;
		const int len = lora_recv(lora_dev, data, sizeof(data), K_FOREVER, &rssi, &snr);
		if (len < 0) {
			LOG_ERR("LoRa receive failed");
			app_unrecoverable_error();
			return;
		}

		LOG_INF("Received data (RSSI:%ddBm, SNR:%ddBm)", rssi, snr);
		LOG_HEXDUMP_INF(data, len, "data");

		uart_write(uart_dev, data, len);
	}
}
