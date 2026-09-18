#ifndef UBIQOS_CONFIG_H
#define UBIQOS_CONFIG_H

#include <stdbool.h>

// /sd/config.txt: what this machine is called, and what network it belongs to.
//
// Read once, at boot, by the filesystem server as soon as the card is up and
// before /sd/startup runs. Nothing here is a system call: the values are the
// kernel's, and the file is the kernel's to read -- see the note on secrecy in
// config.c, which is the whole reason this is not simply a file like any other.

void ubiqos_config_read(void);

// True once the card has been tried, whether or not there was one and whether
// or not it held a file. Anything that needs the hostname waits for this rather
// than racing it: mDNS announces a name once, and announcing the wrong one and
// correcting it later is worse than starting a moment after the card.
bool ubiqos_config_done(void);
void ubiqos_config_give_up(void);   // no filesystem server: nothing is coming

// Never null. "ubiqos" when the file said nothing, which is what the machine
// has always been called.
const char *ubiqos_config_hostname(void);

// Empty when the file did not say. Empty is the question `wifi connect` asks;
// a value is the answer it does not have to.
const char *ubiqos_config_ssid(void);
bool ubiqos_config_has_password(void);

// "ssid\0password\0", the shape ubiqos_wifi_join wants, or null when the file
// did not give both. This is the only way the password leaves config.c, and it
// goes to the wifi service and nowhere else. The password itself is never
// returned on its own, because there is no caller that needs to see it.
const char *ubiqos_config_credentials(void);

// The board's own address on the USB cable, host byte order: 192.168.7.1
// unless the file said otherwise. The computer at the other end is offered the
// next one, by DHCP, on a /24 -- see kernel/lwipdhcpd.c.
#include <stdint.h>
uint32_t ubiqos_config_usb_address(void);

#endif
