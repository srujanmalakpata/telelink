/*
 * Example device firmware for an STM32F4-class Cortex-M4 (USART2 on PA2/PA3,
 * 16 MHz HSI reset clock, SysTick at 1 kHz).
 *
 * Purpose: prove the library cross-compiles and links into a bare-metal image
 * and measure its flash/RAM footprint. It has NOT been flashed to hardware;
 * register addresses follow the STM32F405/407 reference manual (RM0090) and
 * must be checked against the target part before use.
 *
 * Integration:
 *   USART2 RX interrupt -> tl_link_rx_isr()       (ISR context, lock-free)
 *   main loop           -> tl_link_poll()         (decode, ACK, dispatch, timers)
 *   SysTick             -> millisecond tick for the HAL
 */
#include <stdatomic.h>
#include <stdint.h>

#include "tl/link.h"

#define REG32(addr) (*(volatile uint32_t *)(addr))

#define RCC_AHB1ENR REG32(0x40023830u)
#define RCC_APB1ENR REG32(0x40023840u)
#define GPIOA_MODER REG32(0x40020000u)
#define GPIOA_AFRL REG32(0x40020020u)
#define USART2_SR REG32(0x40004400u)
#define USART2_DR REG32(0x40004404u)
#define USART2_BRR REG32(0x40004408u)
#define USART2_CR1 REG32(0x4000440Cu)
#define NVIC_ISER1 REG32(0xE000E104u)
#define SYST_CSR REG32(0xE000E010u)
#define SYST_RVR REG32(0xE000E014u)
#define SYST_CVR REG32(0xE000E018u)

#define USART_SR_ORE (1u << 3)
#define USART_SR_RXNE (1u << 5)
#define USART_SR_TXE (1u << 7)
#define USART2_IRQN 38u
#define CORE_CLOCK_HZ 16000000u

enum { MSG_SET_PERIOD = 0x10, MSG_PING = 0x11, MSG_PONG = 0x12, MSG_TELEMETRY = 0x20 };

static volatile uint32_t g_ms;           /* written by SysTick only */
static _Atomic uint32_t g_uart_overruns; /* written by the USART ISR only */
static tl_link g_link;
static uint8_t g_rx_ring[128];
static uint32_t g_period_ms = 100u;

void SysTick_Handler(void);
void USART2_IRQHandler(void);
int main(void);

void SysTick_Handler(void)
{
    g_ms = g_ms + 1u;
}

void USART2_IRQHandler(void)
{
    const uint32_t sr = USART2_SR;
    if ((sr & (USART_SR_RXNE | USART_SR_ORE)) != 0u) {
        const uint8_t byte = (uint8_t)USART2_DR; /* SR-then-DR read also clears ORE */
        if ((sr & USART_SR_ORE) != 0u) {
            const uint32_t n = atomic_load_explicit(&g_uart_overruns, memory_order_relaxed);
            atomic_store_explicit(&g_uart_overruns, n + 1u, memory_order_relaxed);
        }
        (void)tl_link_rx_isr(&g_link, byte);
    }
}

/* ---- HAL implementation */

static size_t board_uart_write(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    for (size_t i = 0; i < len; i++) {
        while ((USART2_SR & USART_SR_TXE) == 0u) {
        }
        USART2_DR = data[i];
    }
    return len;
}

static uint32_t board_get_tick_ms(void *ctx)
{
    (void)ctx;
    return g_ms;
}

static void board_init(void)
{
    RCC_AHB1ENR |= 1u << 0;                                   /* GPIOA */
    RCC_APB1ENR |= 1u << 17;                                  /* USART2 */
    GPIOA_MODER = (GPIOA_MODER & ~(0xFu << 4)) | (0xAu << 4); /* PA2, PA3: AF */
    GPIOA_AFRL = (GPIOA_AFRL & ~(0xFFu << 8)) | (0x77u << 8); /* AF7 = USART2 */
    USART2_BRR = 0x8Bu; /* 16 MHz / (16 * 8.6875) ~= 115108 baud (-0.08%) */
    USART2_CR1 = (1u << 13) | (1u << 5) | (1u << 3) | (1u << 2); /* UE RXNEIE TE RE */
    NVIC_ISER1 = 1u << (USART2_IRQN - 32u);
    SYST_RVR = CORE_CLOCK_HZ / 1000u - 1u;
    SYST_CVR = 0u;
    SYST_CSR = 7u; /* core clock, interrupt, enable */
}

/* ---- Application */

static void on_set_period(const tl_packet *pkt, void *ctx)
{
    (void)ctx;
    if (pkt->len == 2u) {
        const uint32_t p = (uint32_t)pkt->payload[0] | ((uint32_t)pkt->payload[1] << 8);
        if (p >= 10u) {
            g_period_ms = p;
        }
    }
}

static void on_ping(const tl_packet *pkt, void *ctx)
{
    tl_link *link = ctx;
    (void)tl_link_send(link, MSG_PONG, pkt->payload, pkt->len);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

int main(void)
{
    const tl_hal hal = {
        .ctx = NULL, .uart_write = board_uart_write, .get_tick_ms = board_get_tick_ms};
    /* This device only sends best-effort telemetry, so session_id is never put
     * on the wire. A node that calls tl_link_send_reliable() must use a value
     * that changes on every boot (hardware RNG, or a boot counter in flash). */
    const tl_link_config cfg = {.retransmit_timeout_ms = 50u, .max_retries = 5u, .session_id = 0u};
    if (tl_link_init(&g_link, &cfg, &hal, g_rx_ring, sizeof g_rx_ring) != TL_OK) {
        for (;;) {
        }
    }
    (void)tl_link_register(&g_link, MSG_SET_PERIOD, on_set_period, NULL);
    (void)tl_link_register(&g_link, MSG_PING, on_ping, &g_link);
    board_init();

    uint32_t last = 0u;
    for (;;) {
        tl_link_poll(&g_link);
        const uint32_t now = g_ms;
        if ((uint32_t)(now - last) >= g_period_ms) {
            last = now;
            uint8_t t[16];
            put_u32(&t[0], now);
            put_u32(&t[4], tl_link_get_stats(&g_link)->rx_bad_crc);
            put_u32(&t[8], tl_link_rx_ring_dropped(&g_link));
            put_u32(&t[12], atomic_load_explicit(&g_uart_overruns, memory_order_relaxed));
            (void)tl_link_send(&g_link, MSG_TELEMETRY, t, sizeof t);
        }
    }
}
