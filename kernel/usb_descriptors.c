#include "tusb.h"

// USB descriptors for UbiqOS. Two functions on one device: a CDC-ACM serial
// port, which replaced the FTDI cable on GP44, and a mass storage device that
// hands the SD card to the host.
//
// Both are declared always, even though the card is only shared when asked.
// The alternative -- adding the storage function when it is wanted -- means
// re-enumerating, and re-enumerating drops the console session you are typing
// the command into. Declared always, the storage device simply reports no
// medium until someone hands it the card, which is what an empty card reader
// does and what every host already knows how to display.
//
// VID 0xcafe is TinyUSB's example identity and is fine for private use.
// Anything distributed needs a real VID/PID pair.

#define USB_VID 0xcafe
#define USB_PID 0x4001

static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    // Miscellaneous / Common class with IAD: required for Windows to bind a
    // composite CDC device without an .inf file of its own.
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&desc_device;
}

#ifndef UBIQOS_LWIP
#define UBIQOS_LWIP 0
#endif

#if UBIQOS_LWIP
enum { ITF_NUM_CDC = 0, ITF_NUM_CDC_DATA, ITF_NUM_MSC,
       ITF_NUM_NCM, ITF_NUM_NCM_DATA, ITF_NUM_TOTAL };
#else
enum { ITF_NUM_CDC = 0, ITF_NUM_CDC_DATA, ITF_NUM_MSC, ITF_NUM_TOTAL };
#endif

#define EPNUM_CDC_NOTIF   0x81
#define EPNUM_CDC_OUT     0x02
#define EPNUM_CDC_IN      0x82
#define EPNUM_MSC_OUT     0x03
#define EPNUM_MSC_IN      0x83
#define EPNUM_NCM_NOTIF   0x84
#define EPNUM_NCM_OUT     0x05
#define EPNUM_NCM_IN      0x85

// The SDK's TUD_CDC_NCM_DESCRIPTOR with ONE BYTE CHANGED, which is why it is
// copied here rather than used: its NCM functional descriptor ends with
// bmNetworkCapabilities = 0, and it has to be 1.
//
// Bit 0 claims SetEthernetPacketFilter. macOS will not bring the link up
// reliably without the claim -- it is the same byte that was found in
// embassy-usb on this bench and fixed upstream as 0f26db1, where a device
// declaring it went from 3 recoveries in 9 attachments to 0 in 19. What macOS
// needs is the capability DECLARED; whether the request then succeeds or
// stalls does not matter.
#define UBIQOS_NCM_DESCRIPTOR(_itfnum, _desc_stridx, _mac_stridx, _ep_notif, _ep_notif_size, _epout, _epin, _epsize, _maxsegmentsize) \
  8, TUSB_DESC_INTERFACE_ASSOCIATION, _itfnum, 2, TUSB_CLASS_CDC, CDC_COMM_SUBCLASS_NETWORK_CONTROL_MODEL, 0, 0,\
  9, TUSB_DESC_INTERFACE, _itfnum, 0, 1, TUSB_CLASS_CDC, CDC_COMM_SUBCLASS_NETWORK_CONTROL_MODEL, 0, _desc_stridx,\
  5, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_HEADER, U16_TO_U8S_LE(0x0110),\
  5, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_UNION, _itfnum, (uint8_t)((_itfnum) + 1),\
  13, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_ETHERNET_NETWORKING, _mac_stridx, 0, 0, 0, 0, U16_TO_U8S_LE(_maxsegmentsize), U16_TO_U8S_LE(0), 0, \
  6, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_NCM, U16_TO_U8S_LE(0x0100), 1, \
  7, TUSB_DESC_ENDPOINT, _ep_notif, TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(_ep_notif_size), 50,\
  9, TUSB_DESC_INTERFACE, (uint8_t)((_itfnum)+1), 0, 0, TUSB_CLASS_CDC_DATA, 0, NCM_DATA_PROTOCOL_NETWORK_TRANSFER_BLOCK, 0,\
  9, TUSB_DESC_INTERFACE, (uint8_t)((_itfnum)+1), 1, 2, TUSB_CLASS_CDC_DATA, 0, NCM_DATA_PROTOCOL_NETWORK_TRANSFER_BLOCK, 0,\
  7, TUSB_DESC_ENDPOINT, _epin, TUSB_XFER_BULK, U16_TO_U8S_LE(_epsize), 0,\
  7, TUSB_DESC_ENDPOINT, _epout, TUSB_XFER_BULK, U16_TO_U8S_LE(_epsize), 0

#if UBIQOS_LWIP
#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN \
                           + TUD_CDC_NCM_DESC_LEN)
#else
#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN)
#endif

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 5, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
#if UBIQOS_LWIP
    UBIQOS_NCM_DESCRIPTOR(ITF_NUM_NCM, 6, 7, EPNUM_NCM_NOTIF, 64,
                          EPNUM_NCM_OUT, EPNUM_NCM_IN, 64, CFG_TUD_NET_MTU),
#endif
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

static const char *string_desc_arr[] = {
    (const char[]){ 0x09, 0x04 },   // 0: engelska (0x0409)
    "UbiqOS",                        // 1: manufacturer
    "UbiqOS console",                // 2: product
    // 3: serial number, replaced at startup by the chip's own unique id.
    //
    // It was "000001" on every board, and that is not a cosmetic fault. Two
    // UbiqOS boards on one Mac then differ in nothing the host can see -- same
    // vendor, same product, same serial -- so macOS cannot keep them apart: the
    // port names churn (usbmodem0000011, usbmodem6, usbmodem2013102 in one
    // afternoon), one console can vanish when the other is reflashed, and a
    // tool cannot be told which board to talk to. A whole day of this was spent
    // identifying boards by asking them how many scanline buffers they had.
    //
    // The MAC beside it has been derived from the unique id all along, with a
    // comment saying it is so two boards on one desk do not collide. The serial
    // needed the same and did not have it.
    "000001",
    "UbiqOS CDC",                    // 4: the CDC interface
    "UbiqOS SD card",                // 5: the mass storage interface
    "UbiqOS network",                // 6: the NCM interface
    // 7: the MAC address, which the class requires as TWELVE HEX DIGITS and
    // not as six bytes. It is filled in at startup from the chip's own unique
    // id, so two boards on one desk do not collide -- see below.
    "000000000000",
};

// The MAC. Locally administered (bit 1 of the first byte) and not multicast
// (bit 0 clear), which is what the 0x02 is for: 02:xx:xx:xx:xx:xx belongs to
// whoever made the device and is guaranteed not to clash with a real vendor.
#if UBIQOS_LWIP
uint8_t tud_network_mac_address[6] = { 0x02, 0, 0, 0, 0, 0 };
#else
static uint8_t tud_network_mac_address[6] = { 0x02, 0, 0, 0, 0, 0 };
#endif

static char mac_string[13];
static char serial_string[17];

void ubiqos_usb_net_id(const uint8_t *unique, uint32_t n)
{
    static const char hex[] = "0123456789abcdef";
    for (uint32_t i = 0; i < 5 && i < n; i++)
        tud_network_mac_address[1 + i] = unique[i];
    for (uint32_t i = 0; i < 6; i++) {
        mac_string[i * 2]     = hex[tud_network_mac_address[i] >> 4];
        mac_string[i * 2 + 1] = hex[tud_network_mac_address[i] & 0x0f];
    }
    mac_string[12] = 0;
    string_desc_arr[7] = mac_string;

    // And the serial number: the LAST four bytes of the id, as eight hex
    // digits.
    //
    // Four and not eight, because macOS puts a short serial into the device
    // node's name and falls back to a location id for a long one -- sixteen
    // digits gave /dev/cu.usbmodem2013701, which says where the cable is
    // plugged in rather than which board answered, and moves when the cable
    // does. Eight digits keeps the name with the board.
    //
    // The last bytes rather than the first: a flash id's low end varies between
    // parts where its high end is a manufacturer's prefix.
    if (n >= 4) {
        const uint8_t *tail = unique + (n - 4);
        for (uint32_t i = 0; i < 4; i++) {
            serial_string[i * 2]     = hex[tail[i] >> 4];
            serial_string[i * 2 + 1] = hex[tail[i] & 0x0f];
        }
        serial_string[8] = 0;
        string_desc_arr[3] = serial_string;
    }
}

static uint16_t desc_str[32];

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    uint8_t chr_count;

    if (index == 0) {
        desc_str[1] = 0x0409;
        chr_count = 1;
    } else {
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) return NULL;
        const char *str = string_desc_arr[index];
        chr_count = 0;
        while (str[chr_count] && chr_count < 31) chr_count++;
        for (uint8_t i = 0; i < chr_count; i++) desc_str[1 + i] = str[i];
    }

    // The first word is length and type.
    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return desc_str;
}
