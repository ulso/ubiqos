// Ethernet on the STM32H5: the MAC, its DMA, and the LAN8742A PHY the
// NUCLEO-H563ZI has on RMII.
//
// The MAC is Synopsys's Ethernet QoS, the same block as in the STM32H7, and
// what it wants is two rings of descriptors in RAM: the driver writes one with
// OWN set to hand a buffer over, and the DMA clears OWN to hand it back. The
// Cortex-M33 here has no data cache, which is the whole of the H7's trouble with
// this block -- descriptors and buffers are ordinary memory, with a barrier
// before OWN is given away.
//
// Polled, from the network thread, once a millisecond or so: that is how
// every other interface in UbiqOS reaches lwIP, and a frame waiting a
// millisecond is not what limits anything on a 100 Mbit link to a machine like
// this. The interrupt can come later, if a measurement ever asks for it.
//
// The pins are fixed by the board -- see boards/nucleo-h563zi.h -- and so is
// the PHY's address, 0. The PHY's 25 MHz crystal gives the MAC its 50 MHz RMII
// reference clock, which is also why the DMA reset below cannot finish without
// the PHY powered: the MAC's clock domains run from it.

#include <string.h>
#include "stm32h5xx.h"
#include "port.h"
#include "eth.h"
#include "pico/time.h"

#define RX_DESC   8u
#define TX_DESC   4u
#define BUF_BYTES 1536u              // a whole frame in one buffer, a multiple of 4

typedef struct { volatile uint32_t d0, d1, d2, d3; } desc_t;

#define DES3_OWN   (1u << 31)
#define DES3_IOC   (1u << 30)        // read format, receive
#define DES3_FD    (1u << 29)
#define DES3_LD    (1u << 28)
#define RDES3_BUF1V (1u << 24)       // read format: buffer 1 address is valid
#define RDES3_ES   (1u << 15)        // write-back: error summary
#define RDES3_PL   0x7FFFu           // write-back: packet length

static desc_t rx_desc[RX_DESC] __attribute__((aligned(16)));
static desc_t tx_desc[TX_DESC] __attribute__((aligned(16)));
static uint8_t rx_buf[RX_DESC][BUF_BYTES] __attribute__((aligned(4)));
static uint8_t tx_buf[TX_DESC][BUF_BYTES] __attribute__((aligned(4)));
static uint32_t rx_next, tx_next;

uint32_t h5_eth_rx_frames, h5_eth_rx_errors, h5_eth_tx_frames, h5_eth_tx_busy;

// --- the pins -----------------------------------------------------------------

static void pin_eth(GPIO_TypeDef *g, uint32_t pin)
{
    g->MODER   = (g->MODER & ~(3u << (pin * 2u))) | (2u << (pin * 2u));   // alternate
    g->OSPEEDR |= (3u << (pin * 2u));                                      // very high
    volatile uint32_t *afr = &g->AFR[pin >> 3];
    const uint32_t shift = (pin & 7u) * 4u;
    *afr = (*afr & ~(0xFu << shift)) | (11u << shift);                     // AF11, ETH
}

static void pins_init(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOBEN | RCC_AHB2ENR_GPIOCEN
                  | RCC_AHB2ENR_GPIOGEN;
    (void)RCC->AHB2ENR;
    pin_eth(GPIOA, 1);     // REF_CLK
    pin_eth(GPIOA, 2);     // MDIO
    pin_eth(GPIOA, 7);     // CRS_DV
    pin_eth(GPIOC, 1);     // MDC
    pin_eth(GPIOC, 4);     // RXD0
    pin_eth(GPIOC, 5);     // RXD1
    pin_eth(GPIOG, 11);    // TX_EN
    pin_eth(GPIOG, 13);    // TXD0
    pin_eth(GPIOB, 15);    // TXD1
}

// --- MDIO: the PHY's registers -------------------------------------------------

#define PHY_ADDR   0u
#define PHY_BMCR   0u
#define PHY_BMSR   1u
#define PHY_ID1    2u
#define PHY_SCSR   31u               // LAN8742A: special control/status, the result
#define BMCR_RESET   (1u << 15)
#define BMCR_ANEN    (1u << 12)
#define BMCR_ANRESTART (1u << 9)
#define BMSR_LINK    (1u << 2)
#define BMSR_ANDONE  (1u << 5)

// MDC must stay under 2.5 MHz: CR = 4 divides HCLK by 102, for 150 to 250 MHz.
#define MDIO_CR    4u

static bool mdio_wait(void)
{
    for (uint32_t i = 0; i < 200000u; i++)
        if (!(ETH->MACMDIOAR & (1u << ETH_MACMDIOAR_MB_Pos))) return true;
    return false;
}

static int32_t mdio_read(uint32_t reg)
{
    if (!mdio_wait()) return -1;
    ETH->MACMDIOAR = (PHY_ADDR << ETH_MACMDIOAR_PA_Pos) | (reg << ETH_MACMDIOAR_RDA_Pos)
                   | (MDIO_CR << ETH_MACMDIOAR_CR_Pos) | (3u << ETH_MACMDIOAR_MOC_Pos)
                   | (1u << ETH_MACMDIOAR_MB_Pos);
    if (!mdio_wait()) return -1;
    return (int32_t)(ETH->MACMDIODR & 0xFFFFu);
}

static bool mdio_write(uint32_t reg, uint32_t value)
{
    if (!mdio_wait()) return false;
    ETH->MACMDIODR = value & 0xFFFFu;
    ETH->MACMDIOAR = (PHY_ADDR << ETH_MACMDIOAR_PA_Pos) | (reg << ETH_MACMDIOAR_RDA_Pos)
                   | (MDIO_CR << ETH_MACMDIOAR_CR_Pos) | (1u << ETH_MACMDIOAR_MOC_Pos)
                   | (1u << ETH_MACMDIOAR_MB_Pos);
    return mdio_wait();
}

// --- the rings -------------------------------------------------------------------

static void rx_give(uint32_t i)
{
    rx_desc[i].d0 = (uint32_t)rx_buf[i];
    rx_desc[i].d1 = 0;
    rx_desc[i].d2 = 0;
    __DMB();                                    // the address before the ownership
    rx_desc[i].d3 = DES3_OWN | DES3_IOC | RDES3_BUF1V;
}

static void rings_init(void)
{
    memset((void *)tx_desc, 0, sizeof tx_desc);
    for (uint32_t i = 0; i < RX_DESC; i++) rx_give(i);
    rx_next = tx_next = 0;

    ETH->DMACTDLAR = (uint32_t)tx_desc;
    ETH->DMACTDRLR = TX_DESC - 1u;
    ETH->DMACTDTPR = (uint32_t)tx_desc;          // nothing to send yet
    ETH->DMACRDLAR = (uint32_t)rx_desc;
    ETH->DMACRDRLR = RX_DESC - 1u;
    ETH->DMACRDTPR = (uint32_t)&rx_desc[RX_DESC - 1u];
}

// --- bring-up --------------------------------------------------------------------

const char *h5_eth_start(const uint8_t mac[6])
{
    // RMII, chosen in the system configuration block before the MAC is clocked:
    // the MAC samples the choice when it comes out of reset.
    RCC->APB3ENR |= RCC_APB3ENR_SBSEN;
    (void)RCC->APB3ENR;
    SBS->PMCR = (SBS->PMCR & ~SBS_PMCR_ETH_SEL_PHY) | SBS_PMCR_ETH_SEL_PHY_2;

    pins_init();
    RCC->AHB1ENR |= RCC_AHB1ENR_ETHEN | RCC_AHB1ENR_ETHTXEN | RCC_AHB1ENR_ETHRXEN;
    (void)RCC->AHB1ENR;

    // The DMA's software reset, which clears once every clock domain has seen
    // it -- including the ones that run from the PHY's reference clock.
    ETH->DMAMR |= ETH_DMAMR_SWR;
    uint32_t n = 0;
    while (ETH->DMAMR & ETH_DMAMR_SWR)
        if (++n > 2000000u) return "the MAC did not come out of reset (no 50 MHz from the PHY?)";

    // The PHY: reset, then autonegotiation, whose result is read when the link
    // comes up -- see h5_eth_link.
    const int32_t id = mdio_read(PHY_ID1);
    if (id < 0 || id == 0xFFFF || id == 0) return "no PHY answers at MDIO address 0";
    mdio_write(PHY_BMCR, BMCR_RESET);
    for (n = 0; n < 1000u; n++) {
        const int32_t v = mdio_read(PHY_BMCR);
        if (v >= 0 && !(v & BMCR_RESET)) break;
        busy_wait_us(100);
    }
    mdio_write(PHY_BMCR, BMCR_ANEN | BMCR_ANRESTART);

    // The MAC: its address, multicast passed (mDNS joins 224.0.0.251 and the
    // hash filter is not worth its trouble), the CRC stripped on the way in.
    ETH->MACA0HR = ((uint32_t)mac[5] << 8) | mac[4];
    ETH->MACA0LR = ((uint32_t)mac[3] << 24) | ((uint32_t)mac[2] << 16)
                 | ((uint32_t)mac[1] << 8) | mac[0];
    ETH->MACPFR  = ETH_MACPFR_PM;
    ETH->MACCR   = ETH_MACCR_ACS | ETH_MACCR_CST;

    // The DMA and the queues between it and the MAC: bursts of 32 beats,
    // aligned, and whole frames through each queue before they move on.
    ETH->DMASBMR = ETH_DMASBMR_AAL;
    ETH->DMACCR  = 0;                                  // descriptors back to back
    ETH->DMACTCR = (32u << ETH_DMACTCR_TPBL_Pos);
    ETH->DMACRCR = (32u << ETH_DMACRCR_RPBL_Pos) | (BUF_BYTES << ETH_DMACRCR_RBSZ_Pos);
    ETH->MTLTQOMR |= ETH_MTLTQOMR_TSF;
    ETH->MTLRQOMR |= ETH_MTLRQOMR_RSF;

    rings_init();
    ETH->DMACSR = 0xFFFFFFFFu;                         // every status bit, cleared
    ETH->DMACTCR |= ETH_DMACTCR_ST;
    ETH->DMACRCR |= ETH_DMACRCR_SR;
    ETH->MACCR   |= ETH_MACCR_TE | ETH_MACCR_RE;
    return 0;
}

// Whether the PHY has a link, and on a change the MAC is told the speed and
// duplex it came up at. 0 no link, 10 or 100 with a link; *full says duplex.
uint32_t h5_eth_link(bool *full)
{
    // BMSR latches a link loss until read, so it is read twice: the first says
    // whether the link went away since last time, the second what it is now.
    (void)mdio_read(PHY_BMSR);
    const int32_t bmsr = mdio_read(PHY_BMSR);
    if (bmsr < 0 || !(bmsr & BMSR_LINK) || !(bmsr & BMSR_ANDONE)) return 0;

    const int32_t scsr = mdio_read(PHY_SCSR);
    if (scsr < 0) return 0;
    const uint32_t speed = (scsr >> 2) & 7u;           // LAN8742A: 001 10H, 101 10F, 010 100H, 110 100F
    const bool fast = (speed & 2u) != 0;
    *full = (speed & 4u) != 0;

    uint32_t cr = ETH->MACCR & ~(ETH_MACCR_FES | ETH_MACCR_DM);
    if (fast)  cr |= ETH_MACCR_FES;
    if (*full) cr |= ETH_MACCR_DM;
    ETH->MACCR = cr;
    return fast ? 100u : 10u;
}

// --- frames ------------------------------------------------------------------------

// One frame into the next free transmit buffer, or false when all four are
// still the DMA's. The MAC adds the padding and the CRC.
bool h5_eth_send(const uint8_t *frame, uint32_t len)
{
    if (len > BUF_BYTES) return false;
    desc_t *d = &tx_desc[tx_next];
    if (d->d3 & DES3_OWN) { h5_eth_tx_busy++; return false; }

    memcpy(tx_buf[tx_next], frame, len);
    d->d0 = (uint32_t)tx_buf[tx_next];
    d->d1 = 0;
    d->d2 = len & 0x3FFFu;
    __DMB();
    d->d3 = DES3_OWN | DES3_FD | DES3_LD | (len & 0x7FFFu);

    tx_next = (tx_next + 1u) % TX_DESC;
    __DMB();
    ETH->DMACTDTPR = (uint32_t)&tx_desc[tx_next];      // the DMA runs up to here
    h5_eth_tx_frames++;
    return true;
}

// The next received frame, handed to deliver and then given back to the DMA.
// Returns false when the DMA still owns the next descriptor -- nothing waiting.
bool h5_eth_receive(void (*deliver)(const uint8_t *frame, uint32_t len))
{
    desc_t *d = &rx_desc[rx_next];
    const uint32_t d3 = d->d3;
    if (d3 & DES3_OWN) return false;

    if ((d3 & (DES3_FD | DES3_LD)) == (DES3_FD | DES3_LD) && !(d3 & RDES3_ES)) {
        h5_eth_rx_frames++;
        deliver(rx_buf[rx_next], d3 & RDES3_PL);
    } else {
        h5_eth_rx_errors++;
    }

    rx_give(rx_next);
    __DMB();
    ETH->DMACRDTPR = (uint32_t)&rx_desc[rx_next];
    rx_next = (rx_next + 1u) % RX_DESC;

    // A ring that ran dry stops the receiver; handing a descriptor back and
    // moving the tail is what restarts it, and the flag only needs clearing.
    if (ETH->DMACSR & ETH_DMACSR_RBU) ETH->DMACSR = ETH_DMACSR_RBU;
    return true;
}
