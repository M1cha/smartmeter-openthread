#ifndef MAIN_H
#define MAIN_H

#include <smartmeter/mqttsndev.h>

void app_unrecoverable_error(void);
int app_setup_uart(void);
int app_publish_callback(struct mqtt_sn_client*const client);

#endif /* MAIN_H */
