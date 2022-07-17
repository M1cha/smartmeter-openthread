#ifndef LORA_HACK_H
#define LORA_HACK_H

#include <stdbool.h>
#include <stdint.h>

void lora_enter_receiver_mode(void);
void lora_enter_sleep_mode(void);
bool lora_in_sleep_mode(void);
int16_t lora_read_rssi(void);

#endif /* LORA_HACK_H */
