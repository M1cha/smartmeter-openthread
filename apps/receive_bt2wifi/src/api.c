#include <stdio.h>
#include <zephyr/net/socket.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(main_api, CONFIG_APP_LOG_LEVEL);

#include "main.h"

struct apiserver {
	struct k_mutex lock;
	int socket;
	int client_socket;
};

static struct apiserver g_apiserver = { 0 };

static int send_exact(int sockfd, const void *buf_, size_t len)
{
	const uint8_t *buf = buf_;

	while (len) {
		ssize_t ret = send(sockfd, buf, len, 0);
		if (ret < 0) {
			if (errno == -EINTR) {
				continue;
			}
			if (errno == -EAGAIN || errno == -EWOULDBLOCK) {
				LOG_WRN("send: got wouldblock on blocking socket");
				return -errno;
			}

			LOG_ERR("send: socket error: %d", errno);

			return -errno;
		}
		if (ret == 0) {
			LOG_ERR("send: remote side closed");
			return -ECONNRESET;
		}

		buf += ret;
		len -= ret;
	}

	return 0;
}

static void process_client(struct apiserver *apiserver, int fd)
{
	LOG_INF("new connection");

	for (;;) {
		uint8_t byte;
		ssize_t ret = recv(fd, &byte, sizeof(byte), 0);
		if (ret < 0) {
			if (errno == -EINTR) {
				continue;
			}
			if (errno == -EAGAIN || errno == -EWOULDBLOCK) {
				LOG_WRN("recv: got wouldblock on blocking socket");
				return;
			}

			LOG_ERR("recv: socket error: %d", errno);
			return;
		}
		if (ret == 0) {
			LOG_INF("recv: remote side closed");
			return;
		}
	}
}

void main_api_send(const void *data, size_t len)
{
	if (len > UINT8_MAX) {
		LOG_WRN("data len %zu is to large for our frame format", len);
		return;
	}
	uint8_t lenu8 = len;

	// We need the lock to prevent the main thread from closing or
	// replacing the socket while we're attempting the two send calls.
	// We might get rescheduled in between the two sends.
	int ret = k_mutex_lock(&g_apiserver.lock, K_NO_WAIT);
	if (ret == -EAGAIN) {
		return;
	} else if (ret) {
		LOG_ERR("failed to lock mutex: %d", ret);
		return;
	}

	if (g_apiserver.client_socket < 0) {
		goto unlock;
	}

	ret = send_exact(g_apiserver.client_socket, &lenu8, sizeof(lenu8));
	if (ret) {
		LOG_WRN("failed to send length to client: %d", ret);
		goto unlock;
	}

	ret = send_exact(g_apiserver.client_socket, data, len);
	if (ret) {
		LOG_WRN("failed to data length to client: %d", ret);
		goto unlock;
	}

	LOG_INF("send data to client");

unlock:
	k_mutex_unlock(&g_apiserver.lock);
}

int main_api_init(void)
{
	int ret = k_mutex_init(&g_apiserver.lock);
	if (ret) {
		LOG_ERR("failed to init mutex");
		return ret;
	}

	int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd < 0) {
		LOG_ERR("failed to create socket %d", errno);
		return -errno;
	}

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
		.sin_port = htons(8888),
	};

	ret = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
	if (ret) {
		LOG_ERR("Failed to bind UDP socket %d", errno);
		close(fd);
		return -errno;
	}

	ret = listen(g_apiserver.socket, 2);
	if (ret < 0) {
		LOG_ERR("Failed to listen on TCP socket: %d", errno);
		close(fd);
		return -errno;
	}

	g_apiserver.socket = fd;
	g_apiserver.client_socket = -1;

	LOG_INF("API server started");

	return 0;
}

void main_api_run(void)
{
	for (;;) {
		LOG_DBG("wait for new connection");

		int client = accept(g_apiserver.socket, NULL, NULL);
		if (client < 0) {
			LOG_ERR("accept failed: %d", errno);
			break;
		}

		k_mutex_lock(&g_apiserver.lock, K_FOREVER);
		g_apiserver.client_socket = client;
		k_mutex_unlock(&g_apiserver.lock);

		process_client(&g_apiserver, client);

		k_mutex_lock(&g_apiserver.lock, K_FOREVER);
		g_apiserver.client_socket = -1;
		k_mutex_unlock(&g_apiserver.lock);

		int ret = shutdown(client, SHUT_RD);
		if (ret) {
			LOG_ERR("failed to shutdown client socket: %d", errno);
		}

		ret = close(client);
		if (ret) {
			LOG_ERR("failed to close client socket: %d", errno);
		}
	}
}
