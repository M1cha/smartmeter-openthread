#include <stdio.h>
#include <zephyr/net/socket.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(main_api, CONFIG_APP_LOG_LEVEL);

#include "main.h"

struct apiserver {
	int socket;
	struct net_context *udp_ctx;
	uint8_t sendbuf[1024];
};

static struct apiserver g_apiserver = { 0 };

static void udp_sent(struct net_context *context, int status, void *user_data)
{
	ARG_UNUSED(context);
	ARG_UNUSED(status);
	ARG_UNUSED(user_data);

	LOG_INF("Message sent");
}

void main_api_send(const bt_addr_le_t *addr, const void *data, size_t data_len)
{
	struct net_buf_simple buf = {
		.data = &g_apiserver.sendbuf[0],
		.len = 0,
		.size = sizeof(g_apiserver.sendbuf),
		.__buf = &g_apiserver.sendbuf[0],
	};

	if (!g_apiserver.udp_ctx) {
		return;
	}

	if (net_buf_simple_tailroom(&buf) < sizeof(uint8_t)) {
		LOG_WRN("not enough space for address type");
		return;
	}
	net_buf_simple_add_u8(&buf, addr->type);

	if (net_buf_simple_tailroom(&buf) < sizeof(addr->a.val)) {
		LOG_WRN("not enough space for address");
		return;
	}
	net_buf_simple_add_mem(&buf, addr->a.val, sizeof(addr->a.val));

	if (net_buf_simple_tailroom(&buf) < data_len) {
		LOG_WRN("not enough space for address data");
		return;
	}
	net_buf_simple_add_mem(&buf, data, data_len);

	LOG_INF("send data to client");

	struct sockaddr_in sockaddr = {
		.sin_family = AF_INET,
		.sin_port = htons(8888),
	};
	inet_pton(AF_INET, "192.168.46.1", &sockaddr.sin_addr);

	int ret = net_context_sendto(g_apiserver.udp_ctx, buf.data, buf.len,
				     (const struct sockaddr *)&sockaddr, sizeof(sockaddr), udp_sent,
				     K_NO_WAIT, NULL);
	if (ret != buf.len) {
		LOG_WRN("failed to send: %d", ret);
		return;
	}
}

int main_api_init(void)
{
	struct net_if *iface = net_if_get_default();
	if (!iface) {
		LOG_ERR("no default interface");
		return -ENOENT;
	}

	int ret = net_context_get(AF_INET, SOCK_DGRAM, IPPROTO_UDP, &g_apiserver.udp_ctx);
	if (ret) {
		LOG_ERR("failed to create net context %d", ret);
		return ret;
	}

	net_context_set_iface(g_apiserver.udp_ctx, iface);

	LOG_INF("API client initialized");

	return 0;
}
