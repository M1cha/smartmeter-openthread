#include <smartmeter/mqttsndev.h>

#include <zephyr/kernel.h>
#include <zephyr/net/mqtt_sn.h>
#include <zephyr/net/conn_mgr_monitor.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/socket.h>
#include <zephyr/zvfs/eventfd.h>
#include <errno.h>

#ifdef CONFIG_WATCHDOG
#include <zephyr/drivers/watchdog.h>
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(mqttsndev, CONFIG_SMARTMETER_MQTTSN_DEVICE_LOG_LEVEL);

#include "private.h"

#define EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)

static struct net_mgmt_event_callback mgmt_cb;
static bool connected;
static bool started;

static K_THREAD_STACK_DEFINE(thread_stack, CONFIG_SMARTMETER_MQTTSN_DEVICE_STACK_SIZE);
static struct k_thread thread;

static struct mqtt_sn_client mqtt_client;
static struct mqtt_sn_transport_udp tp;
static uint8_t tx_buf[CONFIG_SMARTMETER_MQTTSN_DEVICE_BUFFER_SIZE];
static uint8_t rx_buf[CONFIG_SMARTMETER_MQTTSN_DEVICE_BUFFER_SIZE];
static bool mqtt_sn_connected;

static mqttsn_publish_callback_t publish_callback;
static int eventfd_publish = -1;

#ifdef CONFIG_WATCHDOG
static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static int wdt_channel_id = -1;
#endif

static void evt_cb(struct mqtt_sn_client *client, const struct mqtt_sn_evt *evt)
{
	switch (evt->type) {
	case MQTT_SN_EVT_CONNECTED: /* Connected to a gateway */
		LOG_INF("MQTT-SN event EVT_CONNECTED");
		mqtt_sn_connected = true;
		break;
	case MQTT_SN_EVT_DISCONNECTED: /* Disconnected */
		LOG_INF("MQTT-SN event EVT_DISCONNECTED");
		mqtt_sn_connected = false;
		break;
	case MQTT_SN_EVT_ASLEEP: /* Entered ASLEEP state */
		LOG_INF("MQTT-SN event EVT_ASLEEP");
		break;
	case MQTT_SN_EVT_AWAKE: /* Entered AWAKE state */
		LOG_INF("MQTT-SN event EVT_AWAKE");
		break;
	case MQTT_SN_EVT_PUBLISH: /* Received a PUBLISH message */
		LOG_INF("MQTT-SN event EVT_PUBLISH");
		LOG_HEXDUMP_INF(evt->param.publish.data.data, evt->param.publish.data.size,
				"Published data");
		break;
	case MQTT_SN_EVT_PINGRESP: /* Received a PINGRESP */
		LOG_INF("MQTT-SN event EVT_PINGRESP");
		break;
	}
}

static int do_work(void)
{
	int err;

	err = mqtt_sn_input(&mqtt_client);
	if (err < 0) {
		LOG_ERR("failed: input: %d", err);
		return err;
	}

	if (!mqtt_sn_connected) {
		return 0;
	}

	bool publish_requested = false;
	zvfs_eventfd_t value;
	err = zvfs_eventfd_read(eventfd_publish, &value);
	if (err < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
			LOG_ERR("failed to read eventfd: %d", errno);
			return -errno;
		}
	}
	else {
		publish_requested = true;
	}

	if (publish_callback && publish_requested) {
		err = publish_callback(&mqtt_client);
		if (err < 0) {
			LOG_ERR("failed: publish_callback: %d", err);
			return err;
		}
	}

	return 0;
}

static void run_mqtt_client(void)
{
	struct sockaddr_in6 gateway = {0};
	int err;

	gateway.sin6_family = AF_INET6;
	gateway.sin6_port = htons(mqttsndev_gateway_port);
	gateway.sin6_addr = mqttsndev_gateway_ip;

	err = mqtt_sn_transport_udp_init(&tp, (struct sockaddr *)&gateway, sizeof((gateway)));
	if (err) {
		LOG_ERR("mqtt_sn_transport_udp_init() failed %d", err);
		return;
	}

	struct mqtt_sn_data client_id = {
		.data = mqttsndev_client_id,
		.size = mqttsndev_client_id_length,
	};

	LOG_INF("Connecting client");
	err = mqtt_sn_client_init(&mqtt_client, &client_id, &tp.tp, evt_cb, tx_buf, sizeof(tx_buf),
				  rx_buf, sizeof(rx_buf));
	if (err) {
		LOG_ERR("mqtt_sn_client_init() failed %d", err);

		if (tp.tp.deinit) {
			tp.tp.deinit(&tp.tp);
		}

		return;
	}

	for (;;) {
		while (!mqtt_sn_connected) {
			LOG_INF("reconnect ...");

			err = mqtt_sn_connect(&mqtt_client, false, true);
			if (err) {
				LOG_ERR("mqtt_sn_connect() failed %d", err);
				goto out_deinit;
			}

			k_sleep(K_MSEC(500));

			err = mqtt_sn_input(&mqtt_client);
			if (err < 0) {
				LOG_ERR("failed: input: %d", err);
				goto out_deinit;
			}
		}

		LOG_DBG("Poll");

		struct zsock_pollfd fds[] = {
			{
				.fd = eventfd_publish,
				.events = ZSOCK_POLLIN,
			},
			{
				.fd = tp.sock,
				.events = ZSOCK_POLLIN,
			},
		};

		err = zsock_poll(fds, ARRAY_SIZE(fds), -1);
		if (err < 0) {
			LOG_ERR("Failed to poll: %d", err);
			goto out_deinit;
		}

		LOG_DBG("poll event: %d", err);

		err = do_work();
		if (err < 0) {
			LOG_ERR("do_work failed: %d", err);
			goto out_deinit;
		}
	}

out_deinit:
	mqtt_sn_client_deinit(&mqtt_client);
	mqtt_sn_connected = false;
}

static void thread_entry(void *p1, void*p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		LOG_INF("MQTT client started");

		run_mqtt_client();

		LOG_ERR("MQTT client stopped");
		k_sleep(K_SECONDS(CONFIG_SMARTMETER_MQTTSN_DEVICE_RECONNECT_WAIT_DURATION));
	}
}

static void start_thread(void)
{
	LOG_DBG("start thread");

	const k_tid_t tid = k_thread_create(
			&thread,
			thread_stack,
			K_THREAD_STACK_SIZEOF(thread_stack),
			thread_entry,
			NULL, NULL, NULL,
			CONFIG_SMARTMETER_MQTTSN_DEVICE_THREAD_PRIORITY,
			0,
			K_NO_WAIT);

	k_thread_start(tid);
}

#ifdef CONFIG_WATCHDOG
static void submit_watchdog_work(void);

static void watchdog_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!connected) {
		LOG_DBG("not feeding watchdog");
		return;
	}

	int ret = wdt_feed(wdt, wdt_channel_id);
	if (ret) {
		LOG_ERR("Feed failed: %d", ret);
	} else {
		LOG_DBG("Watchdog fed.");
	}

	submit_watchdog_work();
}
static K_WORK_DELAYABLE_DEFINE(watchdog_work, watchdog_work_handler);

static void submit_watchdog_work(void)
{
	int ret = k_work_schedule(&watchdog_work, K_MSEC(CONFIG_SMARTMETER_MQTTSN_DEVICE_WDT_FEED_INTERVAL_MS));
	if (ret < 0) {
		LOG_ERR("Can't schedule work: %d", ret);
	}
}

static int watchdog_init(void)
{
	const struct wdt_timeout_cfg wdt_config = {
		.flags = WDT_FLAG_RESET_SOC,
		.window = {
			.min = 0,
			.max = CONFIG_SMARTMETER_MQTTSN_DEVICE_WDT_MAX_WINDOW_MS,
		},
	};
	int ret;

	wdt_channel_id = wdt_install_timeout(wdt, &wdt_config);
	if (wdt_channel_id < 0) {
		LOG_ERR("Watchdog install error");
		return wdt_channel_id;
	}

	ret = wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
	if (ret < 0) {
		LOG_ERR("Watchdog setup error");
		return ret;
	}

	submit_watchdog_work();

	return 0;
}
SYS_INIT(watchdog_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEVICE);
#endif

static void net_event_handler(struct net_mgmt_event_callback *cb, uint32_t mgmt_event,
			      struct net_if *iface)
{
	int ret;

	if ((mgmt_event & EVENT_MASK) != mgmt_event) {
		return;
	}

	if (mgmt_event == NET_EVENT_L4_CONNECTED) {
		LOG_INF("Network connected");

		connected = true;
		if (!started) {
			started = true;
			start_thread();
		}

		ret = k_work_reschedule(&watchdog_work, K_NO_WAIT);
		if (ret < 0) {
			LOG_ERR("Can't reschedule watchdog work: %d", ret);
		}

		return;
	}

	if (mgmt_event == NET_EVENT_L4_DISCONNECTED) {
		LOG_INF("Network disconnected");
		connected = false;
		return;
	}
}

int mqttsndev_init(void)
{
	eventfd_publish = zvfs_eventfd(0, ZVFS_EFD_NONBLOCK);
	if (eventfd_publish < 0) {
		LOG_ERR("Failed to create eventfd: %d", eventfd_publish);
		return eventfd_publish;
	}

	if (IS_ENABLED(CONFIG_NET_CONNECTION_MANAGER)) {
		net_mgmt_init_event_callback(&mgmt_cb, net_event_handler, EVENT_MASK);
		net_mgmt_add_event_callback(&mgmt_cb);

		conn_mgr_mon_resend_status();
	} else {
		/* If the config library has not been configured to start the
		 * app only after we have a connection, then we can start
		 * it right away.
		 */
		start_thread();
	}

	return 0;
}

void mqttsndev_register_publish_callback(mqttsn_publish_callback_t callback) {
	publish_callback = callback;
}

void mqttsndev_schedule_publish_callback(void) {
	int ret = zvfs_eventfd_write(eventfd_publish, 1);
	if (ret< 0) {
		LOG_ERR("Failed to write to publish eventfd: %d", ret);
	}
}

__attribute__((__format__ (__printf__, 5, 6)))
int mqtt_sn_publish_fmt(struct mqtt_sn_client *client, enum mqtt_sn_qos qos,
		    struct mqtt_sn_data *topic_name, bool retain, const char *fmt, ...) {
	static char buffer[128];
	int ret;

	va_list ap;
	va_start(ap, fmt);
	ret = vsnprintf(buffer, sizeof(buffer), fmt, ap);
	va_end(ap);

	struct mqtt_sn_data pubdata = {
		.data = buffer,
		.size = MIN(sizeof(buffer), ret),
	};

	ret = mqtt_sn_publish(client, qos, topic_name, retain, &pubdata);
	if (ret) {
		LOG_ERR("failed to publish: %d", ret);
		return ret;
	}

	return 0;
}
