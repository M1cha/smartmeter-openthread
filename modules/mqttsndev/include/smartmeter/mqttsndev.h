#ifndef SMARTMETER_MQTTSNDEV_H_
#define SMARTMETER_MQTTSNDEV_H_

#include <zephyr/net/mqtt_sn.h>

typedef int (*mqttsn_publish_callback_t)(struct mqtt_sn_client*);

int mqttsndev_init(void);
void mqttsndev_register_publish_callback(mqttsn_publish_callback_t callback);
void mqttsndev_schedule_publish_callback(void);

int mqtt_sn_publish_fmt(struct mqtt_sn_client *client, enum mqtt_sn_qos qos,
		    struct mqtt_sn_data *topic_name, bool retain, const char *fmt, ...);

#endif /* SMARTMETER_MQTTSNDEV_H_ */
