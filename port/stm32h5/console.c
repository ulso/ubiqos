// The serial console: USART3 on PD8 (TX) and PD9 (RX), alternate function 7,
// which the NUCLEO-H563ZI wires to the ST-LINK's virtual serial port.
//
// Two rings and one interrupt. The shell's output and the kernel's own lines
// both go into the transmit ring -- the board has one serial line to the
// computer, and the Fruit Jam's split between a debug UART and a USB console
// has no counterpart here -- and the interrupt feeds the USART from it. What
// arrives goes into the receive ring, and the kernel's tick finds it there:
// readable() is what wakes a reader, as for every other device.
//
// A write that finds the ring full with interrupts masked cannot wait for the
// interrupt to make room, so it sends the oldest byte itself. That is what lets
// the kernel print from a trap, and from before interrupts are on at all.

#include "stm32h5xx.h"
#include "port.h"
#include "../../common/ubiqos_abi.h"

#define CONSOLE_TX_PIN 8u
#define CONSOLE_RX_PIN 9u
#define CONSOLE_AF     7u

#define TX_SIZE 2048u               // powers of two: the indices wrap by mask
#define RX_SIZE 256u

static uint8_t tx_ring[TX_SIZE];
static volatile uint32_t tx_head, tx_tail;  // head: next free; tail: next to send
static uint8_t rx_ring[RX_SIZE];
static volatile uint32_t rx_head, rx_tail;
static volatile uint32_t rx_dropped;
static volatile bool intr_seen;       // a Ctrl-C arrived; the console thread acts on it
static bool up;

static void pin_af(GPIO_TypeDef *g, uint32_t pin, uint32_t af)
{
    g->MODER = (g->MODER & ~(3u << (pin * 2u))) | (2u << (pin * 2u));
    g->OSPEEDR |= (2u << (pin * 2u));
    volatile uint32_t *afr = &g->AFR[pin >> 3];
    const uint32_t shift = (pin & 7u) * 4u;
    *afr = (*afr & ~(0xFu << shift)) | (af << shift);
}

void h5_console_init(uint32_t baud)
{
    RCC->AHB2ENR  |= RCC_AHB2ENR_GPIODEN;
    RCC->APB1LENR |= RCC_APB1LENR_USART3EN;
    (void)RCC->APB1LENR;              // the enable takes effect before the first access

    pin_af(GPIOD, CONSOLE_TX_PIN, CONSOLE_AF);
    pin_af(GPIOD, CONSOLE_RX_PIN, CONSOLE_AF);

    // USART3's kernel clock is PCLK1 out of reset (CCIPR1.USART3SEL = 0).
    USART3->CR1 = 0;
    USART3->BRR = (h5_pclk1_hz + baud / 2u) / baud;
    USART3->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE_RXFNEIE;

    // With the other devices, below the kernel's critical sections.
    NVIC_SetPriority(USART3_IRQn, 0x80u >> (8u - __NVIC_PRIO_BITS));
    NVIC_EnableIRQ(USART3_IRQn);
    up = true;
}

// Whether USART3's interrupt could run now: PRIMASK clear, and BASEPRI (when
// set) not at or above its priority.
static bool irq_can_run(void)
{
    if (__get_PRIMASK()) return false;
    const uint32_t bp = __get_BASEPRI();
    return bp == 0 || bp > 0x80u;
}

static void send_oldest(void)
{
    while (!(USART3->ISR & USART_ISR_TXE_TXFNF)) { }
    USART3->TDR = tx_ring[tx_tail & (TX_SIZE - 1u)];
    tx_tail++;
}

void USART3_IRQHandler(void)
{
    const uint32_t isr = USART3->ISR;
    if (isr & USART_ISR_ORE) USART3->ICR = USART_ICR_ORECF;
    if (isr & USART_ISR_RXNE_RXFNE) {
        const uint8_t c = (uint8_t)USART3->RDR;
        if (rx_head - rx_tail < RX_SIZE) rx_ring[rx_head++ & (RX_SIZE - 1u)] = c;
        else rx_dropped++;
        if (c == 0x03) intr_seen = true;
    }
    if ((USART3->CR1 & USART_CR1_TXEIE_TXFNFIE) && (isr & USART_ISR_TXE_TXFNF)) {
        if (tx_head != tx_tail) USART3->TDR = tx_ring[tx_tail++ & (TX_SIZE - 1u)];
        else USART3->CR1 &= ~USART_CR1_TXEIE_TXFNFIE;
    }
}

// As much as fits, now; a full ring gives a short count. The kernel's print
// path wants every byte and uses h5_console_put below.
uint32_t h5_console_write(const uint8_t *buf, uint32_t len)
{
    if (!up) return len;
    const uint32_t st = __get_PRIMASK();
    __disable_irq();
    uint32_t n = 0;
    while (n < len && tx_head - tx_tail < TX_SIZE) tx_ring[tx_head++ & (TX_SIZE - 1u)] = buf[n++];
    if (n) USART3->CR1 |= USART_CR1_TXEIE_TXFNFIE;
    __set_PRIMASK(st);
    return n;
}

uint32_t h5_console_room(void) { return TX_SIZE - (tx_head - tx_tail); }

// Every byte, whatever the state of the interrupts.
void h5_console_putc(char c)
{
    if (!up) return;
    for (;;) {
        const uint8_t b = (uint8_t)c;
        if (h5_console_write(&b, 1)) return;
        if (irq_can_run()) continue;           // the interrupt will make room
        const uint32_t st = __get_PRIMASK();
        __disable_irq();
        if (tx_head - tx_tail == TX_SIZE) send_oldest();
        __set_PRIMASK(st);
    }
}

void h5_console_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') h5_console_putc('\r');
        h5_console_putc(*s++);
    }
}

uint32_t h5_console_read(uint8_t *buf, uint32_t len)
{
    uint32_t n = 0;
    while (n < len && rx_tail != rx_head) buf[n++] = rx_ring[rx_tail++ & (RX_SIZE - 1u)];
    return n;
}

uint32_t h5_console_available(void) { return rx_head - rx_tail; }

int h5_console_getc(void)
{
    uint8_t c;
    return h5_console_read(&c, 1) ? c : -1;
}

// --- /dev/term --------------------------------------------------------------
//
// The same device the Fruit Jam calls term, and the one the kernel falls back on
// when no descriptor names a console. Line endings are the terminal's business:
// a newline goes out as CR LF, as the RP2350's UART driver sends it.

static int32_t term_configure(const void *config, uint32_t size)
{
    (void)config; (void)size;
    return 0;
}

static int32_t term_open(void)  { return 0; }
static int32_t term_close(void) { return 0; }

static int32_t term_write(const uint8_t *buf, uint32_t len)
{
    // Room for the worst case, every byte a newline, or nothing: a partial
    // write that split a CR from its LF would be harmless but a partial write
    // that stops mid-line is what the caller retries, so count what went.
    uint32_t n = 0;
    while (n < len) {
        const uint32_t need = buf[n] == '\n' ? 2u : 1u;
        if (h5_console_room() < need) break;
        if (buf[n] == '\n') h5_console_write((const uint8_t *)"\r", 1);
        h5_console_write(&buf[n], 1);
        n++;
    }
    return (int32_t)n;
}

static int32_t term_read(uint8_t *buf, uint32_t len) { return (int32_t)h5_console_read(buf, len); }
static int32_t term_readable(void) { return (int32_t)h5_console_available(); }
static int32_t term_writable(void) { return (int32_t)(h5_console_room() / 2u); }

const ubiqos_driver_t h5_term_driver = {
    .module_name = "h5uart",
    .configure = term_configure,
    .open = term_open, .write = term_write, .read = term_read, .close = term_close,
    .readable = term_readable, .writable = term_writable,
};

// --- Ctrl-C -------------------------------------------------------------------
//
// What the USB console does on the RP2350, here: the key ends the command in
// front of the terminal, and whatever was typed after it goes with it. The
// interrupt only notices -- ending a process is the kernel's business and may
// not be done from a handler -- and this thread, which wakes every few
// milliseconds above every program, does the rest. With nothing in front the
// key stays in the input like any other, and the shell clears its line.
//
// Above every program, because the command a Ctrl-C is for may be one that
// never gives the processor up.

#include "usbdev.h"                    // the thread priorities

void ubiqos_print(const char *s);
int32_t ubiqos_kernel_thread(void (*entry)(void), uint32_t stack_bytes, uint32_t priority);
bool ubiqos_io_interrupt(const char *device_name);

static void console_thread(void)
{
    for (;;) {
        if (intr_seen) {
            intr_seen = false;
            if (ubiqos_io_interrupt("term")) {
                const uint32_t st = __get_PRIMASK();
                __disable_irq();
                rx_tail = rx_head;             // the typed-ahead, and the key itself
                __set_PRIMASK(st);
            }
        }
        ubiqos_sleep(5);
    }
}

void h5_console_start_thread(void)
{
    if (ubiqos_kernel_thread(console_thread, 1024, UBIQOS_PRIO_USB) < 0)
        ubiqos_print("console: could not start its thread; Ctrl-C will not end commands\n");
}
