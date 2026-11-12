/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "nrf5340net/nrf5340net_uart_spi.h"
#include "nrf5340/nrf5340_mmio.h"
#include "nrf_uart_spi.h"

static struct nrf_serial_state g_serials;

static mm_bool nrf5340net_clock_ready(void)
{
    return mm_nrf5340_clock_hf_running();
}

/* SERIAL0: UARTE0/SPIM0/SPIS0/TWIM0/TWIS0 share one base */
static const mm_u32 g_bases[] = { 0x41013000u };
static const int g_irqs[] = { 19 };
static const mm_bool g_has_uarte[] = { MM_TRUE };

static const struct nrf_serial_config g_cfg = {
    g_bases,
    g_irqs,
    g_has_uarte,
    sizeof(g_bases) / sizeof(g_bases[0]),
    nrf5340net_clock_ready,
    MM_TRUE,
    MM_FALSE,
    MM_TRUE,
    MM_TRUE,
    MM_FALSE,
    MM_FALSE,
    MM_FALSE,
    MM_TRUE
};

void mm_nrf5340net_usart_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    nrf_serial_register_all(&g_serials, &g_cfg, bus, nvic);
}

void mm_nrf5340net_usart_reset(void)
{
    nrf_serial_reset_all(&g_serials);
}

void mm_nrf5340net_usart_poll(void)
{
    nrf_serial_usart_poll(&g_serials);
}

void mm_nrf5340net_spi_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    nrf_serial_register_all(&g_serials, &g_cfg, bus, nvic);
}

void mm_nrf5340net_spi_reset(void)
{
    nrf_serial_reset_all(&g_serials);
}

void mm_nrf5340net_spi_poll(void)
{
    nrf_serial_usart_poll(&g_serials);
}
