#ifndef MAIN_H
#define MAIN_H

#include <zephyr/bluetooth/addr.h>

int main_api_init(void);
void main_api_send(const bt_addr_le_t *addr, const void *data, size_t len);

#endif /* MAIN_H */
