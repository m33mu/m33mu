/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "nrf5340/nrf5340_uart_spi.h"
#include "nrf5340/nrf5340_mmio.h"
#include "nrf_uart_spi.h"

static struct nrf_serial_state g_serials;

static mm_bool nrf5340_clock_ready(void)
{
    return mm_nrf5340_clock_hf_running();
}

static const mm_u32 g_bases[] = {
    0x40008000u, 0x40009000u, 0x4000B000u, 0x4000C000u, 0x4000A000u
};
static const int g_irqs[] = { 8, 9, 11, 12, 10 };
static const mm_bool g_has_uarte[] = { MM_TRUE, MM_TRUE, MM_TRUE, MM_TRUE, MM_FALSE };

static const struct nrf_serial_config g_cfg = {
    g_bases,
    g_irqs,
    g_has_uarte,
    sizeof(g_bases) / sizeof(g_bases[0]),
    nrf5340_clock_ready,
    MM_TRUE,
    MM_FALSE,
    MM_TRUE,
    MM_TRUE,
    MM_FALSE,
    MM_FALSE,
    MM_FALSE,
    MM_FALSE
};

void mm_nrf5340_usart_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    nrf_serial_register_all(&g_serials, &g_cfg, bus, nvic);
}

void mm_nrf5340_usart_reset(void)
{
    nrf_serial_reset_all(&g_serials);
}

void mm_nrf5340_usart_poll(void)
{
    nrf_serial_usart_poll(&g_serials);
}

void mm_nrf5340_spi_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    nrf_serial_register_all(&g_serials, &g_cfg, bus, nvic);
}

void mm_nrf5340_spi_reset(void)
{
    nrf_serial_reset_all(&g_serials);
}

void mm_nrf5340_spi_poll(void)
{
    nrf_serial_usart_poll(&g_serials);
}
