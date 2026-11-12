/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * PSOC Control C3M6 firmware test.  Replays the register sequence the
 * wolfBoot HAL (hal/psoc_c3.c) uses: PERI group ungating, HSIOM pin claim
 * and mux, PERI_PCLK divider start with the EN poll, SCB3 UART console, and
 * the boot ROM flash API (erase_row / program_row / write_row through the
 * Secure SBUS alias) with the CBUS read-back wolfBoot relies on.
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

/* Secure aliases, as wolfBoot runs. */
#define PERI_BASE       0x52000000u
#define PERI_PCLK_BASE  0x52040000u
#define HSIOM_BASE      0x52400000u
#define GPIO_BASE       0x52410000u
#define SCB3_BASE       0x52860000u

#define PERI_GR_SL_CTL(g)   (*(volatile uint32_t *)(PERI_BASE + 0x4000u + (g) * 0x40u + 0x10u))
#define PERI_GR_SL_CTL2(g)  (*(volatile uint32_t *)(PERI_BASE + 0x4000u + (g) * 0x40u + 0x14u))

#define PCLK_GR(g)              (PERI_PCLK_BASE + (g) * 0x2000u)
#define PCLK_DIV_CMD(g)         (*(volatile uint32_t *)(PCLK_GR(g) + 0x000u))
#define PCLK_CLOCK_CTL(g, i)    (*(volatile uint32_t *)(PCLK_GR(g) + 0xC00u + (i) * 4u))
#define PCLK_DIV_16_5_CTL(g, d) (*(volatile uint32_t *)(PCLK_GR(g) + 0x1800u + (d) * 4u))
#define PCLK_DIV_CMD_ENABLE     (1u << 31)
#define PCLK_DIV_CMD_PA_NONE    ((3u << 24) | (0xFFu << 16))
#define PCLK_TYPE_16_5          2u

#define HSIOM_PORT_SEL(p, pin)  (*(volatile uint32_t *)(HSIOM_BASE + (p) * 0x10u + (((pin) < 4u) ? 0x0u : 0x4u)))
#define HSIOM_NSMASK(p)         (*(volatile uint32_t *)(HSIOM_BASE + 0x1000u + (p) * 0x10u))
#define GPIO_PRT_CFG(p)         (*(volatile uint32_t *)(GPIO_BASE + (p) * 0x80u + 0x44u))
#define GPIO_PRT_OUT(p)         (*(volatile uint32_t *)(GPIO_BASE + (p) * 0x80u + 0x00u))
#define GPIO_PRT_IN(p)          (*(volatile uint32_t *)(GPIO_BASE + (p) * 0x80u + 0x10u))

#define SCB_CTRL(b)             (*(volatile uint32_t *)((b) + 0x000u))
#define SCB_UART_CTRL(b)        (*(volatile uint32_t *)((b) + 0x040u))
#define SCB_UART_TX_CTRL(b)     (*(volatile uint32_t *)((b) + 0x044u))
#define SCB_UART_RX_CTRL(b)     (*(volatile uint32_t *)((b) + 0x048u))
#define SCB_TX_CTRL(b)          (*(volatile uint32_t *)((b) + 0x200u))
#define SCB_TX_FIFO_CTRL(b)     (*(volatile uint32_t *)((b) + 0x204u))
#define SCB_TX_FIFO_STATUS(b)   (*(volatile uint32_t *)((b) + 0x208u))
#define SCB_TX_FIFO_WR(b)       (*(volatile uint32_t *)((b) + 0x240u))
#define SCB_RX_CTRL(b)          (*(volatile uint32_t *)((b) + 0x300u))
#define SCB_RX_FIFO_CTRL(b)     (*(volatile uint32_t *)((b) + 0x304u))
#define SCB_CTRL_ENABLED        (1u << 31)
#define SCB_CTRL_MODE_UART      (2u << 24)

/* Boot ROM flash API (cyboot_flash_list.h). */
#define ROM_BASE                0x10800000u
#define ROM_SIZE                0x10000u
#define ROM_FUNC_TABLE          0x1080FF6Cu
#define ROM_FUNC_SKIP           29u
#define FLASH_ROW_SIZE          512u
#define FLASH_COL33_SIZE        16u
#define FLASH_ROW_WORDS         ((FLASH_ROW_SIZE + FLASH_COL33_SIZE) / 4u)
#define FLASH_CBUS_S            0x12000000u
#define FLASH_SBUS_S            0x32000000u
#define SBUS_ALIAS(a)           ((a) | 0x20000000u)
#define TEST_ROW                (FLASH_CBUS_S + 0x40000u)
#define CYBOOT_FLASH_SUCCESS    0x0D50B002u
#define CYBOOT_FLASH_ADDR_INVALID 0xBAF0004Cu

typedef struct {
    uint32_t min_count;
    uint32_t max_count;
    uint32_t min_page_addr;
    uint32_t scratch_row_idx;
} cyboot_flash_refresh_t;

typedef struct {
    uint32_t flags;
    uint32_t hv_params_addr;
    cyboot_flash_refresh_t *refresh;
    void (*callback_pre_irq)(void *ctx);
    void (*callback_post_irq)(void *ctx);
    void (*callback_complete)(void *ctx);
    uint32_t callback_param;
    uint32_t state;
    uint32_t flash_addr;
    uint32_t data_addr;
    uint32_t reserved[2];
} cyboot_flash_context_t;

typedef uint32_t (*erase_row_t)(uint32_t addr, cyboot_flash_context_t *ctx);
typedef uint32_t (*row_op_t)(uint32_t addr, uint32_t *data, cyboot_flash_context_t *ctx);

typedef struct {
    uint32_t reserved[ROM_FUNC_SKIP];
    erase_row_t erase_row;
    row_op_t program_row;
    row_op_t write_row;
} rom_flash_t;

#define ROM_FUNC ((const rom_flash_t *)ROM_FUNC_TABLE)

static cyboot_flash_refresh_t flash_refresh[8];
static cyboot_flash_context_t flash_ctx;
static uint32_t flash_row[FLASH_ROW_WORDS];

static int failures;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (cond) {                                                     \
            printf("PASS: %s\n", #cond);                                \
        } else {                                                        \
            printf("FAIL: %s\n", #cond);                                \
            failures++;                                                 \
        }                                                               \
    } while (0)

static void peri_init(void)
{
    uint32_t g;
    for (g = 1u; g < 6u; g++) {
        PERI_GR_SL_CTL2(g) = 0u;
        PERI_GR_SL_CTL(g) = 0xFFFFFFFFu;
    }
    __asm volatile("dsb" ::: "memory");
}

static void pin_setup(uint32_t port, uint32_t pin, uint32_t sel, uint32_t drive)
{
    uint32_t v;
    HSIOM_NSMASK(port) &= ~(1u << pin);
    v = HSIOM_PORT_SEL(port, pin);
    v &= ~(0xFFu << ((pin & 3u) * 8u));
    v |= sel << ((pin & 3u) * 8u);
    HSIOM_PORT_SEL(port, pin) = v;
    v = GPIO_PRT_CFG(port);
    v &= ~(0xFu << (pin * 4u));
    v |= drive << (pin * 4u);
    GPIO_PRT_CFG(port) = v;
}

static int pclk_setup(uint32_t gr, uint32_t idx, uint32_t div, uint32_t int_div, uint32_t frac)
{
    uint32_t timeout = 100000u;
    PCLK_DIV_16_5_CTL(gr, div) = (int_div << 8) | (frac << 3);
    PCLK_CLOCK_CTL(gr, idx) = (PCLK_TYPE_16_5 << 8) | div;
    PCLK_DIV_CMD(gr) = PCLK_DIV_CMD_ENABLE | PCLK_DIV_CMD_PA_NONE | (PCLK_TYPE_16_5 << 8) | div;
    while (timeout > 0u) {
        if ((PCLK_DIV_16_5_CTL(gr, div) & 1u) != 0u) {
            return 0;
        }
        timeout--;
    }
    return -1;
}

static int uart_ready;

static void uart_init(void)
{
    peri_init();
    pin_setup(6u, 2u, 16u, 0x8u);
    pin_setup(6u, 3u, 16u, 0x6u);
    /* 48 MHz / (115200 * 8) = 52.08: INT16_DIV 51, FRAC5 3 */
    if (pclk_setup(4u, 6u, 1u, 51u, 3u) != 0) {
        return;
    }
    SCB_CTRL(SCB3_BASE) = 0u;
    SCB_UART_CTRL(SCB3_BASE) = 0u;
    SCB_UART_TX_CTRL(SCB3_BASE) = 1u;
    SCB_UART_RX_CTRL(SCB3_BASE) = 1u;
    SCB_TX_CTRL(SCB3_BASE) = 7u;
    SCB_RX_CTRL(SCB3_BASE) = 7u;
    SCB_TX_FIFO_CTRL(SCB3_BASE) = 0x3Fu;
    SCB_RX_FIFO_CTRL(SCB3_BASE) = 0x3Fu;
    SCB_CTRL(SCB3_BASE) = SCB_CTRL_ENABLED | SCB_CTRL_MODE_UART | 7u;
    uart_ready = 1;
}

void scb3_putc(char c)
{
    if (!uart_ready) {
        return;
    }
    while ((SCB_TX_FIFO_STATUS(SCB3_BASE) & 0x1FFu) >= 8u) {
    }
    SCB_TX_FIFO_WR(SCB3_BASE) = (uint32_t)(uint8_t)c;
}

static int rom_ptr_valid(const void *fn)
{
    uint32_t a = (uint32_t)fn;
    return (a >= ROM_BASE) && (a < ROM_BASE + ROM_SIZE) && ((a & 1u) != 0u);
}

static uint32_t rom_erase(uint32_t addr)
{
    flash_ctx.flags = 0u;
    flash_ctx.refresh = flash_refresh;
    return ROM_FUNC->erase_row(SBUS_ALIAS(addr), &flash_ctx);
}

static uint32_t rom_write(uint32_t addr, const uint8_t *data, uint32_t len, int program)
{
    memcpy(flash_row, (const void *)addr, FLASH_ROW_SIZE);
    memcpy(flash_row, data, len);
    memset((uint8_t *)flash_row + FLASH_ROW_SIZE, 0, FLASH_COL33_SIZE);
    flash_ctx.flags = 0u;
    flash_ctx.refresh = flash_refresh;
    if (program) {
        return ROM_FUNC->program_row(SBUS_ALIAS(addr), flash_row, &flash_ctx);
    }
    return ROM_FUNC->write_row(SBUS_ALIAS(addr), flash_row, &flash_ctx);
}

static int row_is(uint32_t addr, uint8_t value)
{
    const volatile uint8_t *p = (const volatile uint8_t *)addr;
    uint32_t i;
    for (i = 0; i < FLASH_ROW_SIZE; i++) {
        if (p[i] != value) {
            return 0;
        }
    }
    return 1;
}

static void flash_test(void)
{
    static uint8_t pattern[FLASH_ROW_SIZE];
    const volatile uint32_t *row = (const volatile uint32_t *)TEST_ROW;
    const volatile uint32_t *sbus = (const volatile uint32_t *)SBUS_ALIAS(TEST_ROW);
    uint32_t i;

    CHECK(rom_ptr_valid((const void *)ROM_FUNC->erase_row));
    CHECK(rom_ptr_valid((const void *)ROM_FUNC->program_row));
    CHECK(rom_ptr_valid((const void *)ROM_FUNC->write_row));

    /* The API wants the SBUS view: the CBUS address wolfBoot links at fails. */
    CHECK(ROM_FUNC->erase_row(TEST_ROW, &flash_ctx) == CYBOOT_FLASH_ADDR_INVALID);

    CHECK(rom_erase(TEST_ROW) == CYBOOT_FLASH_SUCCESS);
    CHECK(row_is(TEST_ROW, 0x00u));
    CHECK(flash_refresh[2].max_count == 1u);

    for (i = 0; i < FLASH_ROW_SIZE; i++) {
        pattern[i] = (uint8_t)(i * 7u);
    }
    CHECK(rom_write(TEST_ROW, pattern, FLASH_ROW_SIZE, 1) == CYBOOT_FLASH_SUCCESS);
    CHECK(memcmp((const void *)TEST_ROW, pattern, FLASH_ROW_SIZE) == 0);
    CHECK(row[1] == sbus[1]);

    /* A partial write keeps the rest of the row, as the HAL read-modify-writes. */
    memset(pattern, 0xA5, 16u);
    CHECK(rom_write(TEST_ROW, pattern, 16u, 0) == CYBOOT_FLASH_SUCCESS);
    CHECK(memcmp((const void *)TEST_ROW, pattern, FLASH_ROW_SIZE) == 0);
    CHECK(row[0] == 0xA5A5A5A5u);
    CHECK(flash_refresh[2].max_count == 3u);

    /* Erase again and the next row is untouched. */
    CHECK(rom_erase(TEST_ROW) == CYBOOT_FLASH_SUCCESS);
    CHECK(row_is(TEST_ROW, 0x00u));
    CHECK(!row_is(TEST_ROW + FLASH_ROW_SIZE, 0x00u));
}

static void gpio_test(void)
{
    pin_setup(7u, 7u, 0u, 0x6u);
    GPIO_PRT_OUT(7u) |= (1u << 7);
    CHECK((GPIO_PRT_IN(7u) & (1u << 7)) != 0u);
    GPIO_PRT_OUT(7u) &= ~(1u << 7);
    CHECK((GPIO_PRT_IN(7u) & (1u << 7)) == 0u);
    /* An unclaimed pin ignores the Secure write and reads back zero. */
    GPIO_PRT_OUT(7u) |= (1u << 6);
    CHECK((GPIO_PRT_OUT(7u) & (1u << 6)) == 0u);
}

int main(void)
{
    uart_init();
    printf("PSC3M6 test start\n");
    CHECK(uart_ready == 1);
    flash_test();
    gpio_test();
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
