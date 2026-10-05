// The Ethernet MAC and PHY, on the boards that have them -- see h5/eth.c.
#pragma once
#include <stdint.h>
#include <stdbool.h>

// Clocks, pins, the PHY and both DMA rings. NULL on success, or what failed.
const char *stm32_eth_start(const uint8_t mac[6]);
// 0 without a link, 10 or 100 with one; *full is set to the duplex.
uint32_t stm32_eth_link(bool *full);
bool stm32_eth_send(const uint8_t *frame, uint32_t len);
bool stm32_eth_receive(void (*deliver)(const uint8_t *frame, uint32_t len));

extern uint32_t stm32_eth_rx_frames, stm32_eth_rx_errors, stm32_eth_tx_frames, stm32_eth_tx_busy;
