#include <zephyr/net/net_ip.h>
#include <zephyr/shell/shell.h>
#include <zephyr/settings/settings.h>
#include <zephyr/net/socket.h>
#include <stdlib.h>

#include "private.h"

struct in6_addr mqttsndev_gateway_ip;
uint16_t mqttsndev_gateway_port;
uint8_t mqttsndev_client_id[CONFIG_SMARTMETER_MQTTSN_DEVICE_MAX_CLIENTID_LENGTH] = "ZEPHYR";
size_t mqttsndev_client_id_length = 6;

static int set(const char *const name, const size_t len,
                            const settings_read_cb read_cb, void *const cb_arg)
{
	const char *next;
	int ret;

	if (settings_name_steq(name, "gateway_ip", &next) && !next) {
		static struct in6_addr tmp;

		if (len != sizeof(tmp.s6_addr)) {
			return -EINVAL;
		}

		ret = read_cb(cb_arg, &tmp.s6_addr, sizeof(tmp.s6_addr));
		if (ret < 0) {
			return ret;
		}

		mqttsndev_gateway_ip = tmp;
		return 0;
	}

	if (settings_name_steq(name, "gateway_port", &next) && !next) {
		static uint16_t tmp;

		if (len != sizeof(tmp)) {
			return -EINVAL;
		}

		ret = read_cb(cb_arg, &tmp, sizeof(tmp));
		if (ret < 0) {
			return ret;
		}

		mqttsndev_gateway_port = tmp;
		return 0;
	}

	if (settings_name_steq(name, "client_id", &next) && !next) {
		static uint8_t tmp[sizeof(mqttsndev_client_id)];

		if (len > sizeof(tmp)) {
			return -EINVAL;
		}

		ret = read_cb(cb_arg, tmp, sizeof(tmp));
		if (ret < 0) {
			return ret;
		}

		memcpy(mqttsndev_client_id, tmp, len);
		mqttsndev_client_id_length = len;
		return 0;
	}

	return -ENOENT;
}
SETTINGS_STATIC_HANDLER_DEFINE(mqttsndev, "mqttsndev", NULL, set, NULL, NULL);

static int cmd_gateway_ip(const struct shell *const sh, const size_t argc, char **argv)
{
	int ret;
	static struct in6_addr tmp;

	if (argc == 1) {
		shell_print(sh, "gateway_ip");
		shell_hexdump(sh, mqttsndev_gateway_ip.s6_addr, sizeof(mqttsndev_gateway_ip.s6_addr));
		return 0;
	}
	if (argc != 2) {
		shell_print(sh, "Invalid arguments");
		return -EINVAL;
	}

	ret = zsock_inet_pton(AF_INET6, argv[1], &tmp.s6_addr);
	if (ret != 1) {
		shell_print(sh, "Invalid IPv6 address: %s: %d", argv[1], ret);
		return -EINVAL;
	}

	mqttsndev_gateway_ip = tmp;
	settings_save_one("mqttsndev/gateway_ip", mqttsndev_gateway_ip.s6_addr, sizeof(mqttsndev_gateway_ip.s6_addr));

	shell_print(sh, "new gateway_ip");
	shell_hexdump(sh, mqttsndev_gateway_ip.s6_addr, sizeof(mqttsndev_gateway_ip.s6_addr));

	return 0;
}

static int cmd_gateway_port(const struct shell *const sh, const size_t argc, char **argv)
{
	if (argc == 1) {
		shell_print(sh, "gateway_port: %u", mqttsndev_gateway_port);
		return 0;
	}
	if (argc != 2) {
		shell_print(sh, "Invalid arguments");
		return -EINVAL;
	}

	errno = 0;
	char *endptr = NULL;
	unsigned long ret = strtoul(argv[1], &endptr, 10);
	if (errno || endptr[0] != '\0' || ret > UINT16_MAX) {
		shell_print(sh, "Invalid port: %s", argv[1]);
		return -EINVAL;
	}

	mqttsndev_gateway_port = ret;
	settings_save_one("mqttsndev/gateway_port", &mqttsndev_gateway_port, sizeof(mqttsndev_gateway_port));

	shell_print(sh, "new gateway_port: %u", mqttsndev_gateway_port);

	return 0;
}

static int cmd_client_id(const struct shell *const sh, const size_t argc, char **argv)
{
	if (argc == 1) {
		shell_print(sh, "client_id: (length=%zu)", mqttsndev_client_id_length);
		shell_hexdump(sh, mqttsndev_client_id, mqttsndev_client_id_length);
		return 0;
	}
	if (argc != 2) {
		shell_print(sh, "Invalid arguments");
		return -EINVAL;
	}

	const char *const new_id = argv[1];
	const size_t new_id_length = strlen(new_id);

	if (new_id_length > sizeof(mqttsndev_client_id)) {
		shell_print(sh, "Name is longer than the max size of %zu: %s", sizeof(mqttsndev_client_id), new_id);
		return -EINVAL;
	}

	memcpy(mqttsndev_client_id, new_id, new_id_length);
	mqttsndev_client_id_length = new_id_length;

	settings_save_one("mqttsndev/client_id", &mqttsndev_client_id, mqttsndev_client_id_length);

	shell_print(sh, "new client_id: (length=%zu)", mqttsndev_client_id_length);
	shell_hexdump(sh, mqttsndev_client_id, mqttsndev_client_id_length);

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_mqttsndev,
		SHELL_CMD(gateway_ip, NULL, "Get/set gateway IP address.", cmd_gateway_ip),
		SHELL_CMD(gateway_port,   NULL, "Get/set gateway port.", cmd_gateway_port),
		SHELL_CMD(client_id,   NULL, "Get/set client ID.", cmd_client_id),
		SHELL_SUBCMD_SET_END
		);

SHELL_CMD_REGISTER(mqttsndev, &sub_mqttsndev,
		   "MQTTSN device commands", NULL);

