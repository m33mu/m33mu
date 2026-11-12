/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "nrf54lm20/nrf54lm20_uart_spi.h"
#include "nrf54lm20/nrf54lm20_mmio.h"
#include "nrf_uart_spi.h"
#include "nrf54l_mmio.h"

static struct nrf_serial_state g_serials;

static const mm_u32 g_bases[] = {
    0x4004D000u, 0x400C6000u, 0x400C7000u, 0x400C8000u,
    0x400ED000u, 0x400EE000u, 0x40104000u
};
static const int g_irqs[] = { 77, 198, 199, 200, 237, 238, 260 };
static const mm_bool g_has_uarte[] = {
    MM_TRUE, MM_TRUE, MM_TRUE, MM_TRUE, MM_TRUE, MM_TRUE, MM_TRUE
};

static const struct nrf_serial_config g_cfg = {
    g_bases,
    g_irqs,
    g_has_uarte,
    sizeof(g_bases) / sizeof(g_bases[0]),
    0,
    MM_TRUE,
    MM_FALSE,
    MM_FALSE,
    MM_TRUE,
    MM_FALSE,
    MM_FALSE,
    MM_FALSE,
    MM_TRUE,
    nrf54l_dma_sec,
    MM_FALSE
};

void mm_nrf54lm20_usart_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    nrf_serial_register_all(&g_serials, &g_cfg, bus, nvic);
}

void mm_nrf54lm20_usart_reset(void)
{
    nrf_serial_reset_all(&g_serials);
}

void mm_nrf54lm20_usart_poll(void)
{
    nrf_serial_usart_poll(&g_serials);
}

void mm_nrf54lm20_spi_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    nrf_serial_register_all(&g_serials, &g_cfg, bus, nvic);
}

void mm_nrf54lm20_spi_reset(void)
{
    nrf_serial_reset_all(&g_serials);
}

void mm_nrf54lm20_spi_poll(void)
{
    nrf_serial_usart_poll(&g_serials);
}
