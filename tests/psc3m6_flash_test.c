/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/* PSOC Control C3M6 model: PERI_PCLK divider start, SCB clock gating, the
 * HSIOM non-secure pin mask and the boot ROM flash API row operations. */

#include <stdio.h>
#include <string.h>
#include "m33mu/mmio.h"
#include "m33mu/memmap.h"
#include "m33mu/cpu.h"
#include "m33mu/target.h"
#include "psc3m6/cpu_config.h"
#include "psc3m6/psc3m6_mmio.h"
#include "psc3m6/psc3m6_romapi.h"

#define S(addr) ((addr) | 0x10000000u)

#define PERI_GR_SL_CTL(g) (0x42004000u + (g) * 0x40u + 0x10u)
#define PCLK_GR4        0x42048000u
#define PCLK_DIV_CMD    (PCLK_GR4 + 0x000u)
#define PCLK_CLOCK_CTL6 (PCLK_GR4 + 0xC00u + 6u * 4u)
#define PCLK_DIV_16_5_1 (PCLK_GR4 + 0x1800u + 1u * 4u)
#define SCB3_BASE       0x42860000u
#define SCB3_CTRL       (SCB3_BASE + 0x000u)
#define SCB3_TX_FIFO_STATUS (SCB3_BASE + 0x208u)
#define HSIOM_PRT6_SEL0 0x42400060u
#define HSIOM_SEC_PRT6  0x42401060u
#define GPIO_PRT6_CFG   (0x42410000u + 6u * 0x80u + 0x44u)
#define GPIO_PRT6_OUT   (0x42410000u + 6u * 0x80u + 0x00u)

#define TEST_ROW_OFFSET 0x40000u
#define CTX_ADDR        0x34000000u
#define REFRESH_ADDR    0x34000100u
#define DATA_ADDR       0x34001000u

static struct mm_memmap map;
static struct mmio_region regions[64];
static mm_u8 flash[PSC3M6_FLASH_SIZE];
static mm_u8 ram[PSC3M6_RAM_SIZE];
static struct mm_target_cfg cfg;
static struct mm_cpu cpu;

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

static mm_bool wr(mm_u32 addr, mm_u32 v)
{
    return mmio_bus_write(&map.mmio, addr, 4u, v);
}

static mm_u32 ram_rd(mm_u32 addr)
{
    mm_u32 v = 0u;
    (void)mm_memmap_read(&map, MM_SECURE, addr, 4u, &v);
    return v;
}

static void ram_wr(mm_u32 addr, mm_u32 v)
{
    (void)mm_memmap_write(&map, MM_SECURE, addr, 4u, v);
}

static void setup(void)
{
    memset(&cfg, 0, sizeof(cfg));
    memset(ram, 0, sizeof(ram));
    memset(flash, 0xFF, sizeof(flash));
    cfg.flash_base_s = PSC3M6_FLASH_BASE_S;
    cfg.flash_size_s = sizeof(flash);
    cfg.flash_base_ns = PSC3M6_FLASH_BASE_NS;
    cfg.flash_size_ns = sizeof(flash);
    cfg.flash_base_s2 = PSC3M6_FLASH_BASE_S2;
    cfg.flash_size_s2 = sizeof(flash);
    cfg.ram_base_s = PSC3M6_RAM_BASE_S;
    cfg.ram_size_s = PSC3M6_RAM_SIZE;
    cfg.ram_base_ns = PSC3M6_RAM_BASE_NS;
    cfg.ram_size_ns = PSC3M6_RAM_SIZE;
    cfg.ram_regions = PSC3M6_RAM_REGIONS;
    cfg.ram_region_count = PSC3M6_RAM_REGION_COUNT;
    mm_memmap_init(&map, regions, sizeof(regions) / sizeof(regions[0]));
    (void)mm_memmap_configure_flash(&map, &cfg, flash, MM_TRUE);
    (void)mm_memmap_configure_flash(&map, &cfg, flash, MM_FALSE);
    (void)mm_memmap_configure_ram(&map, &cfg, ram, MM_TRUE);
    (void)mm_memmap_configure_ram(&map, &cfg, ram, MM_FALSE);
    if (!mm_psc3m6_register_mmio(&map.mmio)) {
        printf("FAIL: register_mmio\n");
        failures++;
    }
    mm_psc3m6_flash_bind(&map, flash, sizeof(flash), 0, 0);
    mm_psc3m6_scb_init(&map.mmio, 0);
    mmio_set_active_sec(MM_SECURE);
    memset(&cpu, 0, sizeof(cpu));
    cpu.sec_state = MM_SECURE;
}

static void test_pclk_and_scb_gate(void)
{
    /* The SCB bus-faults until its clock destination has a started divider. */
    mm_u32 v = 0u;
    /* Reset: every clock destination disconnected, groups 1.. gated. */
    CHECK(rd(S(PCLK_CLOCK_CTL6)) == 0x3FFu);
    CHECK(rd(S(PCLK_DIV_CMD)) == 0x03FF03FFu);
    CHECK(rd(S(PERI_GR_SL_CTL(0))) == 0xFFFFFFFFu);
    CHECK(rd(S(PERI_GR_SL_CTL(3))) == 0u);
    CHECK(!wr(S(SCB3_CTRL), 0u));
    CHECK(!mmio_bus_read(&map.mmio, S(SCB3_TX_FIFO_STATUS), 4u, &v));
    /* Ungating the slave is not enough: the SCB still has no clock. */
    CHECK(wr(S(PERI_GR_SL_CTL(3)), 0xFFFFFFFFu));
    CHECK(!wr(S(SCB3_CTRL), 0u));
    /* Writing the divider register does not start it: EN is status. */
    CHECK(wr(S(PCLK_DIV_16_5_1), (51u << 8) | (1u << 0)));
    CHECK((rd(S(PCLK_DIV_16_5_1)) & 1u) == 0u);
    CHECK(wr(S(PCLK_CLOCK_CTL6), (2u << 8) | 1u));
    CHECK(!wr(S(SCB3_CTRL), 0u));
    /* DIV_CMD.ENABLE for the 16.5 divider 1 starts it and reads back clear. */
    CHECK(wr(S(PCLK_DIV_CMD), (1u << 31) | (3u << 24) | (0xFFu << 16) | (2u << 8) | 1u));
    CHECK((rd(S(PCLK_DIV_16_5_1)) & 1u) == 1u);
    CHECK((rd(S(PCLK_DIV_CMD)) & (1u << 31)) == 0u);
    CHECK(mm_psc3m6_pclk_active(4u, 6u));
    /* Divider 1 is only routed to destination 6: SCB2 (index 4) stays off. */
    CHECK(!mm_psc3m6_pclk_active(4u, 4u));
    CHECK(!wr(S(0x42850000u), 0u));
    /* A clocked SCB in a gated group still faults. */
    CHECK(wr(S(PERI_GR_SL_CTL(3)), 0u));
    CHECK(!wr(S(SCB3_CTRL), 0u));
    CHECK(wr(S(PERI_GR_SL_CTL(3)), 0xFFFFFFFFu));
    CHECK(wr(S(SCB3_CTRL), (1u << 31) | (2u << 24) | 7u));
    CHECK(rd(S(SCB3_CTRL)) == ((1u << 31) | (2u << 24) | 7u));
    CHECK((rd(S(SCB3_TX_FIFO_STATUS)) & 0x1FFu) == 0u);
    /* DISABLE stops it again. */
    CHECK(wr(S(PCLK_DIV_CMD), (1u << 30) | (2u << 8) | 1u));
    CHECK(!mm_psc3m6_pclk_active(4u, 6u));
    CHECK(wr(S(PCLK_DIV_CMD), (1u << 31) | (2u << 8) | 1u));
}

static void test_hsiom_ns_mask(void)
{
    mm_u32 v = 0u;
    /* HSIOM and GPIO sit in group 1, gated out of reset. */
    CHECK(!mmio_bus_read(&map.mmio, S(HSIOM_SEC_PRT6), 4u, &v));
    CHECK(!wr(S(GPIO_PRT6_CFG), 0u));
    CHECK(wr(S(PERI_GR_SL_CTL(1)), 0xFFFFFFFFu));
    /* Pins start non-secure: a Secure write to their mux is dropped. */
    CHECK(rd(S(HSIOM_SEC_PRT6)) == 0xFFu);
    CHECK(wr(S(HSIOM_PRT6_SEL0), 0x00101000u));
    CHECK(rd(S(HSIOM_PRT6_SEL0)) == 0u);
    CHECK(wr(S(GPIO_PRT6_CFG), 0x6800u));
    CHECK(rd(S(GPIO_PRT6_CFG)) == 0u);
    /* Claim pins 2 and 3 and the writes land; pin 1 stays masked. */
    CHECK(wr(S(HSIOM_SEC_PRT6), 0xFFu & ~((1u << 2) | (1u << 3))));
    CHECK(wr(S(HSIOM_PRT6_SEL0), 0x10100A00u));
    CHECK(rd(S(HSIOM_PRT6_SEL0)) == 0x10100000u);
    CHECK(wr(S(GPIO_PRT6_CFG), 0x6800u | 0x00F0u));
    CHECK(rd(S(GPIO_PRT6_CFG)) == 0x6800u);
    /* The Non-secure alias sees only the pins still in the mask: it cannot
     * reroute the claimed pins 2 and 3 and reads them as zero. */
    CHECK(wr(HSIOM_PRT6_SEL0, 0x0B0B0A0Bu));
    CHECK(rd(HSIOM_PRT6_SEL0) == 0x00000A0Bu);
    CHECK(rd(S(HSIOM_PRT6_SEL0)) == 0x10100000u);
    CHECK(wr(GPIO_PRT6_CFG, 0xFFFFu));
    CHECK(rd(GPIO_PRT6_CFG) == 0x00FFu);
    CHECK(rd(S(GPIO_PRT6_CFG)) == 0x6800u);
    CHECK(wr(GPIO_PRT6_OUT, 0xFFu));
    CHECK(rd(GPIO_PRT6_OUT) == 0xF3u);
    CHECK(rd(S(GPIO_PRT6_OUT)) == 0x00u);
    CHECK(wr(S(GPIO_PRT6_OUT), 0xFFu));
    CHECK(rd(S(GPIO_PRT6_OUT)) == 0x0Cu);
    CHECK(rd(GPIO_PRT6_OUT) == 0xF3u);
    /* The last port, 13, is covered by the same rules. */
    CHECK(rd(S(0x424010D0u)) == 0xFFu);
    CHECK(wr(S(0x424010D0u), 0xFEu));
    CHECK(wr(S(0x424000D0u), 0x1Fu));
    CHECK(rd(S(0x424000D0u)) == 0x1Fu);
    CHECK(wr(0x424000D0u, 0xFFFFFFFFu));
    CHECK(rd(0x424000D0u) == 0xFFFFFF00u);
    CHECK(rd(S(0x424000D0u)) == 0x1Fu);
}

static mm_u32 rom_call(mm_u32 func, mm_u32 r0, mm_u32 r1, mm_u32 r2)
{
    mm_u32 entry = rd(PSC3M6_ROM_FUNC_TABLE + func * 4u);
    cpu.r[0] = r0;
    cpu.r[1] = r1;
    cpu.r[2] = r2;
    cpu.r[14] = 0x12000401u;
    cpu.r[15] = entry;
    if (!mm_psc3m6_romapi_handle(&cpu, &map)) {
        return 0u;
    }
    CHECK(cpu.r[15] == 0x12000401u);
    return cpu.r[0];
}

static void test_romapi(void)
{
    mm_u32 i;
    mm_u32 entry;
    mm_u32 sbus = PSC3M6_FLASH_BASE_S2 + TEST_ROW_OFFSET;
    mm_u32 cbus = PSC3M6_FLASH_BASE_S + TEST_ROW_OFFSET;

    /* Every table entry is a Thumb pointer inside the 64 KB ROM. */
    for (i = 0; i < PSC3M6_ROM_FUNC_COUNT; ++i) {
        entry = rd(PSC3M6_ROM_FUNC_TABLE + i * 4u);
        CHECK((entry & 1u) == 1u);
        CHECK(entry >= PSC3M6_ROM_BASE_S && entry < PSC3M6_ROM_BASE_S + PSC3M6_ROM_SIZE);
    }
    CHECK(rd(PSC3M6_ROM_FUNC_TABLE + PSC3M6_ROM_FUNC_ERASE_ROW * 4u) ==
          rd((PSC3M6_ROM_FUNC_TABLE & ~0x10000000u) + PSC3M6_ROM_FUNC_ERASE_ROW * 4u));
    CHECK(!wr(PSC3M6_ROM_FUNC_TABLE, 0u));

    /* Blocking context with the refresh table behind it. */
    ram_wr(CTX_ADDR + 0x0u, 0u);
    ram_wr(CTX_ADDR + 0x8u, REFRESH_ADDR);
    for (i = 0; i < PSC3M6_FLASH_ROW_SIZE; i += 4u) {
        ram_wr(DATA_ADDR + i, 0xA5000000u | i);
    }

    /* CBUS addresses, unaligned rows and, from a Non-secure caller, the
     * Secure alias are refused. */
    CHECK(rom_call(PSC3M6_ROM_FUNC_ERASE_ROW, cbus, CTX_ADDR, 0u) == CYBOOT_FLASH_ADDR_INVALID);
    cpu.sec_state = MM_NONSECURE;
    CHECK(rom_call(PSC3M6_ROM_FUNC_ERASE_ROW, sbus, CTX_ADDR, 0u) == CYBOOT_FLASH_ADDR_INVALID);
    cpu.sec_state = MM_SECURE;
    CHECK(rom_call(PSC3M6_ROM_FUNC_ERASE_ROW, sbus + 4u, CTX_ADDR, 0u) == CYBOOT_FLASH_ADDR_INVALID);
    CHECK(rom_call(PSC3M6_ROM_FUNC_ERASE_ROW, sbus, 0u, 0u) == CYBOOT_FLASH_PARAM_INVALID);
    CHECK(flash[TEST_ROW_OFFSET] == 0xFFu);

    /* Erase leaves the row at 0x00. */
    CHECK(rom_call(PSC3M6_ROM_FUNC_ERASE_ROW, sbus, CTX_ADDR, 0u) == CYBOOT_FLASH_SUCCESS);
    for (i = 0; i < PSC3M6_FLASH_ROW_SIZE; ++i) {
        if (flash[TEST_ROW_OFFSET + i] != 0x00u) break;
    }
    CHECK(i == PSC3M6_FLASH_ROW_SIZE);
    CHECK(flash[TEST_ROW_OFFSET + PSC3M6_FLASH_ROW_SIZE] == 0xFFu);
    CHECK(ram_rd(REFRESH_ADDR + (TEST_ROW_OFFSET / PSC3M6_FLASH_SECTOR_SIZE) * 16u + 4u) == 1u);

    /* Program sets bits; the data lands and the CBUS view shows it. */
    CHECK(rom_call(PSC3M6_ROM_FUNC_PROGRAM_ROW, sbus, DATA_ADDR, CTX_ADDR) == CYBOOT_FLASH_SUCCESS);
    CHECK(ram_rd(cbus + 8u) == 0xA5000008u);
    CHECK(ram_rd(sbus + 8u) == 0xA5000008u);
    ram_wr(DATA_ADDR + 8u, 0x0000F000u);
    CHECK(rom_call(PSC3M6_ROM_FUNC_PROGRAM_ROW, sbus, DATA_ADDR, CTX_ADDR) == CYBOOT_FLASH_SUCCESS);
    CHECK(ram_rd(cbus + 8u) == 0xA500F008u);

    /* Write erases first, so the row holds exactly the buffer. */
    CHECK(rom_call(PSC3M6_ROM_FUNC_WRITE_ROW, sbus, DATA_ADDR, CTX_ADDR) == CYBOOT_FLASH_SUCCESS);
    CHECK(ram_rd(cbus + 8u) == 0x0000F000u);
    CHECK(ram_rd(cbus + 12u) == 0xA500000Cu);
    CHECK(ram_rd(REFRESH_ADDR + (TEST_ROW_OFFSET / PSC3M6_FLASH_SECTOR_SIZE) * 16u + 4u) == 4u);

    /* A bad data pointer fails before the refresh bookkeeping moves. */
    CHECK(rom_call(PSC3M6_ROM_FUNC_WRITE_ROW, sbus, 0u, CTX_ADDR) == CYBOOT_FLASH_PARAM_INVALID);
    CHECK(rom_call(PSC3M6_ROM_FUNC_WRITE_ROW, sbus, 0x00700000u, CTX_ADDR) == CYBOOT_FLASH_PARAM_INVALID);
    CHECK(ram_rd(REFRESH_ADDR + (TEST_ROW_OFFSET / PSC3M6_FLASH_SECTOR_SIZE) * 16u + 4u) == 4u);
    CHECK(ram_rd(cbus + 8u) == 0x0000F000u);

    /* Non-blocking mode on a blocking entry, and the entries the model does
     * not service, fail instead of branching into the ROM. */
    ram_wr(CTX_ADDR + 0x0u, 1u);
    CHECK(rom_call(PSC3M6_ROM_FUNC_ERASE_ROW, sbus, CTX_ADDR, 0u) == CYBOOT_FLASH_PARAM_INVALID);
    ram_wr(CTX_ADDR + 0x0u, 0u);
    CHECK(rom_call(0u, sbus, CTX_ADDR, 0u) == CYBOOT_FLASH_FAILED);
    CHECK(rom_call(PSC3M6_ROM_FUNC_WRITE_ROW + 1u, sbus, DATA_ADDR, CTX_ADDR) == CYBOOT_FLASH_FAILED);

    /* A CPU store into the flash faults. */
    CHECK(!mm_memmap_write(&map, MM_SECURE, cbus, 4u, 0u));
}

int main(void)
{
    setup();
    test_pclk_and_scb_gate();
    test_hsiom_ns_mask();
    test_romapi();
    if (failures != 0) {
        printf("psc3m6_flash_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("psc3m6_flash_test: OK\n");
    return 0;
}
