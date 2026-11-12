/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/* nRF54L model: SPU peripheral attribution, MPC00 permissions on RRAM and RAM,
 * RRAMC write gating, overwrite-in-place and write buffer, CRACEN RNG FIFO. */

#include <stdio.h>
#include <string.h>
#include "m33mu/mmio.h"
#include "m33mu/memmap.h"
#include "m33mu/target.h"
#include "nrf54l15/cpu_config.h"
#include "nrf54l15/nrf54l15_mmio.h"
#include "nrf54l15/nrf54l15_timers.h"
#include "nrf54l15/nrf54l15_uart_spi.h"
#include "m33mu/target_hal.h"
#include "nrf54l_mmio.h"

#define RRAMC        0x5004B000u
#define SPU20        0x500C0000u
#define MPC00        0x50041000u
#define CRACEN       0x50048000u
#define RNGCTRL      (0x51800000u + 0x1000u)
#define UARTE20_NS   0x400C6000u
#define GRTC_S       0x500E2000u
#define CLOCK_S      0x5010E000u
#define SPU30        0x50100000u
#define P0_NS        0x4010A000u
#define P0_S         0x5010A000u
#define P1_NS        0x400D8200u
#define P1_S         0x500D8200u
#define UARTE20_S    0x500C6000u

static struct mm_memmap map;
static struct mmio_region regions[64];
static mm_u8 flash[NRF54L15_FLASH_SIZE];
static mm_u8 ram[NRF54L15_RAM_SIZE];
static struct mm_target_cfg cfg;
static int failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            failures++;                                                   \
        }                                                                 \
    } while (0)

static mm_u32 rd(mm_u32 addr)
{
    mm_u32 v = 0xDEADBEEFu;
    (void)mmio_bus_read(&map.mmio, addr, 4u, &v);
    return v;
}

static void wr(mm_u32 addr, mm_u32 v)
{
    (void)mmio_bus_write(&map.mmio, addr, 4u, v);
}

static mm_u32 flash_word(mm_u32 off)
{
    return (mm_u32)flash[off] | ((mm_u32)flash[off + 1u] << 8) |
           ((mm_u32)flash[off + 2u] << 16) | ((mm_u32)flash[off + 3u] << 24);
}

static void setup(void)
{
    memset(&cfg, 0, sizeof(cfg));
    cfg.flash_base_s = NRF54L15_FLASH_BASE_S;
    cfg.flash_size_s = NRF54L15_FLASH_SIZE;
    cfg.flash_base_ns = NRF54L15_FLASH_BASE_NS;
    cfg.flash_size_ns = NRF54L15_FLASH_SIZE;
    cfg.ram_base_s = NRF54L15_RAM_BASE_S;
    cfg.ram_size_s = NRF54L15_RAM_SIZE;
    cfg.ram_base_ns = NRF54L15_RAM_BASE_NS;
    cfg.ram_size_ns = NRF54L15_RAM_SIZE;
    memset(flash, 0xFF, sizeof(flash));
    mm_memmap_init(&map, regions, sizeof(regions) / sizeof(regions[0]));
    (void)mm_memmap_configure_flash(&map, &cfg, flash, MM_TRUE);
    (void)mm_memmap_configure_ram(&map, &cfg, ram, MM_TRUE);
    mm_nrf54l15_mmio_reset();
    CHECK(mm_nrf54l15_register_mmio(&map.mmio));
    mm_nrf54l15_flash_bind(&map, flash, NRF54L15_FLASH_SIZE, 0, 0u);
    mmio_set_active_sec(MM_SECURE);
}

static void test_spu(void)
{
    enum mm_sau_attr attr = MM_SAU_NONSECURE;
    mm_u32 region = 0xFFu;
    mm_u32 v = 0u;
    mm_u8 b = 0u;

    CHECK(mm_nrf54l15_tz_attr_for_addr(UARTE20_NS, &attr, &region));
    CHECK(attr == MM_SAU_SECURE);
    /* The secure window is Secure even with the SAU set to all non-secure. */
    attr = MM_SAU_NONSECURE;
    CHECK(mm_nrf54l15_tz_attr_for_addr(UARTE20_NS + 0x10000000u, &attr, &region));
    CHECK(attr == MM_SAU_SECURE);
    CHECK(!mm_nrf54l15_tz_attr_for_addr(0x00050000u, &attr, &region));

    wr(SPU20 + 0x500u + 6u * 4u, rd(SPU20 + 0x500u + 6u * 4u) & ~(1u << 4));
    CHECK(!mm_nrf54l15_tz_attr_for_addr(UARTE20_NS, &attr, &region));
    CHECK((rd(SPU20 + 0x500u + 6u * 4u) & 0x8000000Fu) == 0x8000000Au);

    /* LOCK freezes PERM until reset. */
    wr(SPU20 + 0x500u + 7u * 4u, (1u << 8) | (1u << 4));
    wr(SPU20 + 0x500u + 7u * 4u, 0u);
    CHECK((rd(SPU20 + 0x500u + 7u * 4u) & (1u << 4)) != 0u);

    /* Peripherals answer only at the alias SECATTR selects. */
    CHECK(mm_memmap_read(&map, MM_SECURE, CLOCK_S, 4u, &v));
    CHECK(!mm_memmap_read(&map, MM_SECURE, CLOCK_S - 0x10000000u, 4u, &v));
    CHECK(!mm_memmap_read(&map, MM_NONSECURE, CLOCK_S, 4u, &v));
    CHECK(rd(SPU30 + 0x100u) == 1u);
    CHECK(rd(SPU30 + 0x404u) == CLOCK_S - 0x10000000u);
    wr(SPU30 + 0x500u + 0x0Eu * 4u, rd(SPU30 + 0x500u + 0x0Eu * 4u) & ~(1u << 4));
    CHECK(mm_memmap_read(&map, MM_SECURE, CLOCK_S - 0x10000000u, 4u, &v));
    CHECK(!mm_memmap_read(&map, MM_SECURE, CLOCK_S, 4u, &v));
    wr(SPU30 + 0x500u + 0x0Eu * 4u, rd(SPU30 + 0x500u + 0x0Eu * 4u) | (1u << 4));

    /* EasyDMA reaches RAM only. */
    CHECK(!mm_memmap_dma_read8(&map, MM_SECURE, 0x1000u, &b));
    CHECK(!mm_memmap_dma_read8(&map, MM_SECURE, CLOCK_S, &b));
    CHECK(mm_memmap_dma_read8(&map, MM_SECURE, 0x20000000u, &b));

    /* EasyDMA follows SECATTR and DMASEC. */
    CHECK(nrf54l_dma_sec(UARTE20_NS) == MM_NONSECURE);
    CHECK(nrf54l_dma_sec(UARTE20_NS + 0x1000u) == MM_NONSECURE);
    CHECK(nrf54l_dma_sec(UARTE20_NS + 0x2000u) == MM_SECURE);
}



static void mpc_set(mm_u32 n, mm_u32 start, mm_u32 end, mm_u32 perm)
{
    wr(MPC00 + 0x804u + n * 0x20u, start);
    wr(MPC00 + 0x808u + n * 0x20u, end);
    wr(MPC00 + 0x810u + n * 0x20u, perm);
    wr(MPC00 + 0x814u + n * 0x20u, 0xFu);
    wr(MPC00 + 0x800u + n * 0x20u, 1u << 9);
}

static mm_bool ns_read(mm_u32 addr)
{
    mm_u32 v = 0u;
    return mm_memmap_read(&map, MM_NONSECURE, addr, 4u, &v);
}

static enum mm_sec_state bus_attr_ns(void *opaque, mm_u32 addr)
{
    (void)opaque;
    (void)addr;
    return MM_NONSECURE;
}

static void test_mpc(void)
{
    mm_u32 v = 0u;
    mm_u8 b = 0u;

    /* RRAM and RAM are Secure out of reset. */
    CHECK(!ns_read(0x20010000u));
    CHECK(!ns_read(0x00060000u));
    CHECK(mm_memmap_read(&map, MM_SECURE, 0x20010000u, 4u, &v));
    CHECK(rd(MPC00 + 0x100u) == 1u);
    CHECK(rd(MPC00 + 0x400u) == 0x20010000u);
    wr(MPC00 + 0x100u, 0u);

    mpc_set(0, 0x00050000u, 0x000DB000u, 0x7u);
    mpc_set(1, 0x20010000u, 0x20040000u, 0x7u);
    CHECK(ns_read(0x20010000u));
    CHECK(!ns_read(0x2000F000u));
    /* An access straddling the edge of a non-secure region is refused. */
    CHECK(!ns_read(0x2000FFFEu));
    wr(RRAMC + 0x500u, 1u);
    CHECK(mm_memmap_write(&map, MM_NONSECURE, 0x000DAFF0u, 4u, 0u));
    CHECK(!mm_memmap_write(&map, MM_NONSECURE, 0x000DAFFEu, 4u, 0u));
    wr(RRAMC + 0x500u, 0u);
    CHECK(ns_read(0x00060000u));
    CHECK(!ns_read(0x00001000u));
    CHECK(!ns_read(0x000DB000u));
    CHECK(!mm_memmap_fetch_read16(&map, MM_NONSECURE, 0x1000u, &v));
    CHECK(mm_memmap_fetch_read16(&map, MM_NONSECURE, 0x60000u, &v));
    CHECK(mm_memmap_fetch_read16(&map, MM_SECURE, 0x1000u, &v));

    /* EasyDMA keeps its own attribute instead of the SAU attribution. */
    map.bus_attr = bus_attr_ns;
    CHECK(!mm_memmap_read(&map, MM_SECURE, 0x2000F000u, 4u, &v));
    CHECK(mm_memmap_dma_read8(&map, MM_SECURE, 0x2000F000u, &b));
    CHECK(!mm_memmap_dma_read8(&map, MM_NONSECURE, 0x2000F000u, &b));
    CHECK(mm_memmap_dma_write8(&map, MM_NONSECURE, 0x20010000u, 0x5Au));
    map.bus_attr = 0;
    wr(MPC00 + 0x100u, 0u);

    /* A refused access leaves the RRAM write buffer alone. */
    wr(RRAMC + 0x500u, 1u | (4u << 8));
    CHECK(mm_memmap_write(&map, MM_SECURE, 0x1000u, 4u, 0x11u));
    CHECK(!ns_read(0x1000u));
    CHECK(rd(RRAMC + 0x418u) == 0u);
    wr(RRAMC + 0x008u, 1u);
    wr(RRAMC + 0x500u, 0u);
    wr(MPC00 + 0x100u, 0u);

    /* Overlaps OR their permissions: Secure wins in either index order. */
    mpc_set(2, 0x00060000u, 0x00061000u, 0xFu);
    mpc_set(3, 0x00060000u, 0x00061000u, 0x7u);
    CHECK(!ns_read(0x00060000u));
    CHECK(ns_read(0x00061000u));

    /* READ/WRITE/EXECUTE are enforced. */
    mpc_set(4, 0x20004000u, 0x20005000u, 0x1u);
    CHECK(ns_read(0x20004000u));
    CHECK(!mm_memmap_write(&map, MM_NONSECURE, 0x20004000u, 4u, 0u));
    CHECK(!mm_memmap_fetch_read16(&map, MM_NONSECURE, 0x20004000u, &v));
    CHECK(mm_memmap_write(&map, MM_NONSECURE, 0x20031000u, 4u, 0u));

    /* OVERRIDE[5] blocks every access to its range, Secure ones too. */
    mpc_set(5, 0x20012000u, 0x20013000u, 0xFu);
    CHECK(!mm_memmap_read(&map, MM_SECURE, 0x20012000u, 4u, &v));
    CHECK(!mm_memmap_dma_read8(&map, MM_SECURE, 0x20012000u, &b));
    CHECK(mm_memmap_read(&map, MM_SECURE, 0x20013000u, 4u, &v));

    /* OVERRIDE[6] is reserved on the nRF54L15. */
    mpc_set(6, 0x00000000u, 0x00001000u, 0x7u);
    CHECK(rd(MPC00 + 0x804u + 6u * 0x20u) == 0u);
    CHECK(!ns_read(0x00000000u));
}

static void test_rramc(void)
{
    mm_u32 v = 0u;

    CHECK(rd(RRAMC + 0x400u) == 1u);
    CHECK(rd(RRAMC + 0x404u) == 1u);
    CHECK(rd(RRAMC + 0x418u) == 1u);

    /* Stores need CONFIG.WEN. */
    CHECK(!mm_memmap_write(&map, MM_SECURE, 0x2000u, 4u, 0x12345678u));
    CHECK(rd(RRAMC + 0x10Cu) == 1u);
    CHECK(rd(RRAMC + 0x408u) == 0x2000u);
    CHECK(flash_word(0x2000u) == 0xFFFFFFFFu);

    wr(RRAMC + 0x500u, 1u);
    CHECK(mm_memmap_write(&map, MM_SECURE, 0x2000u, 4u, 0x12345678u));
    CHECK(flash_word(0x2000u) == 0x12345678u);
    /* Overwritten in place, not ANDed like NOR flash. */
    CHECK(mm_memmap_write(&map, MM_SECURE, 0x2000u, 4u, 0xFFFFFFFFu));
    CHECK(flash_word(0x2000u) == 0xFFFFFFFFu);
    CHECK(rd(RRAMC + 0x418u) == 1u);

    /* WRITEBUFSIZE=2: data waits in the buffer until committed. */
    wr(RRAMC + 0x500u, 1u | (2u << 8));
    CHECK(mm_memmap_write(&map, MM_SECURE, 0x2010u, 4u, 0xA5A5A5A5u));
    CHECK(rd(RRAMC + 0x418u) == 0u);
    wr(RRAMC + 0x008u, 1u);
    CHECK(rd(RRAMC + 0x418u) == 1u);
    CHECK(flash_word(0x2010u) == 0xA5A5A5A5u);
    /* Partial lines wait; filling every byte of the buffer commits. */
    for (v = 0x2010u; v < 0x2030u; v += 4u) {
        CHECK(mm_memmap_write(&map, MM_SECURE, v, 4u, v));
        if (v == 0x2024u) CHECK(rd(RRAMC + 0x418u) == 0u);
    }
    CHECK(rd(RRAMC + 0x418u) == 1u);
    /* A store outside the buffered area commits the buffer first. */
    CHECK(mm_memmap_write(&map, MM_SECURE, 0x2010u, 4u, 3u));
    CHECK(mm_memmap_write(&map, MM_SECURE, 0x3000u, 4u, 4u));
    CHECK(rd(RRAMC + 0x418u) == 0u);
    /* Reading a buffered line back commits it. */
    CHECK(mm_memmap_read(&map, MM_SECURE, 0x3000u, 4u, &v));
    CHECK(v == 4u);
    CHECK(rd(RRAMC + 0x418u) == 1u);
    wr(RRAMC + 0x500u, 0u);

    /* ERASEALL needs no CONFIG.WEN. */
    wr(RRAMC + 0x540u, 1u);
    CHECK(flash_word(0x3000u) == 0xFFFFFFFFu);

    /* FICR is factory-programmed. */
    CHECK(!mmio_bus_write(&map.mmio, 0x00FFC328u, 4u, 0u));
    CHECK(rd(0x00FFC328u) == 0x100u);
}

static void test_rng(void)
{
    mm_u32 a;
    mm_u32 b;

    wr(RNGCTRL + 0x000u, 1u);
    CHECK(((rd(RNGCTRL + 0x030u) >> 1) & 7u) == 0u);
    CHECK(rd(RNGCTRL + 0x004u) == 0u);

    wr(CRACEN + 0x400u, 1u << 1);
    wr(RNGCTRL + 0x000u, 1u << 8);
    wr(RNGCTRL + 0x000u, 1u | (4u << 16));
    CHECK(((rd(RNGCTRL + 0x030u) >> 1) & 7u) == 2u);
    CHECK(rd(RNGCTRL + 0x004u) == 16u);
    a = rd(RNGCTRL + 0x080u);
    b = rd(RNGCTRL + 0x084u);
    CHECK(a != b);

    wr(RNGCTRL + 0x000u, 0u);
    CHECK(rd(RNGCTRL + 0x004u) == 0u);
    CHECK(rd(RNGCTRL + 0x080u) == 0u);
}

static const mm_u8 *rx_feed;
static size_t rx_feed_len;

static size_t uart_tx_sink(void *opaque, const mm_u8 *data, size_t len)
{
    (void)opaque;
    (void)data;
    return len;
}

static mm_bool uart_rx_source(void *opaque, mm_u8 *byte_out)
{
    (void)opaque;
    if (rx_feed_len == 0u) return MM_FALSE;
    *byte_out = *rx_feed++;
    rx_feed_len--;
    return MM_TRUE;
}

static const struct mm_uart_backend_ops uart_ops = { uart_tx_sink, uart_rx_source, 0 };

static void rx_start(mm_u32 maxcnt, const char *feed)
{
    mm_u32 i;

    rx_feed = (const mm_u8 *)feed;
    rx_feed_len = strlen(feed);
    wr(UARTE20_S + 0x14Cu, 0u);
    wr(UARTE20_S + 0x124u, 0u);
    wr(UARTE20_S + 0x704u, 0x20020000u);
    wr(UARTE20_S + 0x708u, maxcnt);
    wr(UARTE20_S + 0x028u, 1u);
    for (i = 0; i < 8u; ++i) {
        mm_nrf54l15_usart_poll();
    }
}

/* TASKS_DMA.RX.STOP raises RXTO but not END; a full buffer or FLUSHRX does. */
static void test_uart_rx(void)
{
    mm_nrf54l15_usart_reset();
    mm_nrf54l15_usart_init(&map.mmio, 0);
    CHECK(mm_uart_backend_attach(UARTE20_NS, "test", &uart_ops, 0));
    wr(UARTE20_S + 0x500u, 8u);

    rx_start(8u, "AB");
    wr(UARTE20_S + 0x02Cu, 1u);
    CHECK(rd(UARTE20_S + 0x124u) == 1u);
    CHECK(rd(UARTE20_S + 0x14Cu) == 0u);
    CHECK(rd(UARTE20_S + 0x70Cu) == 2u);

    rx_start(2u, "XY");
    CHECK(rd(UARTE20_S + 0x14Cu) == 1u);
    CHECK(rd(UARTE20_S + 0x70Cu) == 2u);

    rx_start(8u, "");
    wr(UARTE20_S + 0x02Cu, 1u);
    rx_feed = (const mm_u8 *)"C";
    rx_feed_len = 1u;
    mm_nrf54l15_usart_poll();
    wr(UARTE20_S + 0x01Cu, 1u);
    CHECK(rd(UARTE20_S + 0x14Cu) == 1u);
    CHECK(rd(UARTE20_S + 0x70Cu) == 1u);
    mm_uart_backend_detach(UARTE20_NS);
}

/* Split peripherals: the NS alias reaches what the per-pin / per-feature
 * SPU entries make non-secure, whatever PERIPH SECATTR says. */
static void test_split_ns(void)
{
    enum mm_sau_attr attr = MM_SAU_SECURE;
    mm_u32 region = 0u;
    mm_u32 v = 0u;

    CHECK(rd(SPU20 + 0x800u + 0x80u) == 0x00100010u);
    CHECK(rd(SPU20 + 0xD00u) == 0x00100010u);
    CHECK(rd(SPU20 + 0x600u) == 0u);
    wr(SPU20 + 0x600u, 0x10u);
    CHECK(rd(SPU20 + 0x600u) == 0u);
    CHECK(!mm_nrf54l15_tz_attr_for_addr(P0_NS, &attr, &region));
    CHECK(!mm_nrf54l15_tz_attr_for_addr(GRTC_S - 0x10000000u, &attr, &region));

    /* GPIO P0 stays Secure; pin 6 alone is made non-secure. */
    CHECK(rd(SPU30 + 0x800u) == 0x00100010u);
    wr(SPU30 + 0x800u + 6u * 4u, 0u);
    CHECK(mm_memmap_write(&map, MM_NONSECURE, P0_NS + 0x004u, 4u, 0xFFFFFFFFu));
    CHECK(mm_memmap_read(&map, MM_NONSECURE, P0_NS + 0x000u, 4u, &v));
    CHECK(v == (1u << 6));
    CHECK(mm_memmap_read(&map, MM_SECURE, P0_S + 0x000u, 4u, &v));
    CHECK(v == (1u << 6));
    CHECK(!mm_memmap_read(&map, MM_NONSECURE, P0_NS + 0x024u, 4u, &v));
    CHECK(!mm_memmap_read(&map, MM_NONSECURE, P0_S + 0x000u, 4u, &v));

    /* GRTC stays Secure; CC[1] and SYSCOUNTER are made non-secure. */
    wr(SPU20 + 0xD00u + 1u * 4u, 0u);
    CHECK(rd(SPU20 + 0xD00u + 1u * 4u) == 0x00100000u);
    wr(SPU20 + 0xD7Cu, 0u);
    CHECK(mm_memmap_write(&map, MM_NONSECURE, GRTC_S - 0x10000000u + 0x530u, 4u, 1234u));
    CHECK(mm_memmap_read(&map, MM_NONSECURE, GRTC_S - 0x10000000u + 0x720u, 4u, &v));
    CHECK(!mm_memmap_write(&map, MM_NONSECURE, GRTC_S - 0x10000000u + 0x520u, 4u, 1u));
    CHECK(!mm_memmap_read(&map, MM_NONSECURE, GRTC_S - 0x10000000u + 0x300u, 4u, &v));
    CHECK(!mm_memmap_write(&map, MM_NONSECURE, GRTC_S - 0x10000000u + 0x060u, 4u, 1u));
    /* INTERVAL shares CC[0]'s attribute, not the peripheral's. */
    CHECK(!mm_memmap_write(&map, MM_NONSECURE, GRTC_S - 0x10000000u + 0x6A8u, 4u, 1u));
    wr(SPU20 + 0xD00u, 0u);
    CHECK(mm_memmap_write(&map, MM_NONSECURE, GRTC_S - 0x10000000u + 0x6A8u, 4u, 0u));
    wr(SPU20 + 0xD00u, 0x10u);
    CHECK(rd(SPU20 + 0x100u) == 1u);
    CHECK(mm_memmap_read(&map, MM_SECURE, GRTC_S + 0x530u, 4u, &v));
    CHECK(v == 1234u);
    wr(SPU20 + 0x100u, 0u);
    wr(SPU30 + 0x100u, 0u);
}

/* TIMER20 counts PCLK16M, TIMER10 PCLK32M and has CC[0..7]. */
static void test_timers(void)
{
    mm_nrf54l15_timers_init(&map.mmio, 0);
    mm_nrf54l15_timers_reset();
    wr(0x400CA000u + 0x508u, 3u);
    wr(0x400CA000u + 0x510u, 0u);
    wr(0x400CA000u + 0x000u, 1u);
    wr(0x40085000u + 0x508u, 3u);
    wr(0x40085000u + 0x510u, 0u);
    wr(0x40085000u + 0x540u + 7u * 4u, 1000u);
    CHECK(rd(0x40085000u + 0x540u + 7u * 4u) == 1000u);
    wr(0x40085000u + 0x000u, 1u);
    mm_nrf54l15_timers_tick(mm_nrf54l15_cpu_hz() / 1000u);
    CHECK(rd(0x40085000u + 0x140u + 7u * 4u) == 1u);
    CHECK(rd(0x400CA000u + 0x008u) == 16000u);
    CHECK(rd(0x40085000u + 0x008u) == 32000u);
}

/* The non-secure alias of a split GPIO port only reaches non-secure pins. */
static void test_gpio_split(void)
{
    mm_u32 v = 0u;

    CHECK(mm_memmap_write(&map, MM_SECURE, P1_S + 0x004u, 4u, 0x0000000Fu));
    CHECK(mm_memmap_read(&map, MM_SECURE, P1_NS + 0x000u, 4u, &v));
    CHECK(v == 0u);
    wr(SPU20 + 0x800u + 0x80u + 2u * 4u, 0u);
    CHECK(mm_memmap_read(&map, MM_SECURE, P1_NS + 0x000u, 4u, &v));
    CHECK(v == 0x4u);
    CHECK(mm_memmap_write(&map, MM_SECURE, P1_NS + 0x008u, 4u, 0x0000000Fu));
    CHECK(mm_memmap_read(&map, MM_SECURE, P1_S + 0x000u, 4u, &v));
    CHECK(v == 0xBu);
    CHECK(mm_memmap_read(&map, MM_SECURE, P1_NS + 0x080u + 3u * 4u, 4u, &v));
    CHECK(v == 0u);
}

int main(void)
{
    mm_u32 v = 0u;

    setup();
    test_uart_rx();
    test_split_ns();
    test_gpio_split();
    test_timers();
    test_spu();
    test_rramc();
    test_mpc();
    test_rng();

    /* A read reaching into a buffered line commits it, even unaligned. */
    wr(RRAMC + 0x500u, 1u | (1u << 8));
    CHECK(mm_memmap_write(&map, MM_SECURE, 0x5010u, 4u, 0u));
    CHECK(mm_memmap_read(&map, MM_SECURE, 0x500Eu, 4u, &v));
    CHECK(rd(RRAMC + 0x418u) == 1u);

    /* A reset drops what the RRAMC write buffer never committed. */
    CHECK(mm_memmap_write(&map, MM_SECURE, 0x5000u, 4u, 0u));
    CHECK(rd(RRAMC + 0x418u) == 0u);
    mm_nrf54l15_mmio_reset();
    CHECK(flash_word(0x5000u) == 0xFFFFFFFFu);
    CHECK(flash_word(0x5010u) == 0u);
    if (failures != 0) {
        printf("nrf54l_security_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("nrf54l_security_test: PASS\n");
    return 0;
}
