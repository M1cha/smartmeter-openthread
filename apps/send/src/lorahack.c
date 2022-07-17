/// @file
/// implements code that requires headers and compile-time options that are not
/// available in the public API exposed by Zephyr
///
/// This compilation unit is added into the `loramac-node` library.
#include "lorahack.h"

#include <sx1276-board.h>
#include <sx1276/sx1276.h>

void lora_enter_receiver_mode(void)
{
	uint8_t opMode = RF_OPMODE_RECEIVER;

	// Enable TCXO if operating mode different from SLEEP.
	SX1276SetBoardTcxo(true);
	SX1276SetAntSwLowPower(false);
	SX1276SetAntSw(opMode);
	SX1276Write(REG_OPMODE, (SX1276Read(REG_OPMODE) & RF_OPMODE_MASK) | opMode);
}

void lora_enter_sleep_mode(void)
{
	SX1276SetSleep();
}

bool lora_in_sleep_mode(void)
{
	return SX1276GetStatus() == RF_OPMODE_SLEEP;
}

int16_t lora_read_rssi(void)
{
	return SX1276ReadRssi(MODEM_LORA);
}
