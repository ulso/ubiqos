// The Ethernet MAC and PHY on the NUCLEO-H563ZI -- see eth.c.
#pragma once
#include <stdint.h>
#include <stdbool.h>

// Clocks, pins, the PHY and both DMA rings. NULL on success, or what failed.
const char *h5_eth_start(const uint8_t mac[6]);
// 0 without a link, 10 or 100 with one; *full is set to the duplex.
uint32_t h5_eth_link(bool *full);
bool h5_eth_send(const uint8_t *frame, uint32_t len);
bool h5_eth_receive(void (*deliver)(const uint8_t *frame, uint32_t len));

extern uint32_t h5_eth_rx_frames, h5_eth_rx_errors, h5_eth_tx_frames, h5_eth_tx_busy;
