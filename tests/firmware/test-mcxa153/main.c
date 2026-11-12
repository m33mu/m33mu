/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Replays the register and ROM API sequence the wolfBoot MCX A153 HAL uses:
 * BOARD_BootClockFRO96M (SPC, FMU, SCG, MRCC), FLASH_Init, FLASH_EraseSector,
 * FLASH_ProgramPhrase and the LPUART0 console.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;
extern uint32_t _estack;
extern void __libc_init_array(void);

#define REG32(a) (*(volatile uint32_t *)(a))

/* MRCC0 / SYSCON */
#define MRCC0_BASE        0x40091000u
#define MRCC_GLB_RST0     REG32(MRCC0_BASE + 0x000u)
#define MRCC_GLB_RST0_SET REG32(MRCC0_BASE + 0x004u)
#define MRCC_GLB_RST0_CLR REG32(MRCC0_BASE + 0x008u)
#define MRCC_GLB_CC0      REG32(MRCC0_BASE + 0x040u)
#define MRCC_GLB_CC0_SET  REG32(MRCC0_BASE + 0x044u)
#define MRCC_LPUART0_CLKSEL REG32(MRCC0_BASE + 0x0E0u)
#define MRCC_LPUART0_CLKDIV REG32(MRCC0_BASE + 0x0E4u)
#define MRCC_FRO_HF_DIV_CLKDIV REG32(MRCC0_BASE + 0x154u)
#define SYSCON_AHBCLKDIV  REG32(MRCC0_BASE + 0x380u)
#define SYSCON_CLKUNLOCK  REG32(MRCC0_BASE + 0x3FCu)
#define MRCC_LPUART0_BIT  (1u << 19)

/* SCG0 */
#define SCG0_BASE         0x4008F000u
#define SCG_CSR           REG32(SCG0_BASE + 0x010u)
#define SCG_RCCR          REG32(SCG0_BASE + 0x014u)
#define SCG_SIRCCSR       REG32(SCG0_BASE + 0x200u)
#define SCG_FIRCCSR       REG32(SCG0_BASE + 0x300u)
#define SCG_FIRCCFG       REG32(SCG0_BASE + 0x308u)
#define SCG_SCS_MASK      0x07000000u
#define SCG_SCS_FIRC      0x03000000u
#define SCG_FIRCCSR_FIRCEN (1u << 0)
#define SCG_FIRCCSR_SCLK_PERIPH_EN (1u << 4)
#define SCG_FIRCCSR_FCLK_PERIPH_EN (1u << 5)
#define SCG_FIRCCSR_LK    (1u << 23)
#define SCG_FIRCCSR_FIRCVLD (1u << 24)
#define SCG_SIRCCSR_CLK_PERIPH_EN (1u << 5)
#define SCG_SIRCCSR_LK    (1u << 23)
#define SCG_SIRCCSR_SIRCVLD (1u << 24)
#define SCG_FIRCCFG_96M   (5u << 1)

/* SPC0 */
#define SPC0_BASE         0x40090000u
#define SPC_SC            REG32(SPC0_BASE + 0x010u)
#define SPC_SRAMCTL       REG32(SPC0_BASE + 0x040u)
#define SPC_ACTIVE_CFG    REG32(SPC0_BASE + 0x100u)
#define SPC_SC_BUSY       (1u << 0)
#define SPC_SRAMCTL_VSM_1P1V 2u
#define SPC_SRAMCTL_REQ   (1u << 30)
#define SPC_SRAMCTL_ACK   (1u << 31)
#define SPC_ACTIVE_CFG_CORELDO_VDD_LVL_MASK 0xCu

/* FMU0 */
#define FMU0_BASE         0x40095000u
#define FMU_FCTRL         REG32(FMU0_BASE + 0x008u)
#define FMU_FCTRL_RWSC_MASK 0xFu

/* LPUART0 */
#define LPUART0_BASE      0x4009F000u
#define LPUART_BAUD       REG32(LPUART0_BASE + 0x10u)
#define LPUART_STAT       REG32(LPUART0_BASE + 0x14u)
#define LPUART_CTRL       REG32(LPUART0_BASE + 0x18u)
#define LPUART_DATA       REG32(LPUART0_BASE + 0x1Cu)
#define LPUART_STAT_TDRE  (1u << 23)
#define LPUART_CTRL_TE    (1u << 19)
#define LPUART_CTRL_RE    (1u << 18)

/* ROM API (fsl_romapi.h) */
#define ROM_API_BASE      0x03003FE0u
#define FLASH_ERASE_KEY   0x6B65666Cu
#define FLASH_SECTOR_SIZE 0x2000u
#define FLASH_PHRASE_SIZE 16u
#define FLASH_TOTAL_SIZE  0x20000u
#define FLASH_PAGE_SIZE   128u
#define TEST_SECTOR       (FLASH_TOTAL_SIZE - FLASH_SECTOR_SIZE)

#define kStatus_FLASH_AlignmentError 101
#define kStatus_FLASH_AddressError   102
#define kStatus_FLASH_CommandFailure 105
#define kStatus_FLASH_EraseKeyError  107

typedef struct {
    uint32_t PFlashBlockBase;
    uint32_t PFlashTotalSize;
    uint32_t PFlashBlockCount;
    uint32_t PFlashPageSize;
    uint32_t PFlashSectorSize;
    uint32_t ffrBlockBase;
    uint32_t ffrTotalSize;
    uint32_t ffrPageSize;
    uint32_t sectorSize;
    uint32_t cfpaPageVersion;
    uint32_t cfpaPageOffset;
} flash_config_t;

typedef struct {
    int32_t (*flash_init)(flash_config_t *config);
    int32_t (*flash_erase_sector)(flash_config_t *config, uint32_t start, uint32_t len, uint32_t key);
    int32_t (*flash_program_phrase)(flash_config_t *config, uint32_t start, uint8_t *src, uint32_t len);
    int32_t (*flash_program_page)(flash_config_t *config, uint32_t start, uint8_t *src, uint32_t len);
    int32_t (*flash_verify_program)(flash_config_t *config, uint32_t start, uint32_t len,
                                    const uint8_t *expected, uint32_t *failed_addr, uint32_t *failed_data);
    int32_t (*flash_verify_erase_phrase)(flash_config_t *config, uint32_t start, uint32_t len);
    int32_t (*flash_verify_erase_page)(flash_config_t *config, uint32_t start, uint32_t len);
    int32_t (*flash_verify_erase_sector)(flash_config_t *config, uint32_t start, uint32_t len);
    int32_t (*flash_get_property)(flash_config_t *config, uint32_t which, uint32_t *value);
    int32_t (*ifr_verify_erase_phrase)(flash_config_t *config, uint32_t start, uint32_t len);
    int32_t (*ifr_verify_erase_page)(flash_config_t *config, uint32_t start, uint32_t len);
    int32_t (*ifr_verify_erase_sector)(flash_config_t *config, uint32_t start, uint32_t len);
    int32_t (*flash_read)(flash_config_t *config, uint32_t start, uint8_t *dest, uint32_t len);
    uint32_t version;
} flash_driver_interface_t;

typedef struct {
    void (*run_bootloader)(void *arg);
    const flash_driver_interface_t *flash_driver;
    void (*jump)(void *arg);
} bootloader_tree_t;

#define FLASH_API (((const bootloader_tree_t *)ROM_API_BASE)->flash_driver)

static flash_config_t pflash;
static int failures;

static void check(const char *name, int ok)
{
    printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) {
        failures++;
    }
}

static void lpuart0_init(void)
{
    SYSCON_CLKUNLOCK &= ~1u;
    MRCC_GLB_CC0_SET = MRCC_LPUART0_BIT;
    MRCC_LPUART0_CLKSEL = 0u;
    MRCC_LPUART0_CLKDIV = 0u;
    MRCC_GLB_RST0_CLR = MRCC_LPUART0_BIT;
    MRCC_GLB_RST0_SET = MRCC_LPUART0_BIT;
    SYSCON_CLKUNLOCK |= 1u;
    LPUART_BAUD = 0x0F000007u;
    LPUART_CTRL = LPUART_CTRL_TE | LPUART_CTRL_RE;
}

void lpuart0_putc(char c)
{
    while ((LPUART_STAT & LPUART_STAT_TDRE) == 0u) {
    }
    LPUART_DATA = (uint32_t)c;
}

/* BOARD_BootClockFRO96M from the frdmmcxa153 project template */
static void boot_clock_fro96m(void)
{
    uint32_t cfg;

    while ((SPC_SC & SPC_SC_BUSY) != 0u) {
    }
    cfg = SPC_ACTIVE_CFG;
    if ((cfg & SPC_ACTIVE_CFG_CORELDO_VDD_LVL_MASK) != (2u << 2)) {
        SPC_ACTIVE_CFG = (cfg & ~SPC_ACTIVE_CFG_CORELDO_VDD_LVL_MASK) | (2u << 2);
        while ((SPC_SC & SPC_SC_BUSY) != 0u) {
        }
    }
    FMU_FCTRL = (FMU_FCTRL & ~FMU_FCTRL_RWSC_MASK) | 2u;
    SPC_SRAMCTL = SPC_SRAMCTL_VSM_1P1V;
    SPC_SRAMCTL |= SPC_SRAMCTL_REQ;
    while ((SPC_SRAMCTL & SPC_SRAMCTL_ACK) == 0u) {
    }
    SPC_SRAMCTL &= ~SPC_SRAMCTL_REQ;

    SCG_FIRCCFG = SCG_FIRCCFG_96M;
    SCG_FIRCCSR &= ~SCG_FIRCCSR_LK;
    SCG_FIRCCSR |= SCG_FIRCCSR_SCLK_PERIPH_EN;
    SCG_FIRCCSR |= SCG_FIRCCSR_FCLK_PERIPH_EN;
    SCG_FIRCCSR |= SCG_FIRCCSR_FIRCEN;
    SCG_FIRCCSR |= SCG_FIRCCSR_LK;
    while ((SCG_FIRCCSR & SCG_FIRCCSR_FIRCVLD) == 0u) {
    }

    SCG_SIRCCSR &= ~SCG_SIRCCSR_LK;
    SCG_SIRCCSR |= SCG_SIRCCSR_CLK_PERIPH_EN;
    SCG_SIRCCSR |= SCG_SIRCCSR_LK;
    while ((SCG_SIRCCSR & SCG_SIRCCSR_SIRCVLD) == 0u) {
    }

    SCG_RCCR = (SCG_RCCR & ~SCG_SCS_MASK) | SCG_SCS_FIRC;
    while ((SCG_CSR & SCG_SCS_MASK) != SCG_SCS_FIRC) {
    }

    SYSCON_CLKUNLOCK &= ~1u;
    SYSCON_AHBCLKDIV = 0u;
    MRCC_FRO_HF_DIV_CLKDIV = 0u;
    SYSCON_CLKUNLOCK |= 1u;
}

static int range_is(uint32_t addr, uint32_t len, uint8_t val)
{
    const volatile uint8_t *p = (const volatile uint8_t *)addr;
    uint32_t i;
    for (i = 0; i < len; ++i) {
        if (p[i] != val) {
            return 0;
        }
    }
    return 1;
}

static void test_romapi(void)
{
    static uint8_t phrase[FLASH_PHRASE_SIZE];
    static uint8_t readback[FLASH_PHRASE_SIZE];
    uint32_t value = 0;
    uint32_t failed_addr = 0;
    uint32_t failed_data = 0;
    int32_t rc;
    uint32_t i;

    memset(&pflash, 0, sizeof(pflash));
    rc = FLASH_API->flash_init(&pflash);
    check("flash_init", rc == 0);
    check("flash_init config", pflash.PFlashBlockBase == 0u &&
          pflash.PFlashTotalSize == FLASH_TOTAL_SIZE &&
          pflash.PFlashBlockCount == 1u &&
          pflash.PFlashPageSize == FLASH_PAGE_SIZE &&
          pflash.PFlashSectorSize == FLASH_SECTOR_SIZE);
    check("flash_get_property sector", FLASH_API->flash_get_property(&pflash, 0u, &value) == 0 &&
          value == FLASH_SECTOR_SIZE);
    check("flash_get_property phrase", FLASH_API->flash_get_property(&pflash, 0x30u, &value) == 0 &&
          value == FLASH_PAGE_SIZE);
    check("flash_get_property unknown", FLASH_API->flash_get_property(&pflash, 0x77u, &value) == 106);
    check("version", FLASH_API->version != 0u && FLASH_API->version != 0xFFFFFFFFu);

    rc = FLASH_API->flash_erase_sector(&pflash, TEST_SECTOR, FLASH_SECTOR_SIZE, 0u);
    check("erase bad key", rc == kStatus_FLASH_EraseKeyError);
    rc = FLASH_API->flash_erase_sector(&pflash, TEST_SECTOR + 2u, FLASH_SECTOR_SIZE, FLASH_ERASE_KEY);
    check("erase unaligned", rc == kStatus_FLASH_AlignmentError);
    rc = FLASH_API->flash_erase_sector(&pflash, FLASH_TOTAL_SIZE, 4u, FLASH_ERASE_KEY);
    check("erase out of range", rc == kStatus_FLASH_AddressError);

    /* The wolfBoot HAL erases whole sectors from a 4-byte aligned address */
    rc = FLASH_API->flash_erase_sector(&pflash, TEST_SECTOR + 4u, FLASH_SECTOR_SIZE - 8u, FLASH_ERASE_KEY);
    check("erase sector", rc == 0 && range_is(TEST_SECTOR, FLASH_SECTOR_SIZE, 0xFFu));
    check("verify_erase_sector", FLASH_API->flash_verify_erase_sector(&pflash, TEST_SECTOR, FLASH_SECTOR_SIZE) == 0);

    for (i = 0; i < FLASH_PHRASE_SIZE; ++i) {
        phrase[i] = (uint8_t)(0xF0u | i);
    }
    rc = FLASH_API->flash_program_phrase(&pflash, TEST_SECTOR + 2u, phrase, FLASH_PHRASE_SIZE);
    check("program unaligned", rc == kStatus_FLASH_AlignmentError);
    rc = FLASH_API->flash_program_phrase(&pflash, TEST_SECTOR + 16u, phrase, 6u);
    check("program odd length", rc == kStatus_FLASH_AlignmentError);
    rc = FLASH_API->flash_verify_erase_phrase(&pflash, TEST_SECTOR + 1u, 4u);
    check("verify unaligned", rc == kStatus_FLASH_AlignmentError);
    rc = FLASH_API->flash_program_phrase(&pflash, TEST_SECTOR + 16u, phrase, FLASH_PHRASE_SIZE);
    check("program_phrase", rc == 0 && memcmp((const void *)(TEST_SECTOR + 16u), phrase, FLASH_PHRASE_SIZE) == 0);
    check("verify_program", FLASH_API->flash_verify_program(&pflash, TEST_SECTOR + 16u, FLASH_PHRASE_SIZE,
                                                            phrase, &failed_addr, &failed_data) == 0);
    check("verify_erase_phrase programmed",
          FLASH_API->flash_verify_erase_phrase(&pflash, TEST_SECTOR + 16u, FLASH_PHRASE_SIZE) == kStatus_FLASH_CommandFailure);
    check("flash_read", FLASH_API->flash_read(&pflash, TEST_SECTOR + 16u, readback, FLASH_PHRASE_SIZE) == 0 &&
          memcmp(readback, phrase, FLASH_PHRASE_SIZE) == 0);

    /* Reprogramming a phrase only clears bits: the trailer flag update path */
    phrase[0] = 0x70u;
    rc = FLASH_API->flash_program_phrase(&pflash, TEST_SECTOR + 16u, phrase, FLASH_PHRASE_SIZE);
    check("program merge", rc == 0 && *(volatile uint8_t *)(TEST_SECTOR + 16u) == 0x70u &&
          *(volatile uint8_t *)(TEST_SECTOR + 17u) == 0xF1u);
    phrase[0] = 0xFFu;
    rc = FLASH_API->flash_program_phrase(&pflash, TEST_SECTOR + 16u, phrase, FLASH_PHRASE_SIZE);
    check("program cannot set bits", rc == 0 && *(volatile uint8_t *)(TEST_SECTOR + 16u) == 0x70u);
    phrase[0] = 0x70u;
    check("verify_program mismatch",
          FLASH_API->flash_verify_program(&pflash, TEST_SECTOR + 16u, FLASH_PHRASE_SIZE, readback,
                                          &failed_addr, &failed_data) == kStatus_FLASH_CommandFailure &&
          failed_addr == TEST_SECTOR + 16u);

    rc = FLASH_API->flash_erase_sector(&pflash, TEST_SECTOR, FLASH_SECTOR_SIZE, FLASH_ERASE_KEY);
    check("erase again", rc == 0 && range_is(TEST_SECTOR + 16u, FLASH_PHRASE_SIZE, 0xFFu));
}

int main(void)
{
    boot_clock_fro96m();
    lpuart0_init();

    printf("MCXA153 test start\n");
    check("FIRC 96M selected", (SCG_CSR & SCG_SCS_MASK) == SCG_SCS_FIRC);
    check("LPUART0 clocked", (MRCC_GLB_CC0 & MRCC_LPUART0_BIT) != 0u &&
          (MRCC_GLB_RST0 & MRCC_LPUART0_BIT) != 0u);
    test_romapi();

    if (failures == 0) {
        printf("ALL PASS\n");
        __asm volatile("bkpt #0x7f");
    } else {
        printf("FAILURES: %d\n", failures);
        __asm volatile("bkpt #0x7d");
    }
    while (1) {
        __asm volatile("wfi");
    }
}

void Reset_Handler(void)
{
    uint32_t *src;
    uint32_t *dst;

    src = &_sidata;
    for (dst = &_sdata; dst < &_edata; ) {
        *dst++ = *src++;
    }
    for (dst = &_sbss; dst < &_ebss; ) {
        *dst++ = 0u;
    }

    __libc_init_array();
    main();
}

void HardFault_Handler(void)
{
    __asm volatile("bkpt #0x7e");
    while (1) {
    }
}
