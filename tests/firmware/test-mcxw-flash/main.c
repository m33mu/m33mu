/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/* MCXW716C FMU0: replays the wolfBoot hal/mcxw.c command sequence (FCCOB0,
 * PEWEN, phrase store through the secure alias, PERDY, CCIF) and the SPC,
 * SMSCM and MCM accesses of its clock and cache code. */
#include <stdint.h>
#include <stdio.h>

extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;
extern void __libc_init_array(void);

#define REG32(a)         (*(volatile uint32_t *)(a))

#define MRCC_LPUART1     REG32(0x4001C0E4u)

#define LPUART1_BASE     0x40039000u
#define LPUART_STAT      REG32(LPUART1_BASE + 0x14u)
#define LPUART_CTRL      REG32(LPUART1_BASE + 0x18u)
#define LPUART_DATA      REG32(LPUART1_BASE + 0x1Cu)
#define LPUART_STAT_TDRE (1u << 23)
#define LPUART_CTRL_TE   (1u << 19)

#define FMU0_BASE        0x50020000u
#define FMU_FSTAT        REG32(FMU0_BASE + 0x00u)
#define FMU_FCTRL        REG32(FMU0_BASE + 0x08u)
#define FMU_FCCOB(n)     REG32(FMU0_BASE + 0x10u + 4u * (n))
#define FSTAT_FAIL       0x00000001u
#define FSTAT_ACCERR     0x00000020u
#define FSTAT_CCIF       0x00000080u
#define FSTAT_PEWEN      0x03000000u
#define FSTAT_PEWEN_PG   0x02000000u
#define FSTAT_PERDY      0x80000000u
#define FSTAT_ERR        0x00000075u
#define FSTAT_CLEAR      0x00010074u
#define CMD_RD1PHR       0x04u
#define CMD_PGMPG        0x23u
#define CMD_PGMPHR       0x24u
#define CMD_ERSSCR       0x42u

#define SPC0_BASE        0x50016000u
#define SPC_SC           REG32(SPC0_BASE + 0x10u)
#define SPC_ACTIVE_CFG   REG32(SPC0_BASE + 0x100u)
#define SPC_SRAMCTL      REG32(SPC0_BASE + 0x40u)
#define SRAMCTL_REQ      (1u << 30)
#define SRAMCTL_ACK      (1u << 31)

#define SMSCM_OCMDR0     REG32(0x50015400u)
#define MCM_CPCR2        REG32(0xE0080034u)

/* Scratch sector well past this image; S alias for stores, NS for reads. */
#define SCRATCH          0x000F0000u
#define SECURE_ALIAS     0x10000000u
#define SECTOR_SIZE      0x2000u
#define PHRASE_WORDS     4u
#define PAGE_WORDS       32u

#define RAMFUNC __attribute__((section(".ramfunc"), noinline, long_call))

static int failures;

void lpuart1_putc(char c)
{
    while ((LPUART_STAT & LPUART_STAT_TDRE) == 0u) {
    }
    LPUART_DATA = (uint32_t)c;
}

static void check(const char *what, int ok)
{
    printf("%-50s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) {
        failures++;
    }
}

static uint32_t RAMFUNC fmu_wait(uint32_t mask)
{
    uint32_t fstat;
    do {
        fstat = FMU_FSTAT;
    } while (((fstat & mask) == 0u) && ((fstat & FSTAT_ERR) == 0u));
    return fstat;
}

/* The wolfBoot HAL sequence; words == 0 stores zeros (erase). Returns FSTAT. */
static uint32_t RAMFUNC fmu_cmd(uint32_t cmd, uint32_t addr,
                                const uint32_t *words, uint32_t nwords)
{
    volatile uint32_t *dst = (volatile uint32_t *)(addr | SECURE_ALIAS);
    uint32_t fstat;
    uint32_t i;

    __asm volatile("cpsid i" ::: "memory");
    while ((FMU_FSTAT & FSTAT_CCIF) == 0u) {
    }
    FMU_FSTAT = FSTAT_CLEAR;
    FMU_FCCOB(0) = cmd;
    FMU_FSTAT = FSTAT_CCIF;
    fstat = fmu_wait(FSTAT_PEWEN);
    if ((fstat & FSTAT_ERR) == 0u) {
        for (i = 0; i < nwords; i++) {
            dst[i] = (words != 0) ? words[i] : 0u;
        }
        fstat = fmu_wait(FSTAT_PERDY);
    }
    if ((fstat & FSTAT_ERR) == 0u) {
        FMU_FSTAT = FSTAT_PERDY;
    }
    do {
        fstat = FMU_FSTAT;
    } while ((fstat & FSTAT_CCIF) == 0u);
    __asm volatile("cpsie i" ::: "memory");
    return fstat;
}

/* Commands without a write phase (verify, bad opcodes). */
static uint32_t RAMFUNC fmu_simple(uint32_t cmd, uint32_t fccob2, int clear)
{
    uint32_t fstat;
    while ((FMU_FSTAT & FSTAT_CCIF) == 0u) {
    }
    if (clear) {
        FMU_FSTAT = FSTAT_CLEAR;
    }
    FMU_FCCOB(0) = cmd;
    FMU_FCCOB(2) = fccob2;
    FMU_FSTAT = FSTAT_CCIF;
    do {
        fstat = FMU_FSTAT;
    } while ((fstat & FSTAT_CCIF) == 0u);
    return fstat;
}

/* FSTAT at each step of a phrase program: launch, 3 of 4 words, 4 words. */
static void RAMFUNC fmu_phrase_steps(uint32_t addr, const uint32_t *w,
                                     uint32_t *steps)
{
    volatile uint32_t *dst = (volatile uint32_t *)(addr | SECURE_ALIAS);
    uint32_t i;

    FMU_FSTAT = FSTAT_CLEAR;
    FMU_FCCOB(0) = CMD_PGMPHR;
    FMU_FSTAT = FSTAT_CCIF;
    steps[0] = FMU_FSTAT;
    for (i = 0; i < 3u; i++) {
        dst[i] = w[i];
    }
    steps[1] = FMU_FSTAT;
    dst[3] = w[3];
    steps[2] = FMU_FSTAT;
    FMU_FSTAT = FSTAT_PERDY;
    do {
        steps[3] = FMU_FSTAT;
    } while ((steps[3] & FSTAT_CCIF) == 0u);
}

/* A store outside the latched phrase aborts the command with ACCERR. */
static uint32_t RAMFUNC fmu_stray_store(uint32_t addr)
{
    volatile uint32_t *dst = (volatile uint32_t *)(addr | SECURE_ALIAS);
    uint32_t fstat;

    FMU_FSTAT = FSTAT_CLEAR;
    FMU_FCCOB(0) = CMD_PGMPHR;
    FMU_FSTAT = FSTAT_CCIF;
    dst[0] = 0x11111111u;
    dst[PHRASE_WORDS] = 0x22222222u;
    do {
        fstat = FMU_FSTAT;
    } while ((fstat & FSTAT_CCIF) == 0u);
    return fstat;
}

static int range_is(uint32_t addr, uint32_t len, uint32_t v)
{
    uint32_t off;
    for (off = 0; off < len; off += 4u) {
        if (REG32(addr + off) != v) {
            return 0;
        }
    }
    return 1;
}

static void test_reset_and_power(void)
{
    uint32_t v;

    check("fmu: FSTAT resets to CCIF", FMU_FSTAT == FSTAT_CCIF);
    FMU_FCTRL = (FMU_FCTRL & ~0xFu) | 0x2u;
    check("fmu: FCTRL.RWSC holds the wait states", (FMU_FCTRL & 0xFu) == 0x2u);

    SPC_ACTIVE_CFG = (SPC_ACTIVE_CFG & ~0xCu) | (1u << 2);
    check("spc: SC.BUSY stays clear", (SPC_SC & 1u) == 0u);
    SPC_SRAMCTL = (SPC_SRAMCTL & ~0x3u) | 0x2u;
    SPC_SRAMCTL |= SRAMCTL_REQ;
    v = SPC_SRAMCTL;
    check("spc: SRAMCTL.ACK follows REQ", (v & SRAMCTL_ACK) != 0u && (v & 0x3u) == 0x2u);
    SPC_SRAMCTL &= ~SRAMCTL_REQ;
    check("spc: SRAMCTL.ACK drops with REQ", (SPC_SRAMCTL & SRAMCTL_ACK) == 0u);

    v = SMSCM_OCMDR0;
    SMSCM_OCMDR0 = (v & ~0xFF0u) | (0x3u << 4) | (0x1u << 8);
    check("smscm: OCMDR0 holds the cache controls", (SMSCM_OCMDR0 & 0xFF0u) == 0x130u);
    SMSCM_OCMDR0 = v;
    MCM_CPCR2 |= 1u;
    check("mcm: CPCR2.CCBC self-clears", (MCM_CPCR2 & 1u) == 0u);
}

static void test_erase_program(void)
{
    uint32_t phrase[PHRASE_WORDS] = { 0x01234567u, 0x89ABCDEFu, 0xDEADBEEFu, 0x0BADF00Du };
    uint32_t again[PHRASE_WORDS] = { 0xFFFF0000u, 0xFFFFFFFFu, 0x0000FFFFu, 0xFFFFFFFFu };
    uint32_t page[PAGE_WORDS];
    uint32_t fstat;
    uint32_t i;
    int ok;

    fstat = fmu_cmd(CMD_ERSSCR, SCRATCH, 0, PHRASE_WORDS);
    check("erase: sector command completes clean", (fstat & FSTAT_ERR) == 0u);
    check("erase: sector reads 0xFFFFFFFF",
          range_is(SCRATCH, SECTOR_SIZE, 0xFFFFFFFFu));

    fstat = fmu_cmd(CMD_PGMPHR, SCRATCH + 0x10u, phrase, PHRASE_WORDS);
    check("program: phrase command completes clean", (fstat & FSTAT_ERR) == 0u);
    ok = 1;
    for (i = 0; i < PHRASE_WORDS; i++) {
        ok &= REG32(SCRATCH + 0x10u + 4u * i) == phrase[i];
        ok &= REG32(SCRATCH + 0x10u + 4u * i + SECURE_ALIAS) == phrase[i];
    }
    check("program: phrase visible through both aliases", ok);
    check("program: neighbours stay erased",
          REG32(SCRATCH + 0x0Cu) == 0xFFFFFFFFu && REG32(SCRATCH + 0x20u) == 0xFFFFFFFFu);

    fstat = fmu_simple(CMD_RD1PHR, SCRATCH + 0x10u, 1);
    check("verify: programmed phrase sets FAIL", (fstat & FSTAT_FAIL) != 0u);
    fstat = fmu_simple(CMD_RD1PHR, SCRATCH + 0x20u, 1);
    check("verify: erased phrase leaves FAIL clear", (fstat & FSTAT_ERR) == 0u);

    (void)fmu_cmd(CMD_PGMPHR, SCRATCH + 0x10u, again, PHRASE_WORDS);
    ok = 1;
    for (i = 0; i < PHRASE_WORDS; i++) {
        ok &= REG32(SCRATCH + 0x10u + 4u * i) == (phrase[i] & again[i]);
    }
    check("program: reprogram only clears bits", ok);

    for (i = 0; i < PAGE_WORDS; i++) {
        page[i] = 0xA5000000u | i;
    }
    fstat = fmu_cmd(CMD_PGMPG, SCRATCH + 0x100u, page, PAGE_WORDS);
    ok = (fstat & FSTAT_ERR) == 0u;
    for (i = 0; i < PAGE_WORDS; i++) {
        ok &= REG32(SCRATCH + 0x100u + 4u * i) == page[i];
    }
    check("program: 128-byte page", ok);

    fstat = fmu_cmd(CMD_ERSSCR, SCRATCH + 0x1F00u, 0, PHRASE_WORDS);
    check("erase: any phrase selects its sector",
          (fstat & FSTAT_ERR) == 0u && range_is(SCRATCH, SECTOR_SIZE, 0xFFFFFFFFu));
}

static void test_handshake(void)
{
    uint32_t w[PHRASE_WORDS] = { 0x0u, 0x1u, 0x2u, 0x3u };
    uint32_t steps[4];

    fmu_phrase_steps(SCRATCH + 0x40u, w, steps);
    check("handshake: launch drops CCIF, raises PEWEN",
          (steps[0] & FSTAT_CCIF) == 0u && (steps[0] & FSTAT_PEWEN) == 0x01000000u);
    check("handshake: partial phrase keeps PEWEN, no PERDY",
          (steps[1] & FSTAT_PEWEN) != 0u && (steps[1] & FSTAT_PERDY) == 0u);
    check("handshake: full phrase raises PERDY, drops PEWEN",
          (steps[2] & FSTAT_PERDY) != 0u && (steps[2] & FSTAT_PEWEN) == 0u);
    check("handshake: PERDY ack completes with CCIF",
          (steps[3] & (FSTAT_CCIF | FSTAT_PERDY | FSTAT_ERR)) == FSTAT_CCIF);
    check("handshake: data landed", REG32(SCRATCH + 0x4Cu) == 0x3u);
}

static void test_errors(void)
{
    uint32_t fstat;

    fstat = fmu_simple(0x7Fu, 0u, 1);
    check("error: unknown command sets ACCERR", (fstat & FSTAT_ACCERR) != 0u);
    fstat = fmu_simple(CMD_ERSSCR, SCRATCH, 0);
    check("error: launch ignored until ACCERR cleared",
          (fstat & FSTAT_PEWEN) == 0u && (fstat & FSTAT_ACCERR) != 0u);
    FMU_FSTAT = FSTAT_CLEAR;
    check("error: ACCERR is write-1-to-clear", FMU_FSTAT == FSTAT_CCIF);

    fstat = fmu_stray_store(SCRATCH + 0x80u);
    check("error: store outside the phrase aborts",
          (fstat & FSTAT_ACCERR) != 0u && (fstat & FSTAT_PEWEN) == 0u);
    check("error: aborted phrase left erased",
          range_is(SCRATCH + 0x80u, 0x20u, 0xFFFFFFFFu));
    FMU_FSTAT = FSTAT_CLEAR;
}

int main(void)
{
    MRCC_LPUART1 = (1u << 30) | 1u;
    LPUART_CTRL = LPUART_CTRL_TE;

    printf("MCXW FMU0 test start\n");
    test_reset_and_power();
    test_erase_program();
    test_handshake();
    test_errors();
    printf("=== %s ===\n", (failures == 0) ? "ALL PASS" : "SOME FAIL");
    if (failures == 0) {
        __asm volatile("bkpt #0x7f");
    }
    __asm volatile("bkpt #0x7e");
    while (1) {
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
    (void)main();
    while (1) {
    }
}
