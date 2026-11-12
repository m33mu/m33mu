/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <stdint.h>
#include <arm_cmse.h>

extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;
extern uint32_t _estack;

#define REG(a)            (*(volatile uint32_t *)(a))

#define RCC_APB1LENR      REG(0x40021058u)
#define USART3_BASE       0x40004800u
#define USART_CR1         REG(USART3_BASE + 0x00u)
#define USART_BRR         REG(USART3_BASE + 0x0Cu)
#define USART_ISR         REG(USART3_BASE + 0x1Cu)
#define USART_TDR         REG(USART3_BASE + 0x28u)

#define SCB_SHCSR         REG(0xE000ED24u)
#define SHCSR_SECUREFAULTENA (1u << 19)
#define SAU_CTRL          REG(0xE000EDD0u)
#define SAU_RNR           REG(0xE000EDD8u)
#define SAU_RBAR          REG(0xE000EDDCu)
#define SAU_RLAR          REG(0xE000EDE0u)
#define SAU_SFSR          REG(0xE000EDE4u)
#define SAU_SFAR          REG(0xE000EDE8u)

#define MPCBB1_S_VCTR(n)  REG(0x50032D00u + 4u * (n))
#define MPCBB1_NS_VCTR_ADDR(n) (0x40032D00u + 4u * (n))
#define TZSC_SECCFGR2     REG(0x50032414u)
#define TZSC_RNGSEC       (1u << 14)
#define FLASH_OPTR_S      REG(0x50022040u)
#define RNG_NS_ADDR       0x420C0800u

/* Non-secure world: SRAM1 0x20020000..0x2002FFFF, code at its start. */
#define NS_CODE           0x20020000u
#define NS_DATA           0x20020100u
#define NS_STACK_TOP      0x20030000u
/* A "peer" block the MPCBB keeps Secure while the SAU calls it NS. */
#define PEER_RAM_NS       0x20010000u
#define PEER_RAM_S        0x30010000u

#define PROBE_LOAD        0u
#define PROBE_STORE       1u

typedef uint32_t __attribute__((cmse_nonsecure_call)) (*ns_probe_fn)(uint32_t addr,
                                                                       uint32_t op,
                                                                       uint32_t value);

volatile uint32_t sf_count;
volatile uint32_t sf_sfar;
volatile uint32_t sf_sfsr;
static uint32_t failures;

static void putc_uart(char c)
{
    while ((USART_ISR & (1u << 7)) == 0u) {
    }
    USART_TDR = (uint32_t)c;
}

static void puts_uart(const char *s)
{
    while (*s != '\0') {
        putc_uart(*s++);
    }
}

static void puthex(uint32_t v)
{
    int i;
    puts_uart("0x");
    for (i = 28; i >= 0; i -= 4) {
        putc_uart("0123456789abcdef"[(v >> i) & 0xFu]);
    }
}

static void check(const char *what, int ok)
{
    puts_uart(ok ? "PASS " : "FAIL ");
    puts_uart(what);
    puts_uart("\r\n");
    if (!ok) {
        failures++;
    }
}

/* Every access in here is a 16-bit instruction: the fault handler skips it. */
__attribute__((naked, noinline, aligned(4)))
static uint32_t ns_probe_body(uint32_t addr, uint32_t op, uint32_t value)
{
    __asm volatile(
        "cmp r1, #0\n"
        "beq 1f\n"
        "str r2, [r0]\n"
        "movs r0, #0\n"
        "bx lr\n"
        "1:\n"
        "ldr r0, [r0]\n"
        "bx lr\n");
}

/* Non-secure accesses the model refuses: record and step over the access. */
void securefault_c(uint32_t exc_return)
{
    uint32_t *frame;

    sf_sfsr = SAU_SFSR;
    sf_sfar = SAU_SFAR;
    SAU_SFSR = sf_sfsr;
    sf_count++;
    if ((exc_return & (1u << 6)) != 0u) {
        puts_uart("FAIL SecureFault from Secure state\r\n");
        __asm volatile("bkpt #0x7e");
    }
    if ((exc_return & (1u << 2)) != 0u) {
        __asm volatile("mrs %0, psp_ns" : "=r"(frame));
    } else {
        __asm volatile("mrs %0, msp_ns" : "=r"(frame));
    }
    frame[6] += 2u;
}

__attribute__((naked)) void SecureFault_Handler(void)
{
    __asm volatile(
        "mov r0, lr\n"
        "b securefault_c\n");
}

void HardFault_Handler(void)
{
    puts_uart("FAIL HardFault\r\n");
    __asm volatile("bkpt #0x7e");
    while (1) {
    }
}

static void sau_region(uint32_t n, uint32_t base, uint32_t limit)
{
    SAU_RNR = n;
    SAU_RBAR = base & ~0x1Fu;
    SAU_RLAR = (limit & ~0x1Fu) | 1u;
}

static uint32_t ns_call(uint32_t addr, uint32_t op, uint32_t value, uint32_t *faults)
{
    ns_probe_fn fn = (ns_probe_fn)cmse_nsfptr_create(NS_CODE);
    uint32_t before = sf_count;
    uint32_t ret;

    sf_sfar = 0u;
    ret = fn(addr, op, value);
    *faults = sf_count - before;
    return ret;
}

static int faulted_at(uint32_t faults, uint32_t addr)
{
    /* AUVIOL (bit 3) with SFARVALID (bit 6). */
    return faults == 1u && sf_sfar == addr && (sf_sfsr & 0x48u) == 0x48u;
}

static void run(void)
{
    const uint16_t *src = (const uint16_t *)((uintptr_t)ns_probe_body & ~1u);
    volatile uint16_t *dst = (volatile uint16_t *)NS_CODE;
    uint32_t faults;
    uint32_t v;
    uint32_t i;

    check("OPTR reads TZEN=1 DBANK=1 RDP=0xAA", FLASH_OPTR_S == 0x9F4F70AAu);
    check("SRAM1 VCTR23 resets Secure", MPCBB1_S_VCTR(23) == 0xFFFFFFFFu);

    /* SRAM1 upper 64 KB Non-secure, as wolfBoot's hal_gtzc_init leaves it. */
    for (i = 16u; i < 24u; ++i) {
        MPCBB1_S_VCTR(i) = 0u;
    }
    sau_region(0u, 0x20000000u, 0x2003FFFFu);
    sau_region(1u, 0x40000000u, 0x4FFFFFFFu);
    sau_region(2u, 0x08000000u, 0x0807FFFFu);
    SAU_CTRL = 1u;
    SCB_SHCSR |= SHCSR_SECUREFAULTENA;
    __asm volatile("msr msp_ns, %0" : : "r"(NS_STACK_TOP));

    for (i = 0u; i < 8u; ++i) {
        dst[i] = src[i];
    }
    REG(PEER_RAM_S) = 0x11223344u;
    __asm volatile("dsb\nisb");

    (void)ns_call(NS_DATA, PROBE_STORE, 0xA5A5A5A5u, &faults);
    check("NS store to a Non-secure block", faults == 0u && REG(NS_DATA) == 0xA5A5A5A5u);

    (void)ns_call(PEER_RAM_NS, PROBE_STORE, 0xDEADBEEFu, &faults);
    check("NS store to a Secure block faults", faulted_at(faults, PEER_RAM_NS));
    check("Secure block keeps its value", REG(PEER_RAM_S) == 0x11223344u);

    v = ns_call(PEER_RAM_NS, PROBE_LOAD, 0u, &faults);
    check("NS load from a Secure block faults", faulted_at(faults, PEER_RAM_NS) &&
          v != 0x11223344u);

    (void)ns_call(MPCBB1_NS_VCTR_ADDR(8), PROBE_STORE, 0u, &faults);
    check("NS write to MPCBB1 VCTR8 is ignored", faults == 0u &&
          MPCBB1_S_VCTR(8) == 0xFFFFFFFFu);
    (void)ns_call(PEER_RAM_NS, PROBE_STORE, 0xDEADBEEFu, &faults);
    check("Secure block still faults after it", faulted_at(faults, PEER_RAM_NS));

    v = ns_call(RNG_NS_ADDR, PROBE_LOAD, 0u, &faults);
    check("NS RNG load with RNGSEC=0", faults == 0u);
    TZSC_SECCFGR2 |= TZSC_RNGSEC;
    v = ns_call(RNG_NS_ADDR, PROBE_LOAD, 0u, &faults);
    check("NS RNG load with RNGSEC=1 faults", faulted_at(faults, RNG_NS_ADDR));
    TZSC_SECCFGR2 &= ~TZSC_RNGSEC;

    v = ns_call(0x08000000u, PROBE_LOAD, 0u, &faults);
    check("NS load from Secure flash page reads zero", faults == 0u && v == 0u);
    check("Secure load of the same page", REG(0x0C000000u) == (uint32_t)&_estack);

    puts_uart("SecureFaults taken: ");
    puthex(sf_count);
    puts_uart("\r\n");
}

void Reset_Handler(void)
{
    uint32_t *s;
    uint32_t *d;

    for (s = &_sidata, d = &_sdata; d < &_edata; ) {
        *d++ = *s++;
    }
    for (d = &_sbss; d < &_ebss; ) {
        *d++ = 0u;
    }
    RCC_APB1LENR |= (1u << 18);
    USART_BRR = 4000000u / 115200u;
    USART_CR1 = (1u << 0) | (1u << 2) | (1u << 3);
    puts_uart("STM32L552 TrustZone isolation test\r\n");
    run();
    if (failures == 0u) {
        puts_uart("ALL PASS\r\n");
        __asm volatile("bkpt #0x7f");
    }
    puts_uart("FAILED\r\n");
    __asm volatile("bkpt #0x7e");
    while (1) {
    }
}

static void default_handler(void)
{
    puts_uart("FAIL unexpected exception\r\n");
    __asm volatile("bkpt #0x7e");
    while (1) {
    }
}

__attribute__((section(".isr_vector"), used))
const uint32_t vector_table[16] = {
    (uint32_t)&_estack,
    (uint32_t)&Reset_Handler,
    (uint32_t)&default_handler,
    (uint32_t)&HardFault_Handler,
    (uint32_t)&default_handler,
    (uint32_t)&default_handler,
    (uint32_t)&default_handler,
    (uint32_t)&SecureFault_Handler,
    0u, 0u, 0u,
    (uint32_t)&default_handler,
    (uint32_t)&default_handler,
    0u,
    (uint32_t)&default_handler,
    (uint32_t)&default_handler
};
