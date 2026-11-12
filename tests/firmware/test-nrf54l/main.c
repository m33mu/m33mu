/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * nRF54L15 / nRF54LM20 test. Replays the register sequences of wolfBoot's
 * hal/nrf54l.c: clocks, ICACHE, UARTE EasyDMA, RRAMC, CRACEN RNG, SPU/MPC.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define REG32(a) (*(volatile uint32_t *)(a))

#ifdef NRF54LM20
#define PART_NAME        "nRF54LM20"
#define RRAMC_BASE       0x5004E000u
#define CRACEN_BASE      0x50059000u
#define CRACENCORE_BASE  0x50010000u
#define TAMPC_BASE       0x500EF000u
#define FICR_RAM_KB      512u
#define FICR_RRAM_KB     2036u
#define CLOCK_POWER_IRQN 270u
#else
#define PART_NAME        "nRF54L15"
#define RRAMC_BASE       0x5004B000u
#define CRACEN_BASE      0x50048000u
#define CRACENCORE_BASE  0x51800000u
#define TAMPC_BASE       0x500DC000u
#define FICR_RAM_KB      256u
#define FICR_RRAM_KB     1524u
#define CLOCK_POWER_IRQN 261u
#endif

extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;
extern void __libc_init_array(void);

#define FICR_BASE            0x00FFC000u
#define FICR_INFO_RAM        REG32(FICR_BASE + 0x328u)
#define FICR_INFO_RRAM       REG32(FICR_BASE + 0x32Cu)
#define FICR_XOSC32KTRIM     REG32(FICR_BASE + 0x624u)

#define CLOCK_BASE                 0x5010E000u
#define CLOCK_TASKS_XOSTART        REG32(CLOCK_BASE + 0x000u)
#define CLOCK_TASKS_LFCLKSTART     REG32(CLOCK_BASE + 0x010u)
#define CLOCK_TASKS_CAL            REG32(CLOCK_BASE + 0x018u)
#define CLOCK_EVENTS_DONE          REG32(CLOCK_BASE + 0x10Cu)
#define CLOCK_INTENSET             REG32(CLOCK_BASE + 0x304u)
#define CLOCK_INTENCLR             REG32(CLOCK_BASE + 0x308u)
#define CLOCK_INT_DONE             (1u << 3)
#define NVIC_ISER(n)               REG32(0xE000E100u + (n) * 4u)
#define NVIC_ICER(n)               REG32(0xE000E180u + (n) * 4u)
#define CLOCK_EVENTS_XOSTARTED     REG32(CLOCK_BASE + 0x100u)
#define CLOCK_EVENTS_LFCLKSTARTED  REG32(CLOCK_BASE + 0x108u)
#define CLOCK_XO_STAT              REG32(CLOCK_BASE + 0x40Cu)
#define CLOCK_LFCLK_SRC            REG32(CLOCK_BASE + 0x440u)
#define CLOCK_LFCLK_STAT           REG32(CLOCK_BASE + 0x44Cu)
#define CLOCK_STAT_RUNNING         (1u << 16)
#define CLOCK_LFCLK_SRC_LFXO       1u

#define OSC_BASE                   0x50120000u
#define OSC_PLL_FREQ               REG32(OSC_BASE + 0x800u)
#define OSC_PLL_CURRENTFREQ        REG32(OSC_BASE + 0x804u)
#define OSC_XOSC32KI_INTCAP        REG32(OSC_BASE + 0x904u)
#define OSC_PLL_FREQ_CK128M        1u

#define RRAMC_TASKS_COMMITWRITEBUF    REG32(RRAMC_BASE + 0x008u)
#define RRAMC_READY                   REG32(RRAMC_BASE + 0x400u)
#define RRAMC_READYNEXT               REG32(RRAMC_BASE + 0x404u)
#define RRAMC_BUFSTATUS_WRITEBUFEMPTY REG32(RRAMC_BASE + 0x418u)
#define RRAMC_CONFIG                  REG32(RRAMC_BASE + 0x500u)
#define RRAMC_CONFIG_WEN              1u
#define RRAMC_CONFIG_WRITEBUFSIZE(n)  ((uint32_t)(n) << 8)

#define ICACHE_BASE                   0xE0082000u
#define ICACHE_TASKS_INVALIDATECACHE  REG32(ICACHE_BASE + 0x008u)
#define ICACHE_STATUS                 REG32(ICACHE_BASE + 0x400u)
#define ICACHE_ENABLE                 REG32(ICACHE_BASE + 0x404u)

#define GPIO_P1_BASE          0x500D8200u
#define GPIO_OUTSET(b)        REG32((b) + 0x004u)
#define GPIO_PIN_CNF(b, p)    REG32((b) + 0x080u + (p) * 4u)

#define UARTE20_BASE              0x500C6000u
#define UARTE_TASKS_DMA_TX_START  REG32(UARTE20_BASE + 0x050u)
#define UARTE_EVENTS_DMA_TX_END   REG32(UARTE20_BASE + 0x168u)
#define UARTE_EVENTS_DMA_TX_BUSERROR REG32(UARTE20_BASE + 0x170u)
#define UARTE_ENABLE              REG32(UARTE20_BASE + 0x500u)
#define UARTE_BAUDRATE            REG32(UARTE20_BASE + 0x524u)
#define UARTE_CONFIG              REG32(UARTE20_BASE + 0x56Cu)
#define UARTE_PSEL_TXD            REG32(UARTE20_BASE + 0x604u)
#define UARTE_PSEL_CTS            REG32(UARTE20_BASE + 0x608u)
#define UARTE_PSEL_RXD            REG32(UARTE20_BASE + 0x60Cu)
#define UARTE_PSEL_RTS            REG32(UARTE20_BASE + 0x610u)
#define UARTE_DMA_TX_PTR          REG32(UARTE20_BASE + 0x73Cu)
#define UARTE_DMA_TX_MAXCNT       REG32(UARTE20_BASE + 0x740u)
#define UARTE_DMA_TX_AMOUNT       REG32(UARTE20_BASE + 0x744u)

#define CRACEN_ENABLE             REG32(CRACEN_BASE + 0x400u)
#define CRACEN_ENABLE_RNG         (1u << 1)
#define RNG_BASE                  (CRACENCORE_BASE + 0x1000u)
#define RNG_CONTROL               REG32(RNG_BASE + 0x000u)
#define RNG_FIFOLEVEL             REG32(RNG_BASE + 0x004u)
#define RNG_STATUS                REG32(RNG_BASE + 0x030u)
#define RNG_INITWAITVAL           REG32(RNG_BASE + 0x034u)
#define RNG_SWOFFTMRVAL           REG32(RNG_BASE + 0x040u)
#define RNG_CLKDIV                REG32(RNG_BASE + 0x044u)
#define RNG_WARMUPPERIOD          REG32(RNG_BASE + 0x034u)
#define RNG_SAMPLINGPERIOD        REG32(RNG_BASE + 0x044u)
#define RNG_FIFO                  REG32(RNG_BASE + 0x080u)
#define RNG_CONTROL_ENABLE        (1u << 0)
#define RNG_CONTROL_SOFTRST       (1u << 8)
#define RNG_STATE(status)         (((status) >> 1) & 0x7u)
#define RNG_STATE_RESET           0u
#define RNG_STATE_STARTUP         1u
#define RNG_STATE_ERROR           5u

#define SPU20_BASE                0x500C0000u
#define SPU30_BASE                0x50100000u
#define SPU_PERIPH_PERM(spu, n)   REG32((spu) + 0x500u + (n) * 4u)
#define SPU_FEATURE_GPIO_PIN(spu, g, p) REG32((spu) + 0x800u + (g) * 0x80u + (p) * 4u)
#define SPU_SECATTR               (1u << 4)
#define SPU_PRESENT               (1u << 31)

#define MPC00_BASE                0x50041000u
#define MPC_OVERRIDE_CONFIG(n)    REG32(MPC00_BASE + 0x800u + (n) * 0x20u)
#define MPC_OVERRIDE_STARTADDR(n) REG32(MPC00_BASE + 0x804u + (n) * 0x20u)
#define MPC_OVERRIDE_ENDADDR(n)   REG32(MPC00_BASE + 0x808u + (n) * 0x20u)
#define MPC_OVERRIDE_PERM(n)      REG32(MPC00_BASE + 0x810u + (n) * 0x20u)
#define MPC_OVERRIDE_PERMMASK(n)  REG32(MPC00_BASE + 0x814u + (n) * 0x20u)
#define MPC_CONFIG_LOCK           (1u << 8)
#define MPC_CONFIG_ENABLE         (1u << 9)
#define MPC_PERM_RWX              0x7u
#define MPC_PERM_SECURE           (1u << 3)

#define TAMPC_DOMAIN0_DBGEN_CTRL  REG32(TAMPC_BASE + 0x500u)

/* Well clear of the image and inside the RRAM of both parts. */
#define SCRATCH 0x000F0000u

static uint8_t uart_tx_buf[64];
static uint32_t erase_blank[4] = {
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu
};
static int failures;
static volatile uint32_t clock_irq_count;

void uart_write_raw(const char *buf, unsigned int len)
{
    while (len > 0u) {
        unsigned int xfer = (len > sizeof(uart_tx_buf)) ? sizeof(uart_tx_buf) : len;
        memcpy(uart_tx_buf, buf, xfer);
        UARTE_EVENTS_DMA_TX_END = 0u;
        UARTE_EVENTS_DMA_TX_BUSERROR = 0u;
        UARTE_DMA_TX_PTR = (uint32_t)uart_tx_buf;
        UARTE_DMA_TX_MAXCNT = xfer;
        UARTE_TASKS_DMA_TX_START = 1u;
        while (UARTE_EVENTS_DMA_TX_END == 0u && UARTE_EVENTS_DMA_TX_BUSERROR == 0u) {
        }
        if (UARTE_EVENTS_DMA_TX_BUSERROR != 0u) {
            break;
        }
        len -= xfer;
        buf += xfer;
    }
}

static void report(const char *name, int ok)
{
    printf("%-34s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) {
        failures++;
    }
}

static void uart_init(void)
{
    UARTE_ENABLE = 0u;
    GPIO_OUTSET(GPIO_P1_BASE) = 1u << 4;
    GPIO_PIN_CNF(GPIO_P1_BASE, 4) = 1u;
    GPIO_PIN_CNF(GPIO_P1_BASE, 5) = 0u;
    UARTE_PSEL_TXD = 4u | (1u << 5);
    UARTE_PSEL_RXD = 5u | (1u << 5);
    UARTE_PSEL_CTS = 1u << 31;
    UARTE_PSEL_RTS = 1u << 31;
    UARTE_BAUDRATE = 0x01D60000u;
    UARTE_CONFIG = 8u << 9;
    UARTE_ENABLE = 8u;
}

static int test_uart_amount(void)
{
    static const char msg[] = "uarte20: EasyDMA TX\n";
    uart_write_raw(msg, sizeof(msg) - 1u);
    return UARTE_DMA_TX_AMOUNT == sizeof(msg) - 1u;
}

static int test_clocks(void)
{
    uint32_t intcap = 0x17u;

    OSC_PLL_FREQ = OSC_PLL_FREQ_CK128M;
    while ((OSC_PLL_CURRENTFREQ & 0x3u) != OSC_PLL_FREQ_CK128M) {
    }

    CLOCK_EVENTS_XOSTARTED = 0u;
    CLOCK_TASKS_XOSTART = 1u;
    while (CLOCK_EVENTS_XOSTARTED == 0u || (CLOCK_XO_STAT & CLOCK_STAT_RUNNING) == 0u) {
    }

    if (FICR_XOSC32KTRIM != 0xFFFFFFFFu) {
        intcap = (FICR_XOSC32KTRIM >> 16) & 0x1Fu;
    }
    OSC_XOSC32KI_INTCAP = intcap;
    CLOCK_EVENTS_LFCLKSTARTED = 0u;
    CLOCK_LFCLK_SRC = CLOCK_LFCLK_SRC_LFXO;
    CLOCK_TASKS_LFCLKSTART = 1u;
    while (CLOCK_EVENTS_LFCLKSTARTED == 0u ||
           (CLOCK_LFCLK_STAT & 0x3u) != CLOCK_LFCLK_SRC_LFXO ||
           (CLOCK_LFCLK_STAT & CLOCK_STAT_RUNNING) == 0u) {
    }
    return OSC_XOSC32KI_INTCAP == intcap;
}

void CLOCK_POWER_IRQHandler(void)
{
    CLOCK_EVENTS_DONE = 0u;
    clock_irq_count++;
}

/* CLOCK_POWER sits above IRQ 255: the NVIC must reach it. */
static int test_clock_irq(void)
{
    uint32_t polls;

    clock_irq_count = 0u;
    CLOCK_EVENTS_DONE = 0u;
    CLOCK_INTENSET = CLOCK_INT_DONE;
    NVIC_ISER(CLOCK_POWER_IRQN / 32u) = 1u << (CLOCK_POWER_IRQN % 32u);
    __asm volatile("cpsie i");
    CLOCK_TASKS_CAL = 1u;
    for (polls = 0; polls < 100000u && clock_irq_count == 0u; polls++) {
    }
    NVIC_ICER(CLOCK_POWER_IRQN / 32u) = 1u << (CLOCK_POWER_IRQN % 32u);
    CLOCK_INTENCLR = CLOCK_INT_DONE;
    return clock_irq_count == 1u;
}

static void icache_invalidate(void)
{
    ICACHE_TASKS_INVALIDATECACHE = 1u;
    while ((ICACHE_STATUS & 1u) != 0u) {
    }
}

static int test_icache(void)
{
    icache_invalidate();
    ICACHE_ENABLE = 1u;
    return ICACHE_ENABLE == 1u && ICACHE_STATUS == 0u;
}

static void rram_write_enable(int enable, uint32_t writebuf)
{
    uint32_t cfg = RRAMC_CONFIG & ~(RRAMC_CONFIG_WEN | RRAMC_CONFIG_WRITEBUFSIZE(0x3Fu));
    if (enable) {
        cfg |= RRAMC_CONFIG_WEN | RRAMC_CONFIG_WRITEBUFSIZE(writebuf);
    }
    RRAMC_CONFIG = cfg;
    while ((RRAMC_READY & 1u) == 0u) {
    }
}

static void rram_commit(void)
{
    if ((RRAMC_BUFSTATUS_WRITEBUFEMPTY & 1u) == 0u) {
        RRAMC_TASKS_COMMITWRITEBUF = 1u;
        while ((RRAMC_READY & 1u) == 0u) {
        }
        while ((RRAMC_BUFSTATUS_WRITEBUFEMPTY & 1u) == 0u) {
        }
    }
}

/* Same word/byte split as flash_program_range() in the wolfBoot HAL. */
static void rram_program(uint32_t address, const uint8_t *data, int len)
{
    int i = 0;
    while (i < len) {
        while ((RRAMC_READYNEXT & 1u) == 0u) {
        }
        if ((((address + i) & 0x3u) == 0u) && ((((uintptr_t)(data + i)) & 0x3u) == 0u) &&
            (len - i) >= 4) {
            REG32(address + i) = *(const uint32_t *)(data + i);
            i += 4;
        } else {
            uint32_t word;
            volatile uint32_t *dst = (volatile uint32_t *)((address + i) & ~0x3u);
            word = *dst;
            ((uint8_t *)&word)[(address + i) & 0x3u] = data[i];
            *dst = word;
            i++;
        }
    }
}

static int test_rramc_write_erase(void)
{
    static const uint32_t pattern[4] = {
        0x12345678u, 0x9ABCDEF0u, 0x0F1E2D3Cu, 0x00000000u
    };
    uint32_t off;
    int ok;

    rram_write_enable(1, 0u);
    for (off = 0; off < 0x100u; off += sizeof(erase_blank)) {
        rram_program(SCRATCH + off, (const uint8_t *)erase_blank, sizeof(erase_blank));
    }
    rram_program(SCRATCH, (const uint8_t *)pattern, sizeof(pattern));
    ok = (RRAMC_BUFSTATUS_WRITEBUFEMPTY & 1u) != 0u;
    rram_commit();
    rram_write_enable(0, 0u);
    icache_invalidate();
    ok = ok && memcmp((const void *)SCRATCH, pattern, sizeof(pattern)) == 0;

    /* RRAM is overwritten in place: 0xFFFFFFFF stores are the erase. */
    rram_write_enable(1, 0u);
    rram_program(SCRATCH, (const uint8_t *)erase_blank, sizeof(erase_blank));
    rram_commit();
    rram_write_enable(0, 0u);
    icache_invalidate();
    return ok && memcmp((const void *)SCRATCH, erase_blank, sizeof(erase_blank)) == 0;
}

static int test_rramc_unaligned(void)
{
    static const uint8_t bytes[3] = { 0xA1u, 0xB2u, 0xC3u };
    int ok;

    rram_write_enable(1, 0u);
    rram_program(SCRATCH + 0x41u, bytes, sizeof(bytes));
    rram_commit();
    rram_write_enable(0, 0u);
    ok = REG32(SCRATCH + 0x40u) == 0xC3B2A1FFu;
    ok = ok && REG32(SCRATCH + 0x44u) == 0xFFFFFFFFu;
    return ok;
}

static int test_rramc_writebuf(void)
{
    int ok;

    rram_write_enable(1, 4u);
    while ((RRAMC_READYNEXT & 1u) == 0u) {
    }
    REG32(SCRATCH + 0x80u) = 0xCAFEF00Du;
    ok = (RRAMC_BUFSTATUS_WRITEBUFEMPTY & 1u) == 0u;
    rram_commit();
    ok = ok && (RRAMC_BUFSTATUS_WRITEBUFEMPTY & 1u) != 0u;
    rram_write_enable(0, 0u);
    return ok && REG32(SCRATCH + 0x80u) == 0xCAFEF00Du;
}

static uint32_t rng_state(void)
{
    return RNG_STATE(RNG_STATUS);
}

static int test_cracen_rng(void)
{
    uint32_t words[8];
    uint32_t state = RNG_STATE_RESET;
    uint32_t polls;
    uint32_t avail;
    unsigned int i;
    int distinct = 0;

    CRACEN_ENABLE |= CRACEN_ENABLE_RNG;
    RNG_CONTROL = RNG_CONTROL_SOFTRST;
#ifdef NRF54LM20
    RNG_SAMPLINGPERIOD = 0xFFFu;
    RNG_WARMUPPERIOD = 512u;
#else
    RNG_CLKDIV = 0u;
    RNG_INITWAITVAL = 512u;
    RNG_SWOFFTMRVAL = 0u;
#endif
    RNG_CONTROL = RNG_CONTROL_ENABLE | (4u << 16);
    for (polls = 0; polls < 100000u; polls++) {
        state = rng_state();
        if (state != RNG_STATE_RESET && state != RNG_STATE_STARTUP) {
            break;
        }
    }
    if (polls == 100000u || state == RNG_STATE_ERROR) {
        return 0;
    }

    i = 0;
    while (i < 8u) {
        avail = RNG_FIFOLEVEL;
        if (avail == 0u) {
            return 0;
        }
        while (avail-- > 0u && i < 8u) {
            words[i++] = RNG_FIFO;
        }
    }
    for (i = 1; i < 8u; i++) {
        if (words[i] != words[0]) {
            distinct = 1;
        }
    }

    RNG_CONTROL = 0u;
    CRACEN_ENABLE &= ~CRACEN_ENABLE_RNG;
    return distinct && rng_state() == RNG_STATE_RESET && RNG_FIFOLEVEL == 0u;
}

static int test_spu(void)
{
    uint32_t pin;
    int ok;

    /* UARTE22: UARTE20 carries this test's output on its secure alias. */
    ok = (SPU_PERIPH_PERM(SPU20_BASE, 8) & SPU_SECATTR) != 0u;
    SPU_PERIPH_PERM(SPU20_BASE, 8) &= ~SPU_SECATTR;
    ok = ok && (SPU_PERIPH_PERM(SPU20_BASE, 8) & SPU_SECATTR) == 0u;
    ok = ok && (SPU_PERIPH_PERM(SPU20_BASE, 8) & SPU_PRESENT) != 0u;
    SPU_PERIPH_PERM(SPU30_BASE, 4) &= ~SPU_SECATTR;
    for (pin = 0; pin < 32u; pin++) {
        SPU_FEATURE_GPIO_PIN(SPU20_BASE, 1, pin) &= ~SPU_SECATTR;
    }
    ok = ok && (SPU_FEATURE_GPIO_PIN(SPU20_BASE, 1, 31) & SPU_SECATTR) == 0u;
    return ok && (SPU_PERIPH_PERM(SPU30_BASE, 4) & SPU_SECATTR) == 0u;
}

static void mpc_region_set_ns(uint32_t region, uint32_t start, uint32_t end)
{
    MPC_OVERRIDE_STARTADDR(region) = start;
    MPC_OVERRIDE_ENDADDR(region) = end;
    MPC_OVERRIDE_PERM(region) = MPC_PERM_RWX;
    MPC_OVERRIDE_PERMMASK(region) = MPC_PERM_RWX | MPC_PERM_SECURE;
    MPC_OVERRIDE_CONFIG(region) = MPC_CONFIG_ENABLE;
}

static int test_mpc(void)
{
    int ok;

    mpc_region_set_ns(0, 0x00050000u, 0x000DB000u);
    mpc_region_set_ns(1, 0x20010000u, 0x20040000u);
    ok = MPC_OVERRIDE_STARTADDR(1) == 0x20010000u && MPC_OVERRIDE_ENDADDR(1) == 0x20040000u;
    ok = ok && MPC_OVERRIDE_CONFIG(0) == MPC_CONFIG_ENABLE;

    /* A locked region ignores further writes until reset. */
    MPC_OVERRIDE_CONFIG(2) = MPC_CONFIG_LOCK;
    MPC_OVERRIDE_STARTADDR(2) = 0x12345000u;
    ok = ok && MPC_OVERRIDE_STARTADDR(2) == 0u;

    /* The secure world keeps its view of secure RRAM. */
    return ok && REG32(SCRATCH + 0x80u) == 0xCAFEF00Du;
}

static int test_tampc(void)
{
    if ((TAMPC_DOMAIN0_DBGEN_CTRL & 0x2u) == 0u) {
        TAMPC_DOMAIN0_DBGEN_CTRL = 0x50FA00F0u;
        TAMPC_DOMAIN0_DBGEN_CTRL = 0x50FA0001u;
    }
    return TAMPC_DOMAIN0_DBGEN_CTRL == 0x50FA0001u;
}

int main(void)
{
    uart_init();
    printf("%s wolfBoot HAL sequence test\n", PART_NAME);

    report("uarte20: DMA.TX.AMOUNT", test_uart_amount());
    report("ficr: INFO.RAM/RRAM", FICR_INFO_RAM == FICR_RAM_KB && FICR_INFO_RRAM == FICR_RRAM_KB);
    report("clock: PLL, HFXO, LFXO", test_clocks());
    report("clock: CLOCK_POWER interrupt", test_clock_irq());
    report("icache: invalidate + enable", test_icache());
    report("rramc: program + 0xFF erase", test_rramc_write_erase());
    report("rramc: unaligned byte RMW", test_rramc_unaligned());
    report("rramc: write buffer + commit", test_rramc_writebuf());
    report("cracen: RNG init + FIFO", test_cracen_rng());
    report("spu: PERIPH.PERM SECATTR", test_spu());
    report("mpc00: OVERRIDE regions", test_mpc());
    report("tampc: DBGEN open", test_tampc());

    if (failures == 0) {
        printf("ALL PASS\n");
        __asm volatile("bkpt #0x7f");
    } else {
        printf("%d FAILED\n", failures);
        __asm volatile("bkpt #0x7e");
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
