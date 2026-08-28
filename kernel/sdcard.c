#include "sdcard.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "pico/time.h"

void myrtos_print(const char *s);
void myrtos_print_u32(uint32_t v);

#define SD_SPI       spi0
#define SD_SCK_PIN   34
#define SD_MOSI_PIN  35
#define SD_MISO_PIN  36
#define SD_CS_PIN    39
#define SD_DETECT_PIN 33

#define CMD0_GO_IDLE          0
#define CMD8_SEND_IF_COND     8
#define CMD17_READ_SINGLE    17
#define CMD55_APP            55
#define CMD58_READ_OCR       58
#define ACMD41_SEND_OP_COND  41

#define R1_IDLE 0x01

// Kort större än 2 GB adresseras i block, mindre i byte. CMD58 avgör vilket.
static bool sd_block_addressed;

static void cs_low(void)  { gpio_put(SD_CS_PIN, 0); }
static void cs_high(void) { gpio_put(SD_CS_PIN, 1); }

static uint8_t sd_xfer(uint8_t out) {
    uint8_t in = 0xff;
    spi_write_read_blocking(SD_SPI, &out, &in, 1);
    return in;
}

// Kortet svarar inte omedelbart; det håller MISO högt tills det är redo.
static uint8_t sd_wait_response(void) {
    for (int i = 0; i < 8192; i++) {
        uint8_t r = sd_xfer(0xff);
        if (!(r & 0x80)) return r;
    }
    return 0xff;
}

static uint8_t sd_command(uint8_t cmd, uint32_t arg, uint8_t crc) {
    sd_xfer(0xff);
    sd_xfer(0x40 | cmd);
    sd_xfer((uint8_t)(arg >> 24));
    sd_xfer((uint8_t)(arg >> 16));
    sd_xfer((uint8_t)(arg >> 8));
    sd_xfer((uint8_t)arg);
    sd_xfer(crc);          // CRC krävs bara för CMD0 och CMD8
    return sd_wait_response();
}

// Pinnen måste konfigureras innan den läses. Att fråga före init gav ett
// oinitierat värde, och ett tomt fack rapporterades som isatt kort.
bool myrtos_sd_present(void) {
    static bool configured;
    if (!configured) {
        gpio_init(SD_DETECT_PIN);
        gpio_set_dir(SD_DETECT_PIN, GPIO_IN);
        gpio_pull_up(SD_DETECT_PIN);
        for (volatile int i = 0; i < 1000; i++) { }   // låt upplyftningen sätta sig
        configured = true;
    }
    return gpio_get(SD_DETECT_PIN) == 0;   // aktiv låg
}

bool myrtos_sd_init(void) {
    gpio_init(SD_CS_PIN);
    gpio_set_dir(SD_CS_PIN, GPIO_OUT);
    cs_high();
    gpio_init(SD_DETECT_PIN);
    gpio_set_dir(SD_DETECT_PIN, GPIO_IN);
    gpio_pull_up(SD_DETECT_PIN);

    // Initieringen måste ske långsamt: standarden tillåter högst 400 kHz
    // innan kortet sagt vad det klarar.
    spi_init(SD_SPI, 400 * 1000);
    gpio_set_function(SD_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(SD_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(SD_MISO_PIN, GPIO_FUNC_SPI);

    // Minst 74 klockpulser med CS hög innan kortet lyssnar.
    cs_high();
    for (int i = 0; i < 10; i++) sd_xfer(0xff);

    cs_low();
    uint8_t r = 0xff;
    for (int i = 0; i < 16 && r != R1_IDLE; i++) {
        r = sd_command(CMD0_GO_IDLE, 0, 0x95);
    }
    if (r != R1_IDLE) { cs_high(); myrtos_print("SD: no response to CMD0\n"); return false; }

    // CMD8 skiljer moderna kort (v2) från gamla. 0x1AA = 2,7-3,6 V, mönster AA.
    r = sd_command(CMD8_SEND_IF_COND, 0x1aa, 0x87);
    if (r & ~R1_IDLE) { cs_high(); myrtos_print("SD: card too old (no CMD8)\n"); return false; }
    for (int i = 0; i < 4; i++) sd_xfer(0xff);   // resten av R7

    // ACMD41 med HCS-biten: be kortet lämna idle och tala om att vi klarar
    // högkapacitetskort.
    absolute_time_t deadline = make_timeout_time_ms(1000);
    do {
        sd_command(CMD55_APP, 0, 0xff);
        r = sd_command(ACMD41_SEND_OP_COND, 0x40000000, 0xff);
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) {
            cs_high(); myrtos_print("SD: timed out leaving idle\n"); return false;
        }
    } while (r != 0);

    r = sd_command(CMD58_READ_OCR, 0, 0xff);
    if (r) { cs_high(); myrtos_print("SD: CMD58 failed\n"); return false; }
    uint8_t ocr0 = sd_xfer(0xff);
    for (int i = 0; i < 3; i++) sd_xfer(0xff);
    sd_block_addressed = (ocr0 & 0x40) != 0;   // CCS

    cs_high();
    sd_xfer(0xff);

    // Nu får farten upp.
    spi_set_baudrate(SD_SPI, 12 * 1000 * 1000);

    myrtos_print("SD: card ready, ");
    myrtos_print(sd_block_addressed ? "block addressed (SDHC/SDXC)\n" : "byte addressed (SDSC)\n");
    return true;
}

bool myrtos_sd_read_block(uint32_t lba, uint8_t *buf) {
    uint32_t addr = sd_block_addressed ? lba : lba * 512u;

    cs_low();
    if (sd_command(CMD17_READ_SINGLE, addr, 0xff) != 0) {
        cs_high();
        return false;
    }
    // Kortet skickar 0xFE när datablocket börjar.
    uint8_t token = 0xff;
    for (int i = 0; i < 20000 && token == 0xff; i++) token = sd_xfer(0xff);
    if (token != 0xfe) { cs_high(); return false; }

    for (int i = 0; i < 512; i++) buf[i] = sd_xfer(0xff);
    sd_xfer(0xff);      // CRC, som vi inte kontrollerar i SPI-läge
    sd_xfer(0xff);

    cs_high();
    sd_xfer(0xff);
    return true;
}
