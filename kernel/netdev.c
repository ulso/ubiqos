// Ethernet frames over USB, the device half.
//
// There is no IP stack behind this yet. That is deliberate and it is still
// worth having: a host brings the link up and starts talking the moment it
// enumerates -- ARP, IPv6 router solicitations, mDNS -- so counting what
// arrives proves the descriptor, the endpoints and the class driver all work
// before anything depends on them.
//
// When lwIP arrives it takes tud_network_recv_cb's frame instead of the
// counter, and tud_network_xmit_cb gets its pbuf. Nothing else here changes.
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "class/net/net_device.h"

void myrtos_print(const char *s);

uint32_t myrtos_net_rx_frames, myrtos_net_rx_bytes;
uint32_t myrtos_net_tx_frames, myrtos_net_dropped;
bool     myrtos_net_link_up;

// A frame from the host. Returning true means it has been taken; false means
// ask again later, which is how flow control is done here. Dropping is honest
// while there is nowhere to put it -- pretending to have taken it and then
// losing it would be the same thing with worse bookkeeping.
bool tud_network_recv_cb(const uint8_t *src, uint16_t size)
{
    (void)src;
    // A frame arriving is the only honest evidence that the link works, so it
    // is what the flag records. NCM has no link callback to ask -- see
    // tud_network_init_cb below -- and tud_network_can_xmit is about buffer
    // space, not the link, and has side effects besides.
    myrtos_net_link_up = true;
    myrtos_net_rx_frames++;
    myrtos_net_rx_bytes += size;
    myrtos_net_dropped++;
    tud_network_recv_renew();     // done with it; hand the buffer back
    return true;
}

// Copy one frame out. ref and arg are whatever was passed to tud_network_xmit,
// so a stack puts its own buffer there; with nothing sending, this is never
// called.
uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg)
{
    if (!ref || !arg) return 0;
    memcpy(dst, ref, arg);
    myrtos_net_tx_frames++;
    return arg;
}

// NEVER CALLED FOR NCM, and it took a counter reading "link down" beside 92
// received frames to notice. TinyUSB declares this in net_device.h for all
// three classes but only ecm_rndis_device.c calls it; the NCM driver's own
// notion of the link is ncm_interface.itf_data_alt, which is private to it.
//
// It stays because the header requires the symbol, and because a build that
// ever switches to ECM would want it.
void tud_network_init_cb(void)
{
    myrtos_net_link_up = true;
}
