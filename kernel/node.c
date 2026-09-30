// What a node answers where a computer has a service.
//
// A node is the kernel with the parts that make it a computer left out: the
// file server, the volumes, the key store, the configuration read off a card.
// It keeps the scheduler, messages, drivers and the modules in flash, starts
// every program in its module image at boot, and starts nothing after -- the
// image is the whole configuration, as a task table is in FreeRTOS or ThreadX.
// See UBIQOS_NODE, and kernel/main.c for the start.
//
// The system calls that would have reached those parts stay, and are answered
// here: there is no file server, so every file call fails the way it fails on
// a computer before the server is up; the key store is empty; the host is
// called what the board header says. A module written for a node therefore
// runs unchanged on a computer, and one written for a computer that asks a
// node for a file is told no rather than faulting.
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "board.h"
#include "../common/ubiqos_abi.h"

#if !UBIQOS_NODE
#error "kernel/node.c is for the node profile only"
#endif

// No server: fs_request finds nobody to send to, and each file call answers -1.
int32_t ubiqos_fs_server_pid(void) { return -1; }

// No card to read a name from; the board names itself.
#ifndef UBIQOS_DEFAULT_HOSTNAME
#define UBIQOS_DEFAULT_HOSTNAME "ubiqos"
#endif
const char *ubiqos_config_hostname(void)    { return UBIQOS_DEFAULT_HOSTNAME; }
const char *ubiqos_config_ssid(void)        { return ""; }
bool        ubiqos_config_has_password(void) { return false; }
const char *ubiqos_config_credentials(void) { return 0; }

// No key store: nothing kept, nothing to unlock, nothing to match.
void           ubiqos_keys_init(void) { }
void           ubiqos_keys_lock(void) { }
uint32_t       ubiqos_keys_state(void) { return UBIQOS_KEYS_EMPTY; }
uint32_t       ubiqos_keys_count(void) { return 0; }
bool           ubiqos_keys_nth(uint32_t index, char *name_out, uint32_t *len_out)
               { (void)index; (void)name_out; (void)len_out; return false; }
const uint8_t *ubiqos_keys_value(const char *name, uint32_t *len_out)
               { (void)name; (void)len_out; return 0; }
bool           ubiqos_keys_match(const char *name, const uint8_t *value, uint32_t len)
               { (void)name; (void)value; (void)len; return false; }
bool           ubiqos_keys_derive(const char *label, uint8_t out[32])
               { (void)label; (void)out; return false; }
bool           ubiqos_keys_fingerprint(const char *name, uint32_t *out)
               { (void)name; (void)out; return false; }
