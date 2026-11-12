/* m33mu -- RW612 FlexSPI NOR through the boot ROM API.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Replays the sequence wolfBoot's hal/rw612.c uses to update the external
 * NOR: XIP cache enable (CACHE64), ROM API tree selection from SOCCTRL
 * CHIP_INFO, FlexSPI NOR driver init with the boot FCB copied to RAM, then
 * 0xFF-padded page programs and 4 KB sector erases, each followed by the
 * FlexSPI AHB buffer and CACHE64 flush.  Results are read back through both
 * aliases of the XIP window.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define REG32(a)            (*(volatile uint32_t *)(a))

#define SOCCTRL_CHIP_INFO   REG32(0x45001114u)
#define ROM_API_TREE_A0     0x13024100u
#define ROM_API_TREE_A1     0x13030000u
#define ROM_API_FLEXSPI_IDX 5u

#define CACHE64_CTRL0_CCR   REG32(0x40033800u)
#define CACHE64_POLSEL0_REG0_TOP REG32(0x40033014u)
#define CACHE64_POLSEL0_REG1_TOP REG32(0x40033018u)
#define CACHE64_POLSEL0_POLSEL   REG32(0x4003301Cu)
#define CCR_ENCACHE         (1u << 0)
#define CCR_ENWRBUF         (1u << 1)
#define CCR_INVW0           (1u << 24)
#define CCR_INVW1           (1u << 26)
#define CCR_GO              (1u << 31)

#define FLEXSPI_AHBCR       REG32(0x4013400Cu)
#define AHBCR_CLRAHBRXBUF   (1u << 1)

#define FLASH_BASE_NS       0x08000000u
#define FLASH_BASE_S        0x18000000u
#define FCB_ADDRESS         (FLASH_BASE_NS + 0x400u)
#define PAGE_SIZE           256u
#define SECTOR_SIZE         0x1000u
#define TEST_OFFSET         0x00200000u   /* beyond this 2 MB image region */

typedef int32_t status_t;

typedef struct {
    uint32_t words[0x200 / 4];
} flexspi_nor_config_t;

typedef struct {
    uint32_t version;
    status_t (*init)(uint32_t instance, flexspi_nor_config_t *config);
    status_t (*wait_busy)(uint32_t instance, flexspi_nor_config_t *config, uint32_t address, int keepState);
    status_t (*page_program)(uint32_t instance, flexspi_nor_config_t *config, uint32_t dstAddr,
                             const uint32_t *src, int keepState);
    status_t (*erase_all)(uint32_t instance, flexspi_nor_config_t *config);
    status_t (*erase)(uint32_t instance, flexspi_nor_config_t *config, uint32_t start, uint32_t length);
    status_t (*erase_sector)(uint32_t instance, flexspi_nor_config_t *config, uint32_t address);
    status_t (*erase_block)(uint32_t instance, flexspi_nor_config_t *config, uint32_t address);
    status_t (*read)(uint32_t instance, flexspi_nor_config_t *config, uint32_t *dst, uint32_t start,
                     uint32_t bytes);
    void (*config_clock)(uint32_t instance, uint32_t freqOption, uint32_t sampleClkMode);
    status_t (*set_clock_source)(uint32_t clockSrc);
    status_t (*get_config)(uint32_t instance, flexspi_nor_config_t *config, void *option);
    void (*hw_reset)(uint32_t instance, uint32_t reset_logic);
    status_t (*xfer)(uint32_t instance, void *xfer);
    status_t (*update_lut)(uint32_t instance, uint32_t seqIndex, const uint32_t *lutBase, uint32_t numberOfSeq);
    status_t (*partial_program)(uint32_t instance, flexspi_nor_config_t *config, uint32_t dstAddr,
                                const uint32_t *src, uint32_t length, int keepState);
} flexspi_nor_flash_driver_t;

static int failures;
static flexspi_nor_config_t flash_cfg;
static const flexspi_nor_flash_driver_t *rom_flexspi;
static uint32_t page[PAGE_SIZE / sizeof(uint32_t)];
static uint32_t readback[64];

static void check(const char *name, int ok)
{
    printf("%-36s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) {
        failures++;
    }
}

static uint32_t irq_save(void)
{
    uint32_t primask;
    __asm volatile("mrs %0, primask\n cpsid i" : "=r"(primask) :: "memory");
    return primask;
}

static void irq_restore(uint32_t primask)
{
    __asm volatile("msr primask, %0" :: "r"(primask) : "memory");
}

/* hal/rw612.c rw612_xip_cache_enable() */
static void xip_cache_enable(void)
{
    CACHE64_CTRL0_CCR = CCR_INVW1 | CCR_INVW0;
    CACHE64_CTRL0_CCR |= CCR_GO;
    while ((CACHE64_CTRL0_CCR & CCR_GO) != 0u) {
    }
    CACHE64_CTRL0_CCR = CCR_ENWRBUF | CCR_ENCACHE;
    CACHE64_POLSEL0_REG0_TOP = 0x07FFFC00u;
    CACHE64_POLSEL0_REG1_TOP = 0x0u;
    CACHE64_POLSEL0_POLSEL = 0x1u;
}

/* hal/rw612.c rw612_flash_cache_flush() */
static void flash_cache_flush(void)
{
    FLEXSPI_AHBCR |= AHBCR_CLRAHBRXBUF;
    FLEXSPI_AHBCR &= ~AHBCR_CLRAHBRXBUF;
    CACHE64_CTRL0_CCR |= CCR_INVW0 | CCR_INVW1 | CCR_GO;
    while ((CACHE64_CTRL0_CCR & CCR_GO) != 0u) {
    }
    CACHE64_CTRL0_CCR &= ~(CCR_INVW0 | CCR_INVW1);
    __asm volatile("dsb\n isb" ::: "memory");
}

/* hal/rw612.c hal_flash_write(): one 0xFF-padded page per chunk */
static int nor_write(uint32_t offset, const uint8_t *data, uint32_t len)
{
    uint32_t page_off;
    uint32_t chunk;
    uint32_t primask;
    uint32_t i;
    status_t st;

    while (len > 0u) {
        page_off = offset & (PAGE_SIZE - 1u);
        chunk = PAGE_SIZE - page_off;
        if (chunk > len) {
            chunk = len;
        }
        for (i = 0; i < PAGE_SIZE / sizeof(uint32_t); i++) {
            page[i] = 0xFFFFFFFFu;
        }
        memcpy((uint8_t *)page + page_off, data, chunk);
        primask = irq_save();
        st = rom_flexspi->page_program(0u, &flash_cfg, offset - page_off, page, 0);
        flash_cache_flush();
        irq_restore(primask);
        if (st != 0) {
            return -1;
        }
        offset += chunk;
        data += chunk;
        len -= chunk;
    }
    return 0;
}

/* hal/rw612.c hal_flash_erase() */
static int nor_erase(uint32_t offset, uint32_t len)
{
    uint32_t end = offset + len;
    uint32_t primask;
    status_t st;

    offset -= offset % SECTOR_SIZE;
    while (offset < end) {
        primask = irq_save();
        st = rom_flexspi->erase_sector(0u, &flash_cfg, offset);
        flash_cache_flush();
        irq_restore(primask);
        if (st != 0) {
            return -1;
        }
        offset += SECTOR_SIZE;
    }
    return 0;
}

static int all_bytes(uint32_t addr, uint32_t len, uint8_t v)
{
    const volatile uint8_t *p = (const volatile uint8_t *)addr;
    uint32_t i;
    for (i = 0; i < len; i++) {
        if (p[i] != v) {
            return 0;
        }
    }
    return 1;
}

int main(void)
{
    static const uint8_t pattern_a[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF
    };
    static const uint8_t pattern_b[8] = { 'w', 'o', 'l', 'f', 'B', 'o', 'o', 't' };
    static const uint8_t magic[4] = { 'B', 'O', 'O', 'T' };
    static const uint8_t state_byte = 0x70;
    static const uint8_t over = 0x0F;
    const volatile uint8_t *xip_ns = (const volatile uint8_t *)(FLASH_BASE_NS + TEST_OFFSET);
    const volatile uint8_t *xip_s = (const volatile uint8_t *)(FLASH_BASE_S + TEST_OFFSET);
    uint32_t rev;
    uint32_t tree;
    uint32_t other;
    uint32_t primask;
    uint8_t *big;
    uint32_t i;
    status_t st;
    int ok;

    printf("RW612 FlexSPI NOR / ROM API test\n");

    xip_cache_enable();
    check("CACHE64 CCR GO self-clears", (CACHE64_CTRL0_CCR & CCR_GO) == 0u);
    check("CACHE64 enabled", CACHE64_CTRL0_CCR == (CCR_ENWRBUF | CCR_ENCACHE));
    check("CACHE64 POLSEL readback", CACHE64_POLSEL0_REG0_TOP == 0x07FFFC00u &&
                                     CACHE64_POLSEL0_POLSEL == 0x1u);

    rev = SOCCTRL_CHIP_INFO & 0x0Fu;
    printf("silicon revision %lu, ROM API tree %s\n", (unsigned long)rev, (rev == 0u) ? "A0" : "A1");
    tree = (rev == 0u) ? ROM_API_TREE_A0 : ROM_API_TREE_A1;
    other = (rev == 0u) ? ROM_API_TREE_A1 : ROM_API_TREE_A0;
    rom_flexspi = (const flexspi_nor_flash_driver_t *)((const uint32_t *)tree)[ROM_API_FLEXSPI_IDX];
    check("ROM API tree has a FlexSPI driver", rom_flexspi != 0 &&
          (((uint32_t)rom_flexspi) & 0xFFFC0000u) == 0x13000000u);
    check("other revision's tree slot empty", ((const uint32_t *)other)[ROM_API_FLEXSPI_IDX] == 0u);
    check("FlexSPI driver version", rom_flexspi->version != 0u);

    memcpy(&flash_cfg, (const void *)FCB_ADDRESS, sizeof(flash_cfg));
    primask = irq_save();
    st = rom_flexspi->init(0u, &flash_cfg);
    flash_cache_flush();
    irq_restore(primask);
    check("flexspi init with the boot FCB", st == 0);
    check("FlexSPI AHBCR CLRAHBRXBUF clears", (FLEXSPI_AHBCR & AHBCR_CLRAHBRXBUF) == 0u);

    flash_cfg.words[0] ^= 1u;
    check("flexspi init rejects a bad tag", rom_flexspi->init(0u, &flash_cfg) != 0);
    flash_cfg.words[0] ^= 1u;
    check("flexspi rejects instance 1", rom_flexspi->erase_sector(1u, &flash_cfg, TEST_OFFSET) != 0);

    check("erase test sector", nor_erase(TEST_OFFSET, SECTOR_SIZE) == 0);
    check("erased sector reads 0xFF (NS)", all_bytes(FLASH_BASE_NS + TEST_OFFSET, SECTOR_SIZE, 0xFFu));
    check("erased sector reads 0xFF (S)", all_bytes(FLASH_BASE_S + TEST_OFFSET, SECTOR_SIZE, 0xFFu));

    check("program 16 B at page offset 0x10", nor_write(TEST_OFFSET + 0x10u, pattern_a, sizeof(pattern_a)) == 0);
    check("program 8 B at page offset 0x80", nor_write(TEST_OFFSET + 0x80u, pattern_b, sizeof(pattern_b)) == 0);
    check("first chunk kept by 0xFF padding", memcmp((const void *)(FLASH_BASE_NS + TEST_OFFSET + 0x10u),
                                                    pattern_a, sizeof(pattern_a)) == 0);
    check("second chunk programmed", memcmp((const void *)(FLASH_BASE_S + TEST_OFFSET + 0x80u),
                                            pattern_b, sizeof(pattern_b)) == 0);
    check("untouched bytes stay erased", all_bytes(FLASH_BASE_NS + TEST_OFFSET, 0x10u, 0xFFu) &&
                                         all_bytes(FLASH_BASE_NS + TEST_OFFSET + 0x20u, 0x60u, 0xFFu));

    /* Partition trailer pattern: magic, then a state byte that only clears bits. */
    check("program trailer magic across a page", nor_write(TEST_OFFSET + 0x1FEu, magic, sizeof(magic)) == 0);
    check("trailer magic reads back", memcmp((const void *)(FLASH_BASE_NS + TEST_OFFSET + 0x1FEu),
                                             magic, sizeof(magic)) == 0);
    check("program state byte", nor_write(TEST_OFFSET + 0x300u, &state_byte, 1u) == 0);
    check("reprogram clears bits only (NOR AND)", nor_write(TEST_OFFSET + 0x10u, &over, 1u) == 0 &&
                                                  xip_ns[0x10] == 0x00u && xip_s[0x300] == 0x70u);

    for (i = 0; i < sizeof(readback) / sizeof(readback[0]); i++) {
        readback[i] = 0u;
    }
    st = rom_flexspi->read(0u, &flash_cfg, readback, TEST_OFFSET + 0x80u, sizeof(pattern_b));
    check("flexspi read API", st == 0 && memcmp(readback, pattern_b, sizeof(pattern_b)) == 0);

    /* A multi-sector erase starting mid-sector, as hal_flash_erase() is called. */
    big = (uint8_t *)readback;
    memset(big, 0x5A, sizeof(readback));
    ok = nor_write(TEST_OFFSET + SECTOR_SIZE + 0x40u, big, sizeof(readback)) == 0;
    ok = ok && nor_write(TEST_OFFSET + 2u * SECTOR_SIZE, pattern_b, sizeof(pattern_b)) == 0;
    ok = ok && xip_ns[SECTOR_SIZE + 0x40u] == 0x5Au && xip_s[2u * SECTOR_SIZE] == 'w';
    check("program second and third sector", ok);
    check("erase two sectors from mid-sector", nor_erase(TEST_OFFSET + 0x123u, SECTOR_SIZE + 0x10u) == 0);
    check("both sectors erased", all_bytes(FLASH_BASE_NS + TEST_OFFSET, 2u * SECTOR_SIZE, 0xFFu));
    check("third sector untouched", memcmp((const void *)(FLASH_BASE_NS + TEST_OFFSET + 2u * SECTOR_SIZE),
                                           pattern_b, sizeof(pattern_b)) == 0);

    if (failures == 0) {
        printf("=== ALL PASS ===\n");
    } else {
        printf("=== %d FAILURE(S) ===\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
