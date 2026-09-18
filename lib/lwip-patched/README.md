# lwIP, with two files changed

The Pico SDK's lwIP is used as it ships, except for the mDNS responder's
`apps/mdns/mdns.c` and `apps/mdns/mdns_out.c`, copied here from SDK 2.3.1.
`CMakeLists.txt` builds the responder from these two and the SDK's own
`mdns_domain.c` instead of linking `pico_lwip_mdns`, so there is no patch step.
The build stops if the SDK version moves -- `ubiqosPatchedFrom`, shared with
`lib/tinyusb-patched`.

## What is changed, and why

A host without IPv6 answered nothing when asked for its AAAA record, and lwIP
says so itself: "Sending negative responses NSEC" is on its list of things left
to implement, with a `TODO return NSEC if unsupported protocol requested` where
the question is looked at. A querier asking for both A and AAAA -- which is what
`getaddrinfo` does, and so every `curl` and every browser -- then waits for its
own timeout before it believes there is no AAAA. On macOS, 18 Sep 2026:

| lookup of fruit-jam.local | time    |
|---------------------------|---------|
| A only                    | 0.04 s  |
| A and AAAA                | 5.01 s  |

RFC 6762 section 6.1 is how a responder says "this name has no record of that
type": an NSEC record for the name whose type bitmap lists the types it does
have. So, in a build without IPv6:

* `check_host` (mdns.c) marks an AAAA question for our own name with
  `REPLY_HOST_AAAA`, a flag nothing else uses when IPv6 is off;
* `mdns_create_outpacket` (mdns_out.c) answers it with `mdns_add_nsec_answer`:
  the name, type 47, TTL 120, and rdata of the name again and a bitmap with
  type A in it. The A record rides along as an additional record, as lwIP
  already does for an address question.

A legacy one-shot querier, one asking from a port other than 5353, is not
answered with it.

Every change is marked `UBIQOS:` in the source. To re-sync after an SDK
upgrade, copy the new files over these and put those marked blocks back.
