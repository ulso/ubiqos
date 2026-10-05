// The I2C bus on the STM32F4: I2C1 on the pins the board header names -- on
// the Feather STM32F405, PB6 and PB7, the STEMMA QT connector.
//
// Built in rather than a driver module, as the console's USART is, and under
// the RP2350's driver's name, i2cbus: the same descriptor, i2cdesc, then gives
// the same /dev/i2c on both machines, and the same `i2c` command works on it.
// A process writes a ubiqos_i2c_xfer_t and the bytes to send, and reads back
// what came in -- see the note beside ubiqos_i2c_xfer_t in the ABI.
//
// The F4's I2C block is the older one, whose reading has to be steered byte by
// byte as the reference manual (RM0090, 27.3.3) lays out, with three cases: one
// byte, two, and more. It is done by polling, in the caller's system call, as
// the RP2350's SDK does it. Every wait has a deadline; a bus that misses one is
// stopped and the block reset, and a bus held low by a device that lost its
// place -- a STEMMA cable pulled mid-transfer does that -- is clocked free
// before the next transaction.

#include <stdint.h>
#include <stdbool.h>
#include "stm32f4xx.h"
#include "port.h"
#include "board.h"
#include "pico/time.h"
#include "../../../common/ubiqos_abi.h"

#if !defined(UBIQOS_STM32_I2C_PORT) || !defined(UBIQOS_STM32_I2C_SCL) || !defined(UBIQOS_STM32_I2C_SDA)
#error "the board header must say UBIQOS_STM32_I2C_PORT, _SCL and _SDA"
#endif

#define I2C_GPIO   ((GPIO_TypeDef *)(GPIOA_BASE + (UBIQOS_STM32_I2C_PORT - 'A') * 0x400u))
#define I2C_GPIOEN (1u << (UBIQOS_STM32_I2C_PORT - 'A'))
#define I2C_AF     4u                // I2C1 on every pin it has
#define I2C_HZ     100000u           // standard mode, which every STEMMA board takes
#define WAIT_US    10000u            // far more than one byte at 100 kHz

void ubiqos_print(const char *s);
int32_t ubiqos_pin_claim(uint32_t pin, const char *who);

static bool ready;
static uint8_t rx[UBIQOS_I2C_MAX_READ];
static uint32_t rx_len;
uint32_t stm32_i2c_timeouts, stm32_i2c_recoveries;   // for a probe

static void pin_mode(uint32_t pin, uint32_t mode)
{
    I2C_GPIO->MODER = (I2C_GPIO->MODER & ~(3u << (pin * 2u))) | (mode << (pin * 2u));
}

static void pin_setup(uint32_t pin)
{
    I2C_GPIO->OTYPER |= 1u << pin;                                   // open drain
    I2C_GPIO->OSPEEDR |= 2u << (pin * 2u);
    // Courtesy pull-ups, tens of kilohms: a STEMMA breakout brings the real
    // ones, and these only keep a bus with nothing on it from floating.
    I2C_GPIO->PUPDR = (I2C_GPIO->PUPDR & ~(3u << (pin * 2u))) | (1u << (pin * 2u));
    volatile uint32_t *afr = &I2C_GPIO->AFR[pin >> 3];
    const uint32_t shift = (pin & 7u) * 4u;
    *afr = (*afr & ~(0xFu << shift)) | (I2C_AF << shift);
    pin_mode(pin, 2u);
}

// A device that was cut off in the middle of sending a byte goes on holding
// SDA low, waiting for clocks that never come, and the block then sees a busy
// bus for ever. Nine clocks by hand let it finish its byte, and a stop after
// them puts the bus back where everybody agrees on it.
static void bus_recover(void)
{
    const uint32_t scl = UBIQOS_STM32_I2C_SCL, sda = UBIQOS_STM32_I2C_SDA;
    I2C_GPIO->BSRR = (1u << scl) | (1u << sda);
    pin_mode(scl, 1u);
    pin_mode(sda, 1u);
    for (int i = 0; i < 9 && !(I2C_GPIO->IDR & (1u << sda)); i++) {
        I2C_GPIO->BSRR = 1u << (scl + 16u); busy_wait_us(5);
        I2C_GPIO->BSRR = 1u << scl;         busy_wait_us(5);
    }
    // A stop: SDA rising while SCL is high.
    I2C_GPIO->BSRR = 1u << (sda + 16u); busy_wait_us(5);
    I2C_GPIO->BSRR = 1u << scl;         busy_wait_us(5);
    I2C_GPIO->BSRR = 1u << sda;         busy_wait_us(5);
    pin_mode(scl, 2u);
    pin_mode(sda, 2u);
    stm32_i2c_recoveries++;
}

static void block_init(void)
{
    I2C1->CR1 = I2C_CR1_SWRST;
    I2C1->CR1 = 0;
    const uint32_t mhz = stm32_pclk1_hz / 1000000u;
    I2C1->CR2 = mhz << I2C_CR2_FREQ_Pos;
    I2C1->CCR = stm32_pclk1_hz / (2u * I2C_HZ);       // standard mode, duty 1:1
    I2C1->TRISE = mhz + 1u;                           // 1000 ns rise time
    I2C1->CR1 = I2C_CR1_PE;
}

static int32_t i2c_configure(const void *config, uint32_t size)
{
    (void)config; (void)size;
    RCC->AHB1ENR |= I2C_GPIOEN;
    RCC->APB1ENR |= RCC_APB1ENR_I2C1EN;
    (void)RCC->APB1ENR;
    const uint32_t base = (uint32_t)(UBIQOS_STM32_I2C_PORT - 'A') * 16u;
    if (ubiqos_pin_claim(base + UBIQOS_STM32_I2C_SCL, "i2c") < 0 ||
        ubiqos_pin_claim(base + UBIQOS_STM32_I2C_SDA, "i2c") < 0)
        ubiqos_print("i2c: a pin was already claimed\n");
    pin_setup(UBIQOS_STM32_I2C_SCL);
    pin_setup(UBIQOS_STM32_I2C_SDA);
    if (!(I2C_GPIO->IDR & (1u << UBIQOS_STM32_I2C_SDA))) bus_recover();
    block_init();
    ready = true;
    ubiqos_print("  i2c driver: I2C1, 100 kHz\n");
    return 0;
}

static int32_t i2c_open(void)  { return ready ? 0 : -1; }
static int32_t i2c_close(void) { return 0; }

// Wait for a status bit, or for the device to refuse. 1 when it came, 0 for a
// NACK, -1 when nothing happened in time.
static int wait_sr1(uint32_t bits)
{
    const uint64_t end = time_us_64() + WAIT_US;
    for (;;) {
        const uint32_t sr1 = I2C1->SR1;
        if (sr1 & bits) return 1;
        if (sr1 & I2C_SR1_AF) { I2C1->SR1 = ~I2C_SR1_AF; return 0; }
        if (sr1 & (I2C_SR1_BERR | I2C_SR1_ARLO)) {
            I2C1->SR1 = ~(I2C_SR1_BERR | I2C_SR1_ARLO);
            return -1;
        }
        if (time_us_64() > end) return -1;
    }
}

// Ends a transaction that went wrong: a stop if the bus is ours, and the block
// reset if it timed out, since its state machine may be anywhere then.
static int32_t give_up(int why)
{
    I2C1->CR1 |= I2C_CR1_STOP;
    if (why < 0) {
        stm32_i2c_timeouts++;
        if (!(I2C_GPIO->IDR & (1u << UBIQOS_STM32_I2C_SDA))) bus_recover();
        block_init();
    }
    I2C1->CR1 &= ~(I2C_CR1_POS | I2C_CR1_ACK);
    return -1;
}

static int start(uint8_t addr_rw)
{
    I2C1->CR1 |= I2C_CR1_START;
    int r = wait_sr1(I2C_SR1_SB);
    if (r <= 0) return -1;
    I2C1->DR = addr_rw;
    return wait_sr1(I2C_SR1_ADDR);               // 0: nobody answered
}

static void clear_addr(void)
{
    (void)I2C1->SR1;
    (void)I2C1->SR2;
}

static int32_t receive(uint8_t addr, uint8_t *dst, uint32_t n)
{
    // POS and ACK decide what the block does with bytes not yet asked for, so
    // they are set before the address is acknowledged.
    if (n == 2) I2C1->CR1 |= I2C_CR1_POS | I2C_CR1_ACK;
    else if (n > 2) I2C1->CR1 |= I2C_CR1_ACK;
    else I2C1->CR1 &= ~I2C_CR1_ACK;

    const int r = start((uint8_t)(addr << 1 | 1u));
    if (r <= 0) return give_up(r);

    if (n == 1) {
        // The NACK is already set; the stop must follow ADDR at once, before
        // the byte has been clocked in, so nothing may come between them.
        const uint32_t st = __get_PRIMASK();
        __disable_irq();
        clear_addr();
        I2C1->CR1 |= I2C_CR1_STOP;
        __set_PRIMASK(st);
        if (wait_sr1(I2C_SR1_RXNE) <= 0) return give_up(-1);
        dst[0] = (uint8_t)I2C1->DR;
    } else if (n == 2) {
        const uint32_t st = __get_PRIMASK();
        __disable_irq();
        clear_addr();
        I2C1->CR1 &= ~I2C_CR1_ACK;              // with POS: the NACK is for the second
        __set_PRIMASK(st);
        if (wait_sr1(I2C_SR1_BTF) <= 0) return give_up(-1);
        I2C1->CR1 |= I2C_CR1_STOP;
        dst[0] = (uint8_t)I2C1->DR;
        dst[1] = (uint8_t)I2C1->DR;
    } else {
        clear_addr();
        uint32_t i = 0;
        while (n - i > 3) {
            if (wait_sr1(I2C_SR1_RXNE) <= 0) return give_up(-1);
            dst[i++] = (uint8_t)I2C1->DR;
        }
        // Three left: the third from last in DR, the next in the shift
        // register, and the NACK set for the last before either is taken.
        if (wait_sr1(I2C_SR1_BTF) <= 0) return give_up(-1);
        I2C1->CR1 &= ~I2C_CR1_ACK;
        dst[i++] = (uint8_t)I2C1->DR;
        if (wait_sr1(I2C_SR1_BTF) <= 0) return give_up(-1);
        I2C1->CR1 |= I2C_CR1_STOP;
        dst[i++] = (uint8_t)I2C1->DR;
        if (wait_sr1(I2C_SR1_RXNE) <= 0) return give_up(-1);
        dst[i++] = (uint8_t)I2C1->DR;
    }
    I2C1->CR1 &= ~I2C_CR1_POS;
    return (int32_t)n;
}

// The stop has been asked for; wait until it is on the wire, so that the next
// transaction's start is not taken as a repeated one.
static void stop_done(void)
{
    const uint64_t end = time_us_64() + WAIT_US;
    while ((I2C1->CR1 & I2C_CR1_STOP) && time_us_64() < end) { }
}

// One exchange. The write half ends with a repeated start rather than a stop
// when a read follows, so nothing else can take the bus between naming a
// register and reading it.
static int32_t i2c_do_write(const uint8_t *buf, uint32_t len)
{
    if (!ready || len < sizeof(ubiqos_i2c_xfer_t)) return -1;
    ubiqos_i2c_xfer_t x;
    x.addr = buf[0]; x.nwrite = buf[1]; x.nread = buf[2]; x.reserved = buf[3];
    if (x.nread > UBIQOS_I2C_MAX_READ || x.addr > 0x7Fu) return -1;
    if (len < sizeof x + x.nwrite) return -1;
    rx_len = 0;

    // A bus some other master -- or a stuck device -- is holding.
    if (I2C1->SR2 & I2C_SR2_BUSY) {
        if (!(I2C_GPIO->IDR & (1u << UBIQOS_STM32_I2C_SDA))) bus_recover();
        block_init();
        if (I2C1->SR2 & I2C_SR2_BUSY) return -1;
    }

    if (x.nwrite) {
        const int r = start((uint8_t)(x.addr << 1));
        if (r <= 0) { give_up(r); stop_done(); return -1; }
        clear_addr();
        const uint8_t *p = buf + sizeof x;
        for (uint32_t i = 0; i < x.nwrite; i++) {
            const int t = wait_sr1(I2C_SR1_TXE);
            if (t <= 0) { give_up(t); stop_done(); return -1; }
            I2C1->DR = p[i];
        }
        const int b = wait_sr1(I2C_SR1_BTF);
        if (b <= 0) { give_up(b); stop_done(); return -1; }
        if (!x.nread) I2C1->CR1 |= I2C_CR1_STOP;
    }

    if (x.nread) {
        const int32_t n = receive(x.addr, rx, x.nread);
        if (n < 0) { stop_done(); return -1; }
        rx_len = (uint32_t)n;
    }
    stop_done();
    return (int32_t)len;
}

static int32_t i2c_do_read(uint8_t *buf, uint32_t len)
{
    const uint32_t n = len < rx_len ? len : rx_len;
    for (uint32_t i = 0; i < n; i++) buf[i] = rx[i];
    return (int32_t)n;
}

static int32_t i2c_readable(void) { return (int32_t)rx_len; }

const ubiqos_driver_t stm32_i2c_driver = {
    .module_name = "i2cbus",
    .configure = i2c_configure,
    .open = i2c_open, .write = i2c_do_write, .read = i2c_do_read,
    .close = i2c_close, .readable = i2c_readable,
};
