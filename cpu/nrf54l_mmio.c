/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <string.h>
#include <stdio.h>
#include "nrf54l_mmio.h"
#include "nrf_timers.h"
#include "m33mu/memmap.h"
#include "m33mu/flash_persist.h"
#include "m33mu/gpio.h"
#include "nrf_gpio.h"
#include "m33mu/host_rng.h"
#include "m33mu/code_cache.h"

#define PERIPH_BASE_NS 0x40000000u
#define PERIPH_BASE_S  0x50000000u
#define PERIPH_S_OFFSET 0x10000000u

#define FICR_BASE_NS 0x00FFC000u
#define FICR_SIZE    0x1000u

#define FICR_INFO_BASE 0x300u
#define FICR_INFO_RAM  0x328u
#define FICR_INFO_RRAM 0x32Cu

#define GPIO_P0_BASE_NS 0x4010A000u
#define GPIO_P1_BASE_NS 0x400D8200u
#define GPIO_P2_BASE_NS 0x40050400u
#define GPIO_P3_BASE_NS 0x400D8600u
#define GPIO_SIZE       0x300u

#define GPIO_OUT     0x000u
#define GPIO_OUTSET  0x004u
#define GPIO_OUTCLR  0x008u
#define GPIO_IN      0x00Cu
#define GPIO_DIR     0x010u
#define GPIO_DIRSET  0x014u
#define GPIO_DIRCLR  0x018u
#define GPIO_LATCH   0x020u
#define GPIO_DETECTMODE 0x024u
#define GPIO_PIN_CNF0 0x080u

#define RRAMC_SIZE   0x1000u

#define RRAMC_TASKS_COMMITWRITEBUF 0x008u
#define RRAMC_EVENTS_READY         0x104u
#define RRAMC_EVENTS_READYNEXT     0x108u
#define RRAMC_EVENTS_ACCESSERROR   0x10Cu
#define RRAMC_READY    0x400u
#define RRAMC_READYNEXT 0x404u
#define RRAMC_ACCESSERRORADDR 0x408u
#define RRAMC_BUFSTATUS_WRITEBUFEMPTY 0x418u
#define RRAMC_CONFIG   0x500u
#define RRAMC_ERASEALL 0x540u

#define RRAMC_CONFIG_WEN (1u << 0)
#define RRAMC_CONFIG_WRITEBUFSIZE_SHIFT 8u
#define RRAMC_CONFIG_WRITEBUFSIZE_MASK  0x3Fu
#define RRAMC_CONFIG_MASK (RRAMC_CONFIG_WEN | \
                           (RRAMC_CONFIG_WRITEBUFSIZE_MASK << RRAMC_CONFIG_WRITEBUFSIZE_SHIFT))
/* The write buffer holds WRITEBUFSIZE 128-bit words, at most 32. */
#define RRAMC_WRITEBUF_MAX 32u
#define RRAMC_LINE_SIZE    16u

#define CLOCK_BASE_NS 0x4010E000u
#define CLOCK_SIZE    0x1000u

#define CLOCK_TASKS_XOSTART   0x000u
#define CLOCK_TASKS_XOSTOP    0x004u
#define CLOCK_TASKS_PLLSTART  0x008u
#define CLOCK_TASKS_PLLSTOP   0x00Cu
#define CLOCK_TASKS_LFCLKSTART 0x010u
#define CLOCK_TASKS_LFCLKSTOP  0x014u
#define CLOCK_TASKS_CAL       0x018u
#define CLOCK_TASKS_XOTUNE    0x01Cu
#define CLOCK_TASKS_XOTUNEABORT 0x020u
#define CLOCK_TASKS_XO24MSTART 0x024u
#define CLOCK_TASKS_XO24MSTOP  0x028u

#define CLOCK_EVENTS_XOSTARTED  0x100u
#define CLOCK_EVENTS_PLLSTARTED 0x104u
#define CLOCK_EVENTS_LFCLKSTARTED 0x108u
#define CLOCK_EVENTS_DONE       0x10Cu
#define CLOCK_EVENTS_XOTUNED    0x110u
#define CLOCK_EVENTS_XOTUNEERROR 0x114u
#define CLOCK_EVENTS_XOTUNEFAILED 0x118u
#define CLOCK_EVENTS_XO24MSTARTED 0x11Cu

#define CLOCK_INTEN    0x300u
#define CLOCK_INTENSET 0x304u
#define CLOCK_INTENCLR 0x308u
#define CLOCK_INTPEND  0x30Cu

#define CLOCK_XO_RUN   0x408u
#define CLOCK_XO_STAT  0x40Cu
#define CLOCK_PLL_RUN  0x428u
#define CLOCK_PLL_STAT 0x42Cu
#define CLOCK_LFCLK_SRC 0x440u
#define CLOCK_LFCLK_RUN 0x448u
#define CLOCK_LFCLK_STAT 0x44Cu
#define CLOCK_LFCLK_SRCCOPY 0x450u
#define CLOCK_PLL24M_STAT 0x46Cu

#define OSCILLATORS_BASE_NS 0x40120000u
#define OSCILLATORS_SIZE    0x1000u

#define OSC_PLL_FREQ        0x800u
#define OSC_PLL_CURRENTFREQ 0x804u
#define OSC_XOSC32KI_INTCAP 0x904u

#define GRTC_BASE_NS 0x400E2000u
#define GRTC_SIZE    0x1000u

#define GRTC_TASKS_CAPTURE0 0x000u
#define GRTC_TASKS_START    0x060u
#define GRTC_TASKS_STOP     0x064u
#define GRTC_TASKS_CLEAR    0x068u

#define GRTC_EVENTS_COMPARE0 0x100u

#define GRTC_INTEN0    0x300u
#define GRTC_INTENSET0 0x304u
#define GRTC_INTENCLR0 0x308u
#define GRTC_INTPEND0  0x30Cu
#define GRTC_INT_GROUP_STRIDE 0x10u

#define GRTC_MODE      0x510u
#define GRTC_CC_BASE   0x520u
#define GRTC_CC_STRIDE 0x10u
#define GRTC_CC_CCL    0x000u
#define GRTC_CC_CCH    0x004u
#define GRTC_CC_CCADD  0x008u
#define GRTC_CC_CCEN   0x00Cu

#define GRTC_INTERVAL  0x6A8u
#define GRTC_STATUS_LFTIMER 0x6B0u
#define GRTC_STATUS_PWM     0x6B4u
#define GRTC_STATUS_CLKOUT  0x6B8u

#define GRTC_SYSCOUNTER_BASE   0x720u
#define GRTC_SYSCOUNTER_STRIDE 0x10u
#define GRTC_SYSCOUNTERL 0x000u
#define GRTC_SYSCOUNTERH 0x004u
#define GRTC_SYSCOUNTER_ACTIVE 0x008u

#define GRTC_CC_COUNT 12u
#define GRTC_SYSCOUNTER_COUNT 4u
#define GRTC_INT_GROUPS 4u
#define GRTC_IRQ_BASE 226

#define SPU_COUNT        4u
#define SPU_SIZE         0x1000u
#define SPU_PERIPH_PERM  0x500u
#define SPU_PERIPH_COUNT 64u
#define SPU_FEATURE_BASE 0x600u
#define SPU_FEATURE_GPIO 0x800u
#define SPU_FEATURE_END  0xDC0u

#define SPU_EVENTS_PERIPHACCERR  0x100u
#define SPU_PERIPHACCERR_ADDRESS 0x404u

#define SPU_PERM_SECATTR (1u << 4)
#define SPU_PERM_DMASEC  (1u << 5)
#define SPU_PERM_LOCK    (1u << 8)
#define SPU_PERM_RW_MASK (SPU_PERM_SECATTR | SPU_PERM_DMASEC | SPU_PERM_LOCK)
/* PRESENT, DMA=SeparateAttribute, SECUREMAPPING=UserSelectable, Secure with
 * secure DMA. The MDK lists both clear; firmware unsecures what it hands over. */
#define SPU_PERM_RESET   0x8000003Au
/* FEATURE entries reset Secure and unlocked; absent ones read as zero. */
#define SPU_FEATURE_RESET 0x00100010u
#define SPU_FEATURE_LOCK  (1u << 8)

#define MPC00_BASE_S 0x50041000u
#define MPC_SIZE     0x1000u

#define MPC_OVERRIDE_BASE   0x800u
#define MPC_OVERRIDE_STRIDE 0x20u
#define MPC_OVR_CONFIG    0x00u
#define MPC_OVR_STARTADDR 0x04u
#define MPC_OVR_ENDADDR   0x08u
#define MPC_OVR_PERM      0x10u
#define MPC_OVR_PERMMASK  0x14u

#define MPC_EVENTS_MEMACCERR  0x100u
#define MPC_MEMACCERR_ADDRESS 0x400u

#define MPC_CONFIG_LOCK   (1u << 8)
#define MPC_CONFIG_ENABLE (1u << 9)
#define MPC_PERM_READ     (1u << 0)
#define MPC_PERM_WRITE    (1u << 1)
#define MPC_PERM_EXECUTE  (1u << 2)
#define MPC_PERM_SECATTR  (1u << 3)
/* RRAM and RAM out of reset: readable, writable, executable, Secure. */
#define MPC_PERM_RESET    (MPC_PERM_READ | MPC_PERM_WRITE | MPC_PERM_EXECUTE | MPC_PERM_SECATTR)

#define CRACEN_SIZE       0x1000u
#define CRACEN_ENABLE     0x400u
#define CRACEN_ENABLE_RNG (1u << 1)

#define CRACENCORE_SIZE   0x4000u
#define RNG_BASE          0x1000u
#define RNG_CONTROL       (RNG_BASE + 0x000u)
#define RNG_FIFOLEVEL     (RNG_BASE + 0x004u)
#define RNG_FIFOTHRESHOLD (RNG_BASE + 0x008u)
#define RNG_FIFODEPTH     (RNG_BASE + 0x00Cu)
#define RNG_STATUS        (RNG_BASE + 0x030u)
#define RNG_FIFO          (RNG_BASE + 0x080u)
#define RNG_FIFO_WORDS    16u

#define RNG_CONTROL_ENABLE  (1u << 0)
#define RNG_CONTROL_SOFTRST (1u << 8)
#define RNG_CONTROL_RESET   0x00040000u
#define RNG_STATUS_STATE_SHIFT 1u
#define RNG_STATUS_STATE_MASK  (0x7u << RNG_STATUS_STATE_SHIFT)
#define RNG_STATE_RESET   0u
#define RNG_STATE_IDLERON 2u

#define TAMPC_SIZE 0x1000u

#define ICACHE_BASE_S  0xE0082000u
#define ICACHE_SIZE    0x1000u
#define ICACHE_TASKS_INVALIDATECACHE 0x008u
#define ICACHE_TASKS_INVALIDATELINE  0x014u
#define ICACHE_TASKS_ERASE           0x020u
#define ICACHE_STATUS  0x400u
#define ICACHE_ENABLE  0x404u

#define CLOCK_EVENT_XOSTARTED_BIT    (1u << 0)
#define CLOCK_EVENT_PLLSTARTED_BIT   (1u << 1)
#define CLOCK_EVENT_LFCLKSTARTED_BIT (1u << 2)
#define CLOCK_EVENT_DONE_BIT         (1u << 3)
#define CLOCK_EVENT_XOTUNED_BIT      (1u << 4)
#define CLOCK_EVENT_XOTUNEERROR_BIT  (1u << 5)
#define CLOCK_EVENT_XOTUNEFAILED_BIT (1u << 6)
#define CLOCK_EVENT_XO24MSTARTED_BIT (1u << 7)

struct ficr_state {
    mm_u32 regs[FICR_SIZE / 4];
};

struct rramc_state {
    mm_u32 regs[RRAMC_SIZE / 4];
    mm_u8 *flash;
    mm_u32 flash_size;
    const struct mm_flash_persist *persist;
    struct mm_memmap *map;
    mm_u32 flags;
    mm_u32 buf_lines[RRAMC_WRITEBUF_MAX];
    mm_u8 buf_orig[RRAMC_WRITEBUF_MAX][RRAMC_LINE_SIZE];
    mm_u16 buf_valid[RRAMC_WRITEBUF_MAX];
    mm_u32 buf_count;
    mm_u32 buf_base;
};

struct clock_state {
    mm_u32 regs[CLOCK_SIZE / 4];
    mm_u32 events_mask;
    mm_u32 inten;
    mm_u32 lfclk_src;
    mm_bool xo_running;
    mm_bool pll_running;
    mm_bool lfclk_running;
    mm_bool xo_triggered;
    mm_bool pll_triggered;
    mm_bool lfclk_triggered;
    mm_bool pll24m_running;
};

struct osc_state {
    mm_u32 regs[OSCILLATORS_SIZE / 4];
};

struct grtc_state {
    mm_u32 regs[GRTC_SIZE / 4];
    mm_u64 syscounter;
    mm_u64 cc[GRTC_CC_COUNT];
    mm_u32 cc_active_mask;
    mm_u32 events_compare_mask;
    mm_u32 inten[GRTC_INT_GROUPS];
    mm_u64 accum_cycles;
    mm_bool running;
};

struct spu_state {
    mm_u32 base;
    mm_u32 regs[SPU_SIZE / 4];
};

struct mpc_state {
    mm_u32 regs[MPC_SIZE / 4];
    mm_u32 enabled_mask;
};

struct cracen_state {
    mm_u32 regs[CRACEN_SIZE / 4];
    mm_u32 core[CRACENCORE_SIZE / 4];
    mm_u32 rng_state;
    mm_u32 fifo_level;
};

struct reg_file {
    mm_u32 regs[0x1000u / 4];
};

static const mm_u32 g_gpio_bases_ns[NRF54L_GPIO_MAX] = {
    GPIO_P0_BASE_NS, GPIO_P1_BASE_NS, GPIO_P2_BASE_NS, GPIO_P3_BASE_NS
};

static const mm_u32 g_spu_bases[SPU_COUNT] = {
    0x50040000u, 0x50080000u, 0x500C0000u, 0x50100000u
};

static const struct nrf54l_mmio_variant *V = 0;

static struct nrf_gpio_bank gpio_banks[NRF54L_GPIO_MAX];
static struct ficr_state ficr_state;
static struct rramc_state rramc_state;
static struct clock_state clock_state;
static struct osc_state osc_state;
static struct grtc_state grtc_state;
static struct spu_state spu_state[SPU_COUNT];
static struct mpc_state mpc_state;
static struct cracen_state cracen_state;
static struct reg_file tampc_state;
static struct reg_file icache_state;
static struct mm_nvic *g_nrf54l_nvic = 0;
static struct nrf_timers_state g_timers;

static mm_u32 read_slice(mm_u32 reg, mm_u32 offset_in_reg, mm_u32 size_bytes)
{
    mm_u32 shift = offset_in_reg * 8u;
    mm_u32 mask = (size_bytes == 4u) ? 0xFFFFFFFFu : ((1u << (size_bytes * 8u)) - 1u);
    return (reg >> shift) & mask;
}

static mm_u32 apply_write(mm_u32 cur, mm_u32 offset_in_reg, mm_u32 size_bytes, mm_u32 value)
{
    mm_u32 shift = offset_in_reg * 8u;
    mm_u32 mask = (size_bytes == 4u) ? 0xFFFFFFFFu : ((1u << (size_bytes * 8u)) - 1u);
    mm_u32 shifted = (value & mask) << shift;
    return (cur & ~(mask << shift)) | shifted;
}

static void clock_update_irq(struct clock_state *clk)
{
    mm_u32 pending;
    if (clk == 0 || g_nrf54l_nvic == 0 || V == 0) return;
    pending = clk->events_mask & clk->inten;
    if (pending != 0u) {
        mm_nvic_set_pending(g_nrf54l_nvic, V->clock_irq, MM_TRUE);
    }
}

static void clock_set_event(struct clock_state *clk, mm_u32 offset, mm_u32 bit)
{
    if (clk == 0) return;
    clk->regs[offset / 4] = 1u;
    clk->events_mask |= bit;
    clock_update_irq(clk);
}

static mm_u32 clock_event_bit_for_offset(mm_u32 offset)
{
    switch (offset) {
    case CLOCK_EVENTS_XOSTARTED:
        return CLOCK_EVENT_XOSTARTED_BIT;
    case CLOCK_EVENTS_PLLSTARTED:
        return CLOCK_EVENT_PLLSTARTED_BIT;
    case CLOCK_EVENTS_LFCLKSTARTED:
        return CLOCK_EVENT_LFCLKSTARTED_BIT;
    case CLOCK_EVENTS_DONE:
        return CLOCK_EVENT_DONE_BIT;
    case CLOCK_EVENTS_XOTUNED:
        return CLOCK_EVENT_XOTUNED_BIT;
    case CLOCK_EVENTS_XOTUNEERROR:
        return CLOCK_EVENT_XOTUNEERROR_BIT;
    case CLOCK_EVENTS_XOTUNEFAILED:
        return CLOCK_EVENT_XOTUNEFAILED_BIT;
    case CLOCK_EVENTS_XO24MSTARTED:
        return CLOCK_EVENT_XO24MSTARTED_BIT;
    default:
        return 0u;
    }
}

static void grtc_update_irq(struct grtc_state *grtc)
{
    int group;
    if (grtc == 0 || g_nrf54l_nvic == 0) return;
    for (group = 0; group < (int)GRTC_INT_GROUPS; ++group) {
        mm_u32 pending = grtc->events_compare_mask & grtc->inten[group];
        if (pending != 0u) {
            mm_nvic_set_pending(g_nrf54l_nvic, (mm_u32)(GRTC_IRQ_BASE + group), MM_TRUE);
        }
    }
}

static mm_bool grtc_set_compare_event(struct grtc_state *grtc, mm_u32 ch)
{
    mm_u32 bit;
    if (grtc == 0 || ch >= GRTC_CC_COUNT) return MM_FALSE;
    bit = 1u << ch;
    if ((grtc->events_compare_mask & bit) != 0u) return MM_FALSE;
    grtc->events_compare_mask |= bit;
    grtc->regs[(GRTC_EVENTS_COMPARE0 + ch * 4u) / 4] = 1u;
    grtc_update_irq(grtc);
    return MM_TRUE;
}

static void grtc_sync_cc_regs(struct grtc_state *grtc, mm_u32 ch)
{
    mm_u64 value;
    mm_u32 base;
    if (grtc == 0 || ch >= GRTC_CC_COUNT) return;
    value = grtc->cc[ch];
    base = GRTC_CC_BASE + ch * GRTC_CC_STRIDE;
    grtc->regs[(base + GRTC_CC_CCL) / 4] = (mm_u32)(value & 0xFFFFFFFFu);
    grtc->regs[(base + GRTC_CC_CCH) / 4] = (mm_u32)((value >> 32) & 0xFFFFFFFFu);
}

static int gpio_port_count(void)
{
    if (V == 0 || V->gpio_count <= 0) return 0;
    if (V->gpio_count > NRF54L_GPIO_MAX) return NRF54L_GPIO_MAX;
    return V->gpio_count;
}

static mm_bool nrf54l_gpio_bank_info(void *opaque, int bank, char *name_out, size_t name_len, int *pins_out)
{
    (void)opaque;
    if (bank < 0 || bank >= gpio_port_count()) {
        return MM_FALSE;
    }
    if (name_out != 0 && name_len > 0u) {
        snprintf(name_out, name_len, "P%d", bank);
    }
    if (pins_out != 0) {
        *pins_out = 32;
    }
    return MM_TRUE;
}

static mm_bool nrf_gpio_set_external_input_cb(void *opaque, int bank, int pin, mm_bool level,
                                              mm_u64 late_cycles)
{
    (void)opaque;
    (void)late_cycles;
    if (bank < 0 || bank >= gpio_port_count()) return MM_FALSE;
    return nrf_gpio_set_external(&gpio_banks[bank], pin, level);
}

static struct spu_state *spu_for_secure_addr(mm_u32 addr);
static void spu_access_error(struct spu_state *spu, mm_u32 addr);
static mm_bool spu_feature_present(mm_u32 offset);
static mm_bool spu_perm_nonsecure(mm_u32 addr_ns);

/* FEATURE.GPIO[port] of this SPU holds the per-pin SECATTR of each port. */
static const mm_u32 g_gpio_spu_base[NRF54L_GPIO_MAX] = {
    0x50100000u, 0x500C0000u, 0x50040000u, 0x500C0000u
};

/* GPIO ports and the GRTC carry per-pin / per-feature security. */
static mm_bool nrf54l_split_periph(mm_u32 addr_ns)
{
    mm_u32 i;
    if (addr_ns - GRTC_BASE_NS < GRTC_SIZE) return MM_TRUE;
    for (i = 0; i < (mm_u32)gpio_port_count(); ++i) {
        if (addr_ns - g_gpio_bases_ns[i] < GPIO_SIZE) return MM_TRUE;
    }
    return MM_FALSE;
}

static mm_bool gpio_masked_reg(mm_u32 reg)
{
    return reg == GPIO_OUT || reg == GPIO_OUTSET || reg == GPIO_OUTCLR || reg == GPIO_IN ||
           reg == GPIO_DIR || reg == GPIO_DIRSET || reg == GPIO_DIRCLR || reg == GPIO_LATCH;
}

static mm_u32 gpio_ns_pins(const struct nrf_gpio_bank *g)
{
    mm_u32 port = (mm_u32)(g - gpio_banks);
    const struct spu_state *spu = spu_for_secure_addr(g_gpio_spu_base[port]);
    mm_u32 mask = 0u;
    mm_u32 pin;
    if (spu == 0) return 0xFFFFFFFFu;
    for (pin = 0; pin < 32u; ++pin) {
        if ((spu->regs[(SPU_FEATURE_GPIO + port * 0x80u + pin * 4u) / 4] & SPU_PERM_SECATTR) == 0u) {
            mask |= 1u << pin;
        }
    }
    return mask;
}

/* Port-wide GPIO registers follow the port's SPU PERIPH SECATTR. */
static mm_bool gpio_periph_ns(const struct nrf_gpio_bank *g, mm_u32 offset)
{
    mm_u32 addr = g_gpio_bases_ns[g - gpio_banks] + offset;
    if (spu_perm_nonsecure(addr)) return MM_TRUE;
    spu_access_error(spu_for_secure_addr(addr | PERIPH_S_OFFSET), addr);
    return MM_FALSE;
}

/* The non-secure alias of a split GPIO port reaches non-secure pins only. */
static mm_bool gpio_read_ns(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct nrf_gpio_bank *g = (struct nrf_gpio_bank *)opaque;
    mm_u32 ns;
    mm_u32 v = 0u;
    if (g == 0 || value_out == 0) return MM_FALSE;
    ns = gpio_ns_pins(g);
    if (offset >= GPIO_PIN_CNF0 && offset < (GPIO_PIN_CNF0 + 32u * 4u) &&
        (ns & (1u << ((offset - GPIO_PIN_CNF0) / 4u))) == 0u) {
        *value_out = 0u;
        return MM_TRUE;
    }
    if (!gpio_masked_reg(offset & ~3u) && offset < GPIO_PIN_CNF0 &&
        !gpio_periph_ns(g, offset)) {
        return MM_FALSE;
    }
    if (!nrf_gpio_read(opaque, offset, size_bytes, &v)) return MM_FALSE;
    if (gpio_masked_reg(offset & ~3u)) {
        v &= read_slice(ns, offset & 3u, size_bytes);
    }
    *value_out = v;
    return MM_TRUE;
}

static mm_bool gpio_write_ns(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct nrf_gpio_bank *g = (struct nrf_gpio_bank *)opaque;
    mm_u32 reg = offset & ~3u;
    mm_u32 ns;
    mm_u32 v;
    if (g == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > GPIO_SIZE) return MM_FALSE;
    ns = gpio_ns_pins(g);
    if (offset >= GPIO_PIN_CNF0 && offset < (GPIO_PIN_CNF0 + 32u * 4u)) {
        if ((ns & (1u << ((offset - GPIO_PIN_CNF0) / 4u))) == 0u) return MM_TRUE;
        return nrf_gpio_write(opaque, offset, size_bytes, value);
    }
    if (!gpio_masked_reg(reg)) {
        if (!gpio_periph_ns(g, offset)) return MM_FALSE;
        return nrf_gpio_write(opaque, offset, size_bytes, value);
    }
    v = apply_write(0u, offset & 3u, size_bytes, value) & ns;
    if (reg == GPIO_OUT || reg == GPIO_DIR) {
        mm_u32 cur = g->regs[reg / 4];
        mm_u32 keep = ~(apply_write(0u, offset & 3u, size_bytes, 0xFFFFFFFFu) & ns);
        v = (cur & keep) | v;
    }
    return nrf_gpio_write(opaque, reg, 4u, v);
}

static mm_u32 nrf_gpio_bank_read(void *opaque, int bank)
{
    (void)opaque;
    if (bank < 0 || bank >= gpio_port_count()) return 0;
    return gpio_banks[bank].regs[GPIO_OUT / 4];
}

static mm_u32 nrf_gpio_bank_read_moder(void *opaque, int bank)
{
    mm_u32 moder = 0;
    mm_u32 dir;
    int pin;
    (void)opaque;
    if (bank < 0 || bank >= gpio_port_count()) return 0;
    dir = gpio_banks[bank].regs[GPIO_DIR / 4];
    for (pin = 0; pin < 16; ++pin) {
        mm_u32 mode = ((dir >> pin) & 1u) ? 1u : 0u;
        moder |= (mode << (pin * 2));
    }
    return moder;
}

static mm_bool nrf_gpio_bank_clock(void *opaque, int bank)
{
    (void)opaque;
    (void)bank;
    return MM_TRUE;
}

static mm_u32 nrf_gpio_bank_read_seccfgr(void *opaque, int bank)
{
    (void)opaque;
    (void)bank;
    return 0u;
}

static mm_bool ficr_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct ficr_state *ficr = (struct ficr_state *)opaque;
    if (ficr == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > FICR_SIZE) return MM_FALSE;
    *value_out = read_slice(ficr->regs[offset / 4], offset & 3u, size_bytes);
    return MM_TRUE;
}

/* Factory information is read-only: stores are bus errors. */
static mm_bool ficr_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    (void)opaque;
    (void)offset;
    (void)size_bytes;
    (void)value;
    return MM_FALSE;
}

/* OVERRIDE[0..block-1] apply PERM, OVERRIDE[block] blocks access, and any
 * later slot is reserved: it reads as zero and ignores writes. */
static mm_u32 mpc_override_count(void)
{
    mm_u32 n;
    if (V == 0) return 0u;
    n = V->mpc_override_count;
    if (V->mpc_block_override < n) n = V->mpc_block_override + 1u;
    return (n > NRF54L_MPC_OVERRIDE_MAX) ? NRF54L_MPC_OVERRIDE_MAX : n;
}

static mm_u32 mpc_override_reg(mm_u32 n, mm_u32 reg)
{
    return mpc_state.regs[(MPC_OVERRIDE_BASE + n * MPC_OVERRIDE_STRIDE + reg) / 4];
}

static void mpc_refresh(void)
{
    mm_u32 n;
    mpc_state.enabled_mask = 0u;
    for (n = 0; n < mpc_override_count(); ++n) {
        if ((mpc_override_reg(n, MPC_OVR_CONFIG) & MPC_CONFIG_ENABLE) != 0u) {
            mpc_state.enabled_mask |= (1u << n);
        }
    }
}

/* Overlapping enabled OVERRIDE regions OR their PERM bits over their PERMMASK;
 * bits no matching region masks keep the reset permissions. */
static mm_u32 mpc_addr_perm(mm_u32 addr)
{
    mm_u32 mask = 0u;
    mm_u32 val = 0u;
    mm_u32 n;
    for (n = 0; n < mpc_override_count(); ++n) {
        mm_u32 m;
        if ((mpc_state.enabled_mask & (1u << n)) == 0u || n == V->mpc_block_override) continue;
        if (addr < mpc_override_reg(n, MPC_OVR_STARTADDR) ||
            addr >= mpc_override_reg(n, MPC_OVR_ENDADDR)) continue;
        m = mpc_override_reg(n, MPC_OVR_PERMMASK) & MPC_PERM_RESET;
        mask |= m;
        val |= mpc_override_reg(n, MPC_OVR_PERM) & m;
    }
    return (MPC_PERM_RESET & ~mask) | val;
}

static mm_bool mpc_blocked(mm_u32 addr)
{
    mm_u32 n = (V != 0) ? V->mpc_block_override : NRF54L_MPC_OVERRIDE_MAX;
    if (n >= mpc_override_count() || (mpc_state.enabled_mask & (1u << n)) == 0u) {
        return MM_FALSE;
    }
    return (addr >= mpc_override_reg(n, MPC_OVR_STARTADDR) &&
            addr < mpc_override_reg(n, MPC_OVR_ENDADDR)) ? MM_TRUE : MM_FALSE;
}

static mm_bool mpc_covers(mm_u32 addr)
{
    if (addr - NRF54L_FLASH_BASE < rramc_state.flash_size) return MM_TRUE;
    return (V != 0 && addr - NRF54L_RAM_BASE < V->ram_size) ? MM_TRUE : MM_FALSE;
}

/* MPC00 checks each RRAM/RAM transaction against the region permissions.
 * DMA and non-secure CPU data accesses carry `sec`; otherwise the SAU decides,
 * so an SG fetch from an NSC region stays Secure. */
static mm_bool mpc_check(const struct rramc_state *rramc,
                         enum mm_access_type type,
                         enum mm_sec_state sec,
                         mm_u32 addr,
                         mm_bool dma)
{
    enum mm_sec_state txn = sec;
    mm_u32 perm;
    mm_u32 need;

    if (!mpc_covers(addr)) return MM_TRUE;
    if (!dma && (sec == MM_SECURE || type == MM_ACCESS_EXEC) &&
        rramc->map != 0 && rramc->map->bus_attr != 0) {
        txn = rramc->map->bus_attr(rramc->map->bus_attr_opaque, addr);
    }
    perm = mpc_blocked(addr) ? 0u : mpc_addr_perm(addr);
    if (type == MM_ACCESS_WRITE) {
        need = MPC_PERM_WRITE;
    } else if (type == MM_ACCESS_EXEC) {
        need = MPC_PERM_EXECUTE;
    } else {
        need = MPC_PERM_READ;
    }
    if ((perm & need) != 0u &&
        (txn == MM_SECURE || (perm & MPC_PERM_SECATTR) == 0u)) {
        return MM_TRUE;
    }
    if (mpc_state.regs[MPC_EVENTS_MEMACCERR / 4] == 0u) {
        mpc_state.regs[MPC_MEMACCERR_ADDRESS / 4] = addr;
    }
    mpc_state.regs[MPC_EVENTS_MEMACCERR / 4] = 1u;
    return MM_FALSE;
}

static void rramc_note_read(struct rramc_state *rramc, mm_u32 addr);
static mm_bool spu_check(mm_u32 addr, enum mm_sec_state sec);

/* MM_TRUE when an enabled region starts or ends inside (addr, addr + size). */
static mm_bool mpc_splits(mm_u32 addr, mm_u32 size_bytes)
{
    mm_u32 n;
    for (n = 0; n < mpc_override_count(); ++n) {
        mm_u32 start;
        mm_u32 end;
        if ((mpc_state.enabled_mask & (1u << n)) == 0u) continue;
        start = mpc_override_reg(n, MPC_OVR_STARTADDR);
        end = mpc_override_reg(n, MPC_OVR_ENDADDR);
        if ((start - addr - 1u) < (size_bytes - 1u) || (end - addr - 1u) < (size_bytes - 1u)) {
            return MM_TRUE;
        }
    }
    return MM_FALSE;
}

/* Every byte must pass; only accesses split by a region edge need more than one check. */
static mm_bool mpc_filter(void *opaque,
                          enum mm_access_type type,
                          enum mm_sec_state sec,
                          mm_u32 addr,
                          mm_u32 size_bytes,
                          mm_bool dma)
{
    struct rramc_state *rramc = (struct rramc_state *)opaque;
    mm_u32 i;
    if (rramc == 0) return MM_TRUE;
    if (!spu_check(addr, sec)) return MM_FALSE;
    /* EasyDMA only reaches RAM. */
    if (dma && (V == 0 || addr - NRF54L_RAM_BASE >= V->ram_size ||
                size_bytes > V->ram_size - (addr - NRF54L_RAM_BASE))) {
        return MM_FALSE;
    }
    if (!mpc_check(rramc, type, sec, addr, dma)) return MM_FALSE;
    if (size_bytes > 1u && mpc_splits(addr, size_bytes)) {
        for (i = 1u; i < size_bytes; ++i) {
            if (!mpc_check(rramc, type, sec, addr + i, dma)) return MM_FALSE;
        }
    }
    for (i = 0u; i < size_bytes && rramc->buf_count != 0u && type != MM_ACCESS_WRITE; ++i) {
        rramc_note_read(rramc, addr + i);
    }
    return MM_TRUE;
}

static mm_bool mpc_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct mpc_state *mpc = (struct mpc_state *)opaque;
    if (mpc == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > MPC_SIZE) return MM_FALSE;
    *value_out = read_slice(mpc->regs[offset / 4], offset & 3u, size_bytes);
    return MM_TRUE;
}

static mm_bool mpc_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct mpc_state *mpc = (struct mpc_state *)opaque;
    mm_u32 n;
    if (mpc == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > MPC_SIZE) return MM_FALSE;

    if (offset >= MPC_OVERRIDE_BASE) {
        n = (offset - MPC_OVERRIDE_BASE) / MPC_OVERRIDE_STRIDE;
        if (n >= mpc_override_count()) {
            return MM_TRUE;
        }
        if ((mpc_override_reg(n, MPC_OVR_CONFIG) & MPC_CONFIG_LOCK) != 0u) {
            return MM_TRUE;
        }
        mpc->regs[offset / 4] = apply_write(mpc->regs[offset / 4], offset & 3u, size_bytes, value);
        mpc_refresh();
        /* Translated code was checked under the old permissions. */
        if (rramc_state.map != 0 && rramc_state.map->code_cache != 0) {
            mm_code_cache_note_write(rramc_state.map->code_cache, NRF54L_FLASH_BASE,
                                     rramc_state.flash_size);
            if (V != 0) {
                mm_code_cache_note_write(rramc_state.map->code_cache, NRF54L_RAM_BASE,
                                         V->ram_size);
            }
        }
        return MM_TRUE;
    }

    mpc->regs[offset / 4] = apply_write(mpc->regs[offset / 4], offset & 3u, size_bytes, value);
    return MM_TRUE;
}

static mm_bool spu_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct spu_state *spu = (struct spu_state *)opaque;
    if (spu == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > SPU_SIZE) return MM_FALSE;
    *value_out = read_slice(spu->regs[offset / 4], offset & 3u, size_bytes);
    return MM_TRUE;
}

static mm_bool spu_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct spu_state *spu = (struct spu_state *)opaque;
    mm_u32 cur;
    if (spu == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > SPU_SIZE) return MM_FALSE;

    cur = spu->regs[offset / 4];
    if (offset >= SPU_PERIPH_PERM && offset < (SPU_PERIPH_PERM + SPU_PERIPH_COUNT * 4u)) {
        if ((cur & SPU_PERM_LOCK) != 0u || size_bytes != 4) {
            return MM_TRUE;
        }
        spu->regs[offset / 4] = (cur & ~SPU_PERM_RW_MASK) | (value & SPU_PERM_RW_MASK);
        return MM_TRUE;
    }
    if (offset >= SPU_FEATURE_BASE && offset < SPU_FEATURE_END) {
        if (!spu_feature_present(offset & ~3u) || (cur & SPU_FEATURE_LOCK) != 0u) {
            return MM_TRUE;
        }
        /* Only SECATTR and LOCK are writable; the rest is fixed capability. */
        value = apply_write(cur, offset & 3u, size_bytes, value);
        spu->regs[offset / 4] = (cur & ~(SPU_PERM_SECATTR | SPU_FEATURE_LOCK)) |
                                (value & (SPU_PERM_SECATTR | SPU_FEATURE_LOCK));
        return MM_TRUE;
    }
    spu->regs[offset / 4] = apply_write(cur, offset & 3u, size_bytes, value);
    return MM_TRUE;
}

static mm_bool spu_feature_present(mm_u32 offset)
{
    mm_u32 i;
    if (V == 0 || V->spu_features == 0) return MM_FALSE;
    for (i = 0; i < V->spu_feature_count; ++i) {
        const struct nrf54l_spu_feature *f = &V->spu_features[i];
        if (offset >= f->offset && offset - f->offset < f->count * 4u) return MM_TRUE;
    }
    return MM_FALSE;
}

static void spu_reset_one(struct spu_state *spu, mm_u32 base)
{
    mm_u32 i;
    memset(spu, 0, sizeof(*spu));
    spu->base = base;
    for (i = 0; i < SPU_PERIPH_COUNT; ++i) {
        spu->regs[(SPU_PERIPH_PERM / 4) + i] = SPU_PERM_RESET;
    }
    for (i = SPU_FEATURE_BASE; i < SPU_FEATURE_END; i += 4u) {
        if (spu_feature_present(i)) {
            spu->regs[i / 4] = SPU_FEATURE_RESET;
        }
    }
}

static struct spu_state *spu_for_secure_addr(mm_u32 addr)
{
    mm_u32 base = PERIPH_BASE_S | (addr & 0x00FC0000u);
    mm_u32 i;
    for (i = 0; i < SPU_COUNT; ++i) {
        if (spu_state[i].base == base) {
            return &spu_state[i];
        }
    }
    return 0;
}

static void spu_access_error(struct spu_state *spu, mm_u32 addr)
{
    if (spu == 0) return;
    if (spu->regs[SPU_EVENTS_PERIPHACCERR / 4] == 0u) {
        spu->regs[SPU_PERIPHACCERR_ADDRESS / 4] = addr;
    }
    spu->regs[SPU_EVENTS_PERIPHACCERR / 4] = 1u;
}

static mm_bool spu_perm_nonsecure(mm_u32 addr_ns)
{
    const struct spu_state *spu = spu_for_secure_addr(addr_ns | PERIPH_S_OFFSET);
    if (spu == 0) return MM_TRUE;
    return (spu->regs[(SPU_PERIPH_PERM / 4) + ((addr_ns >> 12) & (SPU_PERIPH_COUNT - 1u))] &
            SPU_PERM_SECATTR) == 0u ? MM_TRUE : MM_FALSE;
}

/* A peripheral answers only at the alias its SPU SECATTR selects, and its
 * secure alias only to secure transactions; anything else is PERIPHACCERR. */
static mm_bool spu_check(mm_u32 addr, enum mm_sec_state sec)
{
    struct spu_state *spu;
    mm_u32 perm;
    mm_u32 i;
    mm_bool secure_alias;

    if (addr < PERIPH_BASE_NS || addr - PERIPH_BASE_NS >= 2u * PERIPH_S_OFFSET) {
        return MM_TRUE;
    }
    spu = spu_for_secure_addr(addr | PERIPH_S_OFFSET);
    if (spu == 0) return MM_TRUE;
    secure_alias = (addr >= PERIPH_BASE_S) ? MM_TRUE : MM_FALSE;
    /* Split peripherals answer at both aliases; their NS register callbacks
     * enforce the per-pin and per-feature attributes. */
    if (nrf54l_split_periph(addr & ~PERIPH_S_OFFSET)) {
        if (!secure_alias || sec == MM_SECURE) return MM_TRUE;
        (void)i;
    }
    perm = spu->regs[(SPU_PERIPH_PERM / 4) + ((addr >> 12) & (SPU_PERIPH_COUNT - 1u))];
    if ((perm & SPU_PERM_SECATTR) != 0u) {
        if (secure_alias && sec == MM_SECURE) return MM_TRUE;
    } else if (!secure_alias) {
        return MM_TRUE;
    }
    spu_access_error(spu, addr);
    return MM_FALSE;
}

static void rramc_persist(struct rramc_state *rramc, mm_u32 offset, mm_u32 size)
{
    if (rramc->persist == 0 || !rramc->persist->enabled) return;
    if (offset >= rramc->flash_size) return;
    if (size > rramc->flash_size - offset) {
        size = rramc->flash_size - offset;
    }
    mm_flash_persist_flush((struct mm_flash_persist *)rramc->persist, offset, size);
}

static void rramc_commit(struct rramc_state *rramc)
{
    mm_u32 i;
    for (i = 0; i < rramc->buf_count; ++i) {
        rramc_persist(rramc, rramc->buf_lines[i], RRAMC_LINE_SIZE);
    }
    if (rramc->buf_count != 0u) {
        rramc->regs[RRAMC_EVENTS_READY / 4] = 1u;
    }
    rramc->buf_count = 0u;
}

static mm_u32 rramc_writebuf_size(const struct rramc_state *rramc)
{
    mm_u32 size = (rramc->regs[RRAMC_CONFIG / 4] >> RRAMC_CONFIG_WRITEBUFSIZE_SHIFT) &
                  RRAMC_CONFIG_WRITEBUFSIZE_MASK;
    return (size > RRAMC_WRITEBUF_MAX) ? RRAMC_WRITEBUF_MAX : size;
}

/* The buffer covers WRITEBUFSIZE lines from the first one written. It commits
 * once every byte of them is written, before a store outside that area, and
 * when a buffered line is read back (rramc_note_read). */
static void rramc_buffer_byte(struct rramc_state *rramc, mm_u32 off)
{
    mm_u32 size = rramc_writebuf_size(rramc);
    mm_u32 line = off & ~(RRAMC_LINE_SIZE - 1u);
    mm_u32 i;
    if (rramc->buf_count != 0u &&
        (line < rramc->buf_base || line - rramc->buf_base >= size * RRAMC_LINE_SIZE)) {
        rramc_commit(rramc);
    }
    if (rramc->buf_count == 0u) {
        rramc->buf_base = line;
    }
    for (i = 0; i < rramc->buf_count; ++i) {
        if (rramc->buf_lines[i] == line) break;
    }
    if (i == rramc->buf_count) {
        rramc->buf_lines[i] = line;
        memcpy(rramc->buf_orig[i], rramc->flash + line, RRAMC_LINE_SIZE);
        rramc->buf_valid[i] = 0u;
        rramc->buf_count++;
    }
    rramc->buf_valid[i] |= (mm_u16)(1u << (off - line));
}

static mm_bool rramc_buffer_full(const struct rramc_state *rramc)
{
    mm_u32 i;
    if (rramc->buf_count < rramc_writebuf_size(rramc)) return MM_FALSE;
    for (i = 0; i < rramc->buf_count; ++i) {
        if (rramc->buf_valid[i] != 0xFFFFu) return MM_FALSE;
    }
    return MM_TRUE;
}

/* A reset loses what was never committed. */
static void rramc_discard(struct rramc_state *rramc)
{
    mm_u32 i;
    for (i = 0; i < rramc->buf_count; ++i) {
        memcpy(rramc->flash + rramc->buf_lines[i], rramc->buf_orig[i], RRAMC_LINE_SIZE);
    }
    rramc->buf_count = 0u;
}

static void rramc_note_read(struct rramc_state *rramc, mm_u32 addr)
{
    mm_u32 line = (addr - NRF54L_FLASH_BASE) & ~(RRAMC_LINE_SIZE - 1u);
    mm_u32 i;
    for (i = 0; i < rramc->buf_count; ++i) {
        if (rramc->buf_lines[i] == line) {
            rramc_commit(rramc);
            return;
        }
    }
}

/* RRAM is overwritten in place, there is no erase: storing 0xFFFFFFFF is how
 * a word is erased. Stores without CONFIG.WEN are an access error. */
static mm_bool rramc_write_cb(void *opaque,
                              enum mm_sec_state sec,
                              mm_u32 addr,
                              mm_u32 size_bytes,
                              mm_u32 value)
{
    struct rramc_state *rramc = (struct rramc_state *)opaque;
    mm_u32 offset;
    mm_u32 i;

    if (rramc == 0 || rramc->flash == 0) return MM_FALSE;
    if (size_bytes != 1u && size_bytes != 2u && size_bytes != 4u) return MM_FALSE;
    offset = addr - NRF54L_FLASH_BASE;
    if (offset >= rramc->flash_size || size_bytes > rramc->flash_size - offset) return MM_FALSE;

    (void)sec;
    if ((rramc->regs[RRAMC_CONFIG / 4] & RRAMC_CONFIG_WEN) == 0u) {
        if (rramc->regs[RRAMC_EVENTS_ACCESSERROR / 4] == 0u) {
            rramc->regs[RRAMC_ACCESSERRORADDR / 4] = addr;
        }
        rramc->regs[RRAMC_EVENTS_ACCESSERROR / 4] = 1u;
        return MM_FALSE;
    }

    for (i = 0; i < size_bytes; ++i) {
        if (rramc_writebuf_size(rramc) != 0u) {
            rramc_buffer_byte(rramc, offset + i);
        }
        rramc->flash[offset + i] = (mm_u8)((value >> (i * 8u)) & 0xFFu);
    }

    if (rramc_writebuf_size(rramc) == 0u) {
        rramc_persist(rramc, offset, size_bytes);
    } else if (rramc_buffer_full(rramc)) {
        rramc_commit(rramc);
    }
    return MM_TRUE;
}

static void rramc_erase_all(struct rramc_state *rramc)
{
    if (rramc == 0 || rramc->flash == 0) return;
    memset(rramc->flash, 0xFF, rramc->flash_size);
    rramc->buf_count = 0u;
    if (rramc->map != 0 && rramc->map->code_cache != 0) {
        mm_code_cache_note_write(rramc->map->code_cache, NRF54L_FLASH_BASE, rramc->flash_size);
    }
    rramc_persist(rramc, 0u, rramc->flash_size);
}

static mm_bool rramc_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct rramc_state *rramc = (struct rramc_state *)opaque;
    if (rramc == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > RRAMC_SIZE) return MM_FALSE;

    if (offset == RRAMC_READY && size_bytes == 4) {
        *value_out = 1u;
        return MM_TRUE;
    }
    if (offset == RRAMC_READYNEXT && size_bytes == 4) {
        *value_out = 1u;
        return MM_TRUE;
    }
    if (offset == RRAMC_BUFSTATUS_WRITEBUFEMPTY && size_bytes == 4) {
        *value_out = (rramc->buf_count == 0u) ? 1u : 0u;
        return MM_TRUE;
    }

    *value_out = read_slice(rramc->regs[offset / 4], offset & 3u, size_bytes);
    return MM_TRUE;
}

static mm_bool rramc_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct rramc_state *rramc = (struct rramc_state *)opaque;
    if (rramc == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > RRAMC_SIZE) return MM_FALSE;

    if (offset == RRAMC_TASKS_COMMITWRITEBUF && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            rramc_commit(rramc);
        }
        return MM_TRUE;
    }
    if (offset == RRAMC_ERASEALL && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            rramc_erase_all(rramc);
        }
        return MM_TRUE;
    }
    if (offset == RRAMC_CONFIG && size_bytes == 4) {
        rramc->regs[RRAMC_CONFIG / 4] = value & RRAMC_CONFIG_MASK;
        if (rramc->buf_count > rramc_writebuf_size(rramc)) {
            rramc_commit(rramc);
        }
        return MM_TRUE;
    }
    if (offset == RRAMC_READY || offset == RRAMC_READYNEXT ||
        offset == RRAMC_ACCESSERRORADDR || offset == RRAMC_BUFSTATUS_WRITEBUFEMPTY) {
        return MM_TRUE;
    }

    rramc->regs[offset / 4] = apply_write(rramc->regs[offset / 4], offset & 3u, size_bytes, value);
    return MM_TRUE;
}

static mm_bool clock_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct clock_state *clk = (struct clock_state *)opaque;
    mm_u32 reg = 0;

    if (clk == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > CLOCK_SIZE) return MM_FALSE;

    if (offset == CLOCK_INTEN || offset == CLOCK_INTENSET || offset == CLOCK_INTENCLR) {
        reg = clk->inten;
        *value_out = read_slice(reg, offset & 3u, size_bytes);
        return MM_TRUE;
    }
    if (offset == CLOCK_INTPEND) {
        reg = clk->events_mask & clk->inten;
        *value_out = read_slice(reg, offset & 3u, size_bytes);
        return MM_TRUE;
    }
    if (offset == CLOCK_XO_RUN) {
        reg = clk->xo_triggered ? 1u : 0u;
        *value_out = read_slice(reg, offset & 3u, size_bytes);
        return MM_TRUE;
    }
    if (offset == CLOCK_XO_STAT) {
        reg = clk->xo_running ? (1u << 16) : 0u;
        *value_out = read_slice(reg, offset & 3u, size_bytes);
        return MM_TRUE;
    }
    if (offset == CLOCK_PLL_RUN) {
        reg = clk->pll_triggered ? 1u : 0u;
        *value_out = read_slice(reg, offset & 3u, size_bytes);
        return MM_TRUE;
    }
    if (offset == CLOCK_PLL_STAT) {
        reg = clk->pll_running ? (1u << 16) : 0u;
        *value_out = read_slice(reg, offset & 3u, size_bytes);
        return MM_TRUE;
    }
    if (offset == CLOCK_LFCLK_SRC) {
        reg = clk->lfclk_src & 0x3u;
        *value_out = read_slice(reg, offset & 3u, size_bytes);
        return MM_TRUE;
    }
    if (offset == CLOCK_LFCLK_RUN) {
        reg = clk->lfclk_triggered ? 1u : 0u;
        *value_out = read_slice(reg, offset & 3u, size_bytes);
        return MM_TRUE;
    }
    if (offset == CLOCK_LFCLK_STAT) {
        reg = (clk->lfclk_src & 0x3u) | (clk->lfclk_running ? (1u << 16) : 0u);
        *value_out = read_slice(reg, offset & 3u, size_bytes);
        return MM_TRUE;
    }
    if (offset == CLOCK_PLL24M_STAT) {
        reg = clk->pll24m_running ? (1u << 16) : 0u;
        *value_out = read_slice(reg, offset & 3u, size_bytes);
        return MM_TRUE;
    }

    reg = clk->regs[offset / 4];
    *value_out = read_slice(reg, offset & 3u, size_bytes);
    return MM_TRUE;
}

static mm_bool clock_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct clock_state *clk = (struct clock_state *)opaque;
    mm_u32 bit;

    if (clk == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > CLOCK_SIZE) return MM_FALSE;

    if (offset == CLOCK_TASKS_XOSTART && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            clk->xo_triggered = MM_TRUE;
            clk->xo_running = MM_TRUE;
            clock_set_event(clk, CLOCK_EVENTS_XOSTARTED, CLOCK_EVENT_XOSTARTED_BIT);
            clock_set_event(clk, CLOCK_EVENTS_XOTUNED, CLOCK_EVENT_XOTUNED_BIT);
        }
        return MM_TRUE;
    }
    if (offset == CLOCK_TASKS_XOSTOP && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            clk->xo_running = MM_FALSE;
        }
        return MM_TRUE;
    }
    if (offset == CLOCK_TASKS_PLLSTART && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            clk->pll_triggered = MM_TRUE;
            clk->pll_running = MM_TRUE;
            clock_set_event(clk, CLOCK_EVENTS_PLLSTARTED, CLOCK_EVENT_PLLSTARTED_BIT);
        }
        return MM_TRUE;
    }
    if (offset == CLOCK_TASKS_PLLSTOP && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            clk->pll_running = MM_FALSE;
        }
        return MM_TRUE;
    }
    if (offset == CLOCK_TASKS_LFCLKSTART && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            clk->lfclk_triggered = MM_TRUE;
            clk->lfclk_src = clk->regs[CLOCK_LFCLK_SRC / 4] & 0x3u;
            clk->regs[CLOCK_LFCLK_SRCCOPY / 4] = clk->lfclk_src;
            clk->lfclk_running = MM_TRUE;
            clock_set_event(clk, CLOCK_EVENTS_LFCLKSTARTED, CLOCK_EVENT_LFCLKSTARTED_BIT);
        }
        return MM_TRUE;
    }
    if (offset == CLOCK_TASKS_LFCLKSTOP && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            clk->lfclk_running = MM_FALSE;
        }
        return MM_TRUE;
    }
    if (offset == CLOCK_TASKS_CAL && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            clock_set_event(clk, CLOCK_EVENTS_DONE, CLOCK_EVENT_DONE_BIT);
        }
        return MM_TRUE;
    }
    if (offset == CLOCK_TASKS_XOTUNE && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            clock_set_event(clk, CLOCK_EVENTS_XOTUNED, CLOCK_EVENT_XOTUNED_BIT);
        }
        return MM_TRUE;
    }
    if (offset == CLOCK_TASKS_XOTUNEABORT && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            clock_set_event(clk, CLOCK_EVENTS_XOTUNEFAILED, CLOCK_EVENT_XOTUNEFAILED_BIT);
        }
        return MM_TRUE;
    }
    if (offset == CLOCK_TASKS_XO24MSTART && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            clk->pll24m_running = MM_TRUE;
            clock_set_event(clk, CLOCK_EVENTS_XO24MSTARTED, CLOCK_EVENT_XO24MSTARTED_BIT);
        }
        return MM_TRUE;
    }
    if (offset == CLOCK_TASKS_XO24MSTOP && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            clk->pll24m_running = MM_FALSE;
        }
        return MM_TRUE;
    }

    bit = clock_event_bit_for_offset(offset);
    if (bit != 0u && size_bytes == 4) {
        if (value == 0u) {
            clk->events_mask &= ~bit;
            clk->regs[offset / 4] = 0u;
        } else {
            clk->events_mask |= bit;
            clk->regs[offset / 4] = value;
        }
        clock_update_irq(clk);
        return MM_TRUE;
    }

    if (offset == CLOCK_INTEN && size_bytes == 4) {
        clk->inten = value;
        clock_update_irq(clk);
        return MM_TRUE;
    }
    if (offset == CLOCK_INTENSET && size_bytes == 4) {
        clk->inten |= value;
        clock_update_irq(clk);
        return MM_TRUE;
    }
    if (offset == CLOCK_INTENCLR && size_bytes == 4) {
        clk->inten &= ~value;
        return MM_TRUE;
    }
    if (offset == CLOCK_LFCLK_SRC && size_bytes == 4) {
        clk->lfclk_src = value & 0x3u;
        clk->regs[offset / 4] = clk->lfclk_src;
        return MM_TRUE;
    }

    clk->regs[offset / 4] = apply_write(clk->regs[offset / 4], offset & 3u, size_bytes, value);
    return MM_TRUE;
}

static mm_bool oscillators_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct osc_state *osc = (struct osc_state *)opaque;
    if (osc == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > OSCILLATORS_SIZE) return MM_FALSE;
    *value_out = read_slice(osc->regs[offset / 4], offset & 3u, size_bytes);
    return MM_TRUE;
}

static mm_bool oscillators_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct osc_state *osc = (struct osc_state *)opaque;
    mm_u32 v;
    if (osc == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > OSCILLATORS_SIZE) return MM_FALSE;

    if (offset == OSC_PLL_FREQ && size_bytes == 4) {
        v = value & 0x3u;
        osc->regs[OSC_PLL_FREQ / 4] = v;
        osc->regs[OSC_PLL_CURRENTFREQ / 4] = v;
        return MM_TRUE;
    }
    if (offset == OSC_PLL_CURRENTFREQ) {
        return MM_TRUE;
    }

    osc->regs[offset / 4] = apply_write(osc->regs[offset / 4], offset & 3u, size_bytes, value);
    return MM_TRUE;
}

static mm_bool grtc_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct grtc_state *grtc = (struct grtc_state *)opaque;
    mm_u32 reg = 0;
    mm_u32 rel;
    mm_u32 idx;
    mm_u32 sub;
    mm_u32 ch;

    if (grtc == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > GRTC_SIZE) return MM_FALSE;

    if (offset >= GRTC_INTEN0 && offset < (GRTC_INTEN0 + GRTC_INT_GROUP_STRIDE * GRTC_INT_GROUPS)) {
        idx = (offset - GRTC_INTEN0) / GRTC_INT_GROUP_STRIDE;
        sub = (offset - GRTC_INTEN0) % GRTC_INT_GROUP_STRIDE;
        if (sub == 0u || sub == 4u || sub == 8u) {
            reg = grtc->inten[idx];
        } else if (sub == 0x0Cu) {
            reg = grtc->events_compare_mask & grtc->inten[idx];
        } else {
            reg = grtc->regs[offset / 4];
        }
        *value_out = read_slice(reg, offset & 3u, size_bytes);
        return MM_TRUE;
    }

    if (offset >= GRTC_EVENTS_COMPARE0 && offset < (GRTC_EVENTS_COMPARE0 + GRTC_CC_COUNT * 4u)) {
        ch = (offset - GRTC_EVENTS_COMPARE0) / 4u;
        if (grtc->running) {
            mm_u32 active = (grtc->cc_active_mask >> ch) & 1u;
            mm_u32 set = (grtc->events_compare_mask >> ch) & 1u;
            if (active && !set) {
                if (grtc->syscounter < grtc->cc[ch]) {
                    grtc->syscounter = grtc->cc[ch];
                }
                (void)grtc_set_compare_event(grtc, ch);
            }
        }
        reg = (grtc->events_compare_mask >> ch) & 1u;
        *value_out = read_slice(reg, offset & 3u, size_bytes);
        return MM_TRUE;
    }

    if (offset >= GRTC_CC_BASE && offset < (GRTC_CC_BASE + GRTC_CC_COUNT * GRTC_CC_STRIDE)) {
        rel = offset - GRTC_CC_BASE;
        ch = rel / GRTC_CC_STRIDE;
        sub = rel % GRTC_CC_STRIDE;
        if (sub == GRTC_CC_CCL) {
            reg = (mm_u32)(grtc->cc[ch] & 0xFFFFFFFFu);
        } else if (sub == GRTC_CC_CCH) {
            reg = (mm_u32)((grtc->cc[ch] >> 32) & 0xFFFFFFFFu);
        } else if (sub == GRTC_CC_CCEN) {
            mm_u32 active = (grtc->cc_active_mask >> ch) & 1u;
            mm_u32 past = (active && grtc->syscounter >= grtc->cc[ch]) ? 1u : 0u;
            reg = active | (past << 1);
        } else {
            reg = 0u;
        }
        *value_out = read_slice(reg, offset & 3u, size_bytes);
        return MM_TRUE;
    }

    if (offset >= GRTC_SYSCOUNTER_BASE &&
        offset < (GRTC_SYSCOUNTER_BASE + GRTC_SYSCOUNTER_COUNT * GRTC_SYSCOUNTER_STRIDE)) {
        rel = offset - GRTC_SYSCOUNTER_BASE;
        sub = rel % GRTC_SYSCOUNTER_STRIDE;
        if (sub == GRTC_SYSCOUNTERL) {
            if (grtc->running) {
                grtc->syscounter += 1000u;
            }
            reg = (mm_u32)(grtc->syscounter & 0xFFFFFFFFu);
        } else if (sub == GRTC_SYSCOUNTERH) {
            reg = (mm_u32)((grtc->syscounter >> 32) & 0x000FFFFFu);
            reg |= (1u << 29);
        } else if (sub == GRTC_SYSCOUNTER_ACTIVE) {
            reg = grtc->regs[offset / 4] & 1u;
        } else {
            reg = 0u;
        }
        *value_out = read_slice(reg, offset & 3u, size_bytes);
        return MM_TRUE;
    }

    if (offset == GRTC_STATUS_LFTIMER || offset == GRTC_STATUS_PWM || offset == GRTC_STATUS_CLKOUT) {
        reg = 1u;
        *value_out = read_slice(reg, offset & 3u, size_bytes);
        return MM_TRUE;
    }

    reg = grtc->regs[offset / 4];
    *value_out = read_slice(reg, offset & 3u, size_bytes);
    return MM_TRUE;
}

static mm_bool grtc_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct grtc_state *grtc = (struct grtc_state *)opaque;
    mm_u32 rel;
    mm_u32 idx;
    mm_u32 sub;
    mm_u32 ch;
    mm_u64 base;
    mm_u64 add;

    if (grtc == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > GRTC_SIZE) return MM_FALSE;

    if (offset < (GRTC_TASKS_CAPTURE0 + GRTC_CC_COUNT * 4u)) {
        ch = (offset - GRTC_TASKS_CAPTURE0) / 4u;
        if (size_bytes == 4 && (value & 1u) != 0u) {
            grtc->cc[ch] = grtc->syscounter;
            grtc_sync_cc_regs(grtc, ch);
        }
        return MM_TRUE;
    }
    if (offset == GRTC_TASKS_START && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            grtc->running = MM_TRUE;
            grtc->regs[GRTC_MODE / 4] |= (1u << 1);
        }
        return MM_TRUE;
    }
    if (offset == GRTC_TASKS_STOP && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            grtc->running = MM_FALSE;
            grtc->regs[GRTC_MODE / 4] &= ~(1u << 1);
        }
        return MM_TRUE;
    }
    if (offset == GRTC_TASKS_CLEAR && size_bytes == 4) {
        if ((value & 1u) != 0u) {
            grtc->syscounter = 0u;
        }
        return MM_TRUE;
    }

    if (offset >= GRTC_EVENTS_COMPARE0 && offset < (GRTC_EVENTS_COMPARE0 + GRTC_CC_COUNT * 4u)) {
        ch = (offset - GRTC_EVENTS_COMPARE0) / 4u;
        if (size_bytes == 4) {
            if (value == 0u) {
                grtc->events_compare_mask &= ~(1u << ch);
                grtc->regs[offset / 4] = 0u;
            } else {
                grtc->events_compare_mask |= (1u << ch);
                grtc->regs[offset / 4] = value;
            }
            grtc_update_irq(grtc);
        }
        return MM_TRUE;
    }

    if (offset >= GRTC_INTEN0 && offset < (GRTC_INTEN0 + GRTC_INT_GROUP_STRIDE * GRTC_INT_GROUPS)) {
        idx = (offset - GRTC_INTEN0) / GRTC_INT_GROUP_STRIDE;
        sub = (offset - GRTC_INTEN0) % GRTC_INT_GROUP_STRIDE;
        if (size_bytes == 4) {
            if (sub == 0u) {
                grtc->inten[idx] = value;
            } else if (sub == 4u) {
                grtc->inten[idx] |= value;
            } else if (sub == 8u) {
                grtc->inten[idx] &= ~value;
            }
            grtc_update_irq(grtc);
        }
        return MM_TRUE;
    }

    if (offset == GRTC_MODE && size_bytes == 4) {
        grtc->regs[GRTC_MODE / 4] = value;
        grtc->running = ((value & (1u << 1)) != 0u) ? MM_TRUE : MM_FALSE;
        return MM_TRUE;
    }

    if (offset >= GRTC_CC_BASE && offset < (GRTC_CC_BASE + GRTC_CC_COUNT * GRTC_CC_STRIDE)) {
        rel = offset - GRTC_CC_BASE;
        ch = rel / GRTC_CC_STRIDE;
        sub = rel % GRTC_CC_STRIDE;
        if (size_bytes == 4) {
            if (sub == GRTC_CC_CCL) {
                grtc->cc[ch] = (grtc->cc[ch] & 0xFFFFFFFF00000000ull) | value;
                grtc_sync_cc_regs(grtc, ch);
            } else if (sub == GRTC_CC_CCH) {
                grtc->cc[ch] = (grtc->cc[ch] & 0x00000000FFFFFFFFull) | ((mm_u64)value << 32);
                grtc_sync_cc_regs(grtc, ch);
            } else if (sub == GRTC_CC_CCADD) {
                add = (mm_u64)(value & 0x7FFFFFFFu);
                base = (value & 0x80000000u) ? grtc->cc[ch] : grtc->syscounter;
                grtc->cc[ch] = base + add;
                grtc_sync_cc_regs(grtc, ch);
            } else if (sub == GRTC_CC_CCEN) {
                if ((value & 1u) != 0u) {
                    grtc->cc_active_mask |= (1u << ch);
                } else {
                    grtc->cc_active_mask &= ~(1u << ch);
                }
                grtc->regs[offset / 4] = value & 1u;
            }
        }
        return MM_TRUE;
    }

    if (offset >= GRTC_SYSCOUNTER_BASE &&
        offset < (GRTC_SYSCOUNTER_BASE + GRTC_SYSCOUNTER_COUNT * GRTC_SYSCOUNTER_STRIDE)) {
        rel = offset - GRTC_SYSCOUNTER_BASE;
        sub = rel % GRTC_SYSCOUNTER_STRIDE;
        if (sub == GRTC_SYSCOUNTER_ACTIVE && size_bytes == 4) {
            grtc->regs[offset / 4] = value & 1u;
            return MM_TRUE;
        }
    }

    if (offset == GRTC_INTERVAL && size_bytes == 4) {
        grtc->regs[offset / 4] = value;
        return MM_TRUE;
    }

    grtc->regs[offset / 4] = apply_write(grtc->regs[offset / 4], offset & 3u, size_bytes, value);
    return MM_TRUE;
}

static void cracen_rng_update(struct cracen_state *c)
{
    mm_u32 control = c->core[RNG_CONTROL / 4];
    if ((c->regs[CRACEN_ENABLE / 4] & CRACEN_ENABLE_RNG) == 0u ||
        (control & RNG_CONTROL_SOFTRST) != 0u ||
        (control & RNG_CONTROL_ENABLE) == 0u) {
        c->rng_state = RNG_STATE_RESET;
        c->fifo_level = 0u;
        return;
    }
    /* Start-up and conditioning finish at once; the FIFO then refills
     * as fast as it is drained. */
    c->rng_state = RNG_STATE_IDLERON;
}

static mm_bool cracen_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct cracen_state *c = (struct cracen_state *)opaque;
    if (c == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > CRACEN_SIZE) return MM_FALSE;
    *value_out = read_slice(c->regs[offset / 4], offset & 3u, size_bytes);
    return MM_TRUE;
}

static mm_bool cracen_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct cracen_state *c = (struct cracen_state *)opaque;
    if (c == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > CRACEN_SIZE) return MM_FALSE;
    c->regs[offset / 4] = apply_write(c->regs[offset / 4], offset & 3u, size_bytes, value);
    if (offset == CRACEN_ENABLE) {
        cracen_rng_update(c);
    }
    return MM_TRUE;
}

static mm_bool cracencore_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct cracen_state *c = (struct cracen_state *)opaque;
    mm_u32 reg;
    if (c == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > CRACENCORE_SIZE) return MM_FALSE;

    if (offset == RNG_FIFOLEVEL && size_bytes == 4) {
        if (c->rng_state == RNG_STATE_IDLERON) {
            c->fifo_level = RNG_FIFO_WORDS;
        }
        *value_out = c->fifo_level;
        return MM_TRUE;
    }
    if (offset == RNG_STATUS && size_bytes == 4) {
        reg = c->core[RNG_STATUS / 4] & ~RNG_STATUS_STATE_MASK;
        *value_out = reg | (c->rng_state << RNG_STATUS_STATE_SHIFT);
        return MM_TRUE;
    }
    if (offset >= RNG_FIFO && offset < (RNG_FIFO + RNG_FIFO_WORDS * 4u) && size_bytes == 4) {
        if (c->fifo_level == 0u) {
            *value_out = 0u;
            return MM_TRUE;
        }
        c->fifo_level--;
        *value_out = mm_host_rng_u32();
        return MM_TRUE;
    }

    *value_out = read_slice(c->core[offset / 4], offset & 3u, size_bytes);
    return MM_TRUE;
}

static mm_bool cracencore_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct cracen_state *c = (struct cracen_state *)opaque;
    if (c == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > CRACENCORE_SIZE) return MM_FALSE;

    if (offset == RNG_FIFOLEVEL || offset == RNG_FIFODEPTH ||
        (offset >= RNG_FIFO && offset < (RNG_FIFO + RNG_FIFO_WORDS * 4u))) {
        return MM_TRUE;
    }
    c->core[offset / 4] = apply_write(c->core[offset / 4], offset & 3u, size_bytes, value);
    if (offset == RNG_CONTROL) {
        cracen_rng_update(c);
    }
    return MM_TRUE;
}

static mm_bool reg_file_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct reg_file *f = (struct reg_file *)opaque;
    if (f == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > sizeof(f->regs)) return MM_FALSE;
    *value_out = read_slice(f->regs[offset / 4], offset & 3u, size_bytes);
    return MM_TRUE;
}

static mm_bool reg_file_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct reg_file *f = (struct reg_file *)opaque;
    if (f == 0 || size_bytes == 0 || size_bytes > 4) return MM_FALSE;
    if ((offset + size_bytes) > sizeof(f->regs)) return MM_FALSE;
    f->regs[offset / 4] = apply_write(f->regs[offset / 4], offset & 3u, size_bytes, value);
    return MM_TRUE;
}

/* Cache maintenance completes at once: STATUS never reads Busy. */
static mm_bool icache_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    if (offset == ICACHE_TASKS_INVALIDATECACHE || offset == ICACHE_TASKS_INVALIDATELINE ||
        offset == ICACHE_TASKS_ERASE || offset == ICACHE_STATUS) {
        return (size_bytes != 0 && size_bytes <= 4) ? MM_TRUE : MM_FALSE;
    }
    return reg_file_write(opaque, offset, size_bytes, value);
}

static void nrf54l_state_init(void)
{
    mm_u32 i;

    for (i = 0; i < NRF54L_GPIO_MAX; ++i) {
        nrf_gpio_bank_init(&gpio_banks[i], (int)i, 32);
        nrf_gpio_bank_set_layout(&gpio_banks[i], &nrf_gpio_layout_nrf54l);
    }
    mm_gpio_set_external_input_writer(nrf_gpio_set_external_input_cb, 0);
    memset(&ficr_state, 0xFF, sizeof(ficr_state));
    memset(&clock_state, 0, sizeof(clock_state));
    memset(&osc_state, 0, sizeof(osc_state));
    memset(&grtc_state, 0, sizeof(grtc_state));
    memset(&mpc_state, 0, sizeof(mpc_state));
    memset(&cracen_state, 0, sizeof(cracen_state));
    memset(&tampc_state, 0, sizeof(tampc_state));
    memset(&icache_state, 0, sizeof(icache_state));
    memset(&rramc_state.regs, 0, sizeof(rramc_state.regs));
    rramc_state.buf_count = 0u;

    if (V != 0) {
        ficr_state.regs[FICR_INFO_RAM / 4] = V->ficr_info_ram;
        ficr_state.regs[FICR_INFO_RRAM / 4] = V->ficr_info_rram;
    }
    osc_state.regs[OSC_PLL_FREQ / 4] = 0x3u;
    osc_state.regs[OSC_PLL_CURRENTFREQ / 4] = 0x3u;
    osc_state.regs[OSC_XOSC32KI_INTCAP / 4] = 0x17u;
    grtc_state.running = MM_TRUE;
    grtc_state.regs[GRTC_MODE / 4] |= (1u << 1);
    grtc_state.regs[GRTC_STATUS_LFTIMER / 4] = 1u;
    grtc_state.regs[GRTC_STATUS_PWM / 4] = 1u;
    grtc_state.regs[GRTC_STATUS_CLKOUT / 4] = 1u;
    for (i = 0; i < SPU_COUNT; ++i) {
        spu_reset_one(&spu_state[i], g_spu_bases[i]);
    }
    cracen_state.core[RNG_CONTROL / 4] = RNG_CONTROL_RESET;
    cracen_state.core[RNG_FIFOTHRESHOLD / 4] = 3u;
    cracen_state.core[RNG_FIFODEPTH / 4] = RNG_FIFO_WORDS;
    cracen_rng_update(&cracen_state);
}

static mm_bool register_pair(struct mmio_bus *bus, struct mmio_region *reg, mm_u32 base_ns)
{
    reg->base = base_ns;
    if (!mmio_bus_register_region(bus, reg)) return MM_FALSE;
    reg->base = base_ns + PERIPH_S_OFFSET;
    return mmio_bus_register_region(bus, reg);
}

#define SPU_FEATURE_GRTC_CC         0xD00u
#define SPU_FEATURE_GRTC_PWMCONFIG  0xD74u
#define SPU_FEATURE_GRTC_CLK        0xD78u
#define SPU_FEATURE_GRTC_SYSCOUNTER 0xD7Cu
#define SPU_FEATURE_GRTC_INTERRUPT  0xD80u

/* SPU FEATURE.GRTC entry governing a GRTC register, or 0 for the
 * peripheral-wide registers that follow PERIPH SECATTR. */
static mm_u32 grtc_feature(mm_u32 offset)
{
    if (offset < GRTC_TASKS_START) return SPU_FEATURE_GRTC_CC + (offset / 4u) * 4u;
    if (offset >= GRTC_EVENTS_COMPARE0 && offset < GRTC_EVENTS_COMPARE0 + 24u * 4u) {
        return SPU_FEATURE_GRTC_CC + ((offset - GRTC_EVENTS_COMPARE0) / 4u) * 4u;
    }
    if (offset >= GRTC_INTEN0 && offset < GRTC_INTEN0 + 16u * GRTC_INT_GROUP_STRIDE) {
        return SPU_FEATURE_GRTC_INTERRUPT + ((offset - GRTC_INTEN0) / GRTC_INT_GROUP_STRIDE) * 4u;
    }
    if (offset >= GRTC_CC_BASE && offset < GRTC_CC_BASE + 24u * GRTC_CC_STRIDE) {
        return SPU_FEATURE_GRTC_CC + ((offset - GRTC_CC_BASE) / GRTC_CC_STRIDE) * 4u;
    }
    if (offset >= GRTC_SYSCOUNTER_BASE &&
        offset < GRTC_SYSCOUNTER_BASE + GRTC_SYSCOUNTER_COUNT * GRTC_SYSCOUNTER_STRIDE) {
        return SPU_FEATURE_GRTC_SYSCOUNTER;
    }
    if (offset == GRTC_INTERVAL) return SPU_FEATURE_GRTC_CC;
    if (offset == GRTC_STATUS_PWM) return SPU_FEATURE_GRTC_PWMCONFIG;
    if (offset == GRTC_STATUS_CLKOUT) return SPU_FEATURE_GRTC_CLK;
    return 0u;
}

/* The GRTC non-secure alias reaches each feature its SPU entry makes
 * non-secure, and the common registers when the GRTC itself is non-secure. */
static mm_bool grtc_ns_allowed(mm_u32 offset)
{
    struct spu_state *spu = spu_for_secure_addr(GRTC_BASE_NS | PERIPH_S_OFFSET);
    mm_u32 f = grtc_feature(offset & ~3u);
    mm_bool ok;
    if (spu == 0) return MM_TRUE;
    if (f != 0u) {
        ok = ((spu->regs[f / 4] & SPU_PERM_SECATTR) == 0u) ? MM_TRUE : MM_FALSE;
    } else {
        ok = spu_perm_nonsecure(GRTC_BASE_NS);
    }
    if (!ok) spu_access_error(spu, GRTC_BASE_NS + offset);
    return ok;
}

static mm_bool grtc_read_ns(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    if (!grtc_ns_allowed(offset)) return MM_FALSE;
    return grtc_read(opaque, offset, size_bytes, value_out);
}

static mm_bool grtc_write_ns(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    if (!grtc_ns_allowed(offset)) return MM_FALSE;
    return grtc_write(opaque, offset, size_bytes, value);
}

mm_bool nrf54l_register_mmio(const struct nrf54l_mmio_variant *v, struct mmio_bus *bus)
{
    struct mmio_region reg;
    mm_u32 i;
    if (v == 0 || bus == 0) return MM_FALSE;
    V = v;

    nrf54l_state_init();
    memset(&reg, 0, sizeof(reg));

    reg.base = FICR_BASE_NS;
    reg.size = FICR_SIZE;
    reg.opaque = &ficr_state;
    reg.read = ficr_read;
    reg.write = ficr_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.size = GPIO_SIZE;
    for (i = 0; i < (mm_u32)gpio_port_count(); ++i) {
        reg.opaque = &gpio_banks[i];
        reg.base = g_gpio_bases_ns[i];
        reg.read = gpio_read_ns;
        reg.write = gpio_write_ns;
        if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
        reg.base = g_gpio_bases_ns[i] + PERIPH_S_OFFSET;
        reg.read = nrf_gpio_read;
        reg.write = nrf_gpio_write;
        if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    }

    reg.base = v->rramc_base;
    reg.size = RRAMC_SIZE;
    reg.opaque = &rramc_state;
    reg.read = rramc_read;
    reg.write = rramc_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.size = CLOCK_SIZE;
    reg.opaque = &clock_state;
    reg.read = clock_read;
    reg.write = clock_write;
    if (!register_pair(bus, &reg, CLOCK_BASE_NS)) return MM_FALSE;

    reg.size = OSCILLATORS_SIZE;
    reg.opaque = &osc_state;
    reg.read = oscillators_read;
    reg.write = oscillators_write;
    if (!register_pair(bus, &reg, OSCILLATORS_BASE_NS)) return MM_FALSE;

    reg.size = GRTC_SIZE;
    reg.opaque = &grtc_state;
    reg.base = GRTC_BASE_NS;
    reg.read = grtc_read_ns;
    reg.write = grtc_write_ns;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = GRTC_BASE_NS + PERIPH_S_OFFSET;
    reg.read = grtc_read;
    reg.write = grtc_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.size = SPU_SIZE;
    reg.read = spu_read;
    reg.write = spu_write;
    for (i = 0; i < SPU_COUNT; ++i) {
        reg.base = g_spu_bases[i];
        reg.opaque = &spu_state[i];
        if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    }

    reg.base = MPC00_BASE_S;
    reg.size = MPC_SIZE;
    reg.opaque = &mpc_state;
    reg.read = mpc_read;
    reg.write = mpc_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = v->cracen_base;
    reg.size = CRACEN_SIZE;
    reg.opaque = &cracen_state;
    reg.read = cracen_read;
    reg.write = cracen_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = v->cracencore_base;
    reg.size = CRACENCORE_SIZE;
    reg.read = cracencore_read;
    reg.write = cracencore_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = v->tampc_base;
    reg.size = TAMPC_SIZE;
    reg.opaque = &tampc_state;
    reg.read = reg_file_read;
    reg.write = reg_file_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    reg.base = ICACHE_BASE_S;
    reg.size = ICACHE_SIZE;
    reg.opaque = &icache_state;
    reg.read = reg_file_read;
    reg.write = icache_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;

    mm_gpio_bank_set_reader(nrf_gpio_bank_read, 0);
    mm_gpio_bank_set_moder_reader(nrf_gpio_bank_read_moder, 0);
    mm_gpio_bank_set_clock_reader(nrf_gpio_bank_clock, 0);
    mm_gpio_bank_set_seccfgr_reader(nrf_gpio_bank_read_seccfgr, 0);

    return MM_TRUE;
}

void nrf54l_flash_bind(const struct nrf54l_mmio_variant *v,
                       struct mm_memmap *map,
                       mm_u8 *flash,
                       mm_u32 flash_size,
                       const struct mm_flash_persist *persist,
                       mm_u32 flags)
{
    if (map == 0) return;
    if (v != 0) {
        V = v;
    }
    rramc_state.flash = flash;
    rramc_state.flash_size = flash_size;
    rramc_state.persist = persist;
    rramc_state.map = map;
    rramc_state.flags = flags;
    mm_memmap_set_flash_writer(map, rramc_write_cb, &rramc_state);
    mm_memmap_set_access_filter(map, mpc_filter, &rramc_state);
}

mm_u64 nrf54l_cpu_hz(void)
{
    /* SystemCoreClockUpdate uses OSCILLATORS->PLL.CURRENTFREQ (64/128 MHz). */
    mm_u32 freq = osc_state.regs[OSC_PLL_CURRENTFREQ / 4] & 0x3u;
    if (freq == 0x1u) return 128000000ull;
    if (freq == 0x3u) return 64000000ull;
    return 128000000ull;
}

void nrf54l_mmio_reset(const struct nrf54l_mmio_variant *v)
{
    if (v != 0) {
        V = v;
    }
    if (rramc_state.flash != 0) {
        rramc_discard(&rramc_state);
    }
    nrf54l_state_init();
    mm_gpio_set_bank_info_reader(nrf54l_gpio_bank_info, 0);
}

void nrf54l_mmio_set_nvic(struct mm_nvic *nvic)
{
    g_nrf54l_nvic = nvic;
}

void nrf54l_grtc_tick(mm_u64 cycles)
{
    mm_u64 div;
    mm_u64 ticks;
    mm_u64 hz;
    mm_u32 ch;
    mm_u32 interval;
    mm_bool fired;
    if (!grtc_state.running) return;
    hz = nrf54l_cpu_hz();
    if (hz == 0u) return;
    div = hz / 1000000ull;
    if (div == 0u) return;

    grtc_state.accum_cycles += cycles;
    ticks = grtc_state.accum_cycles / div;
    if (ticks == 0u) return;
    grtc_state.accum_cycles -= ticks * div;
    grtc_state.syscounter += ticks;

    interval = grtc_state.regs[GRTC_INTERVAL / 4] & 0xFFFFu;
    for (ch = 0; ch < GRTC_CC_COUNT; ++ch) {
        if ((grtc_state.cc_active_mask & (1u << ch)) == 0u) continue;
        if (grtc_state.syscounter >= grtc_state.cc[ch]) {
            fired = grtc_set_compare_event(&grtc_state, ch);
            if (fired && ch == 0u && interval != 0u) {
                grtc_state.cc[0] += interval;
                grtc_sync_cc_regs(&grtc_state, 0u);
            }
        }
    }
}

mm_bool nrf54l_tz_attr_for_addr(mm_u32 addr,
                                enum mm_sau_attr *attr_out,
                                mm_u32 *region_out)
{
    const struct spu_state *spu;
    mm_u32 slave;

    if (attr_out == 0) return MM_FALSE;
    if (region_out != 0) {
        *region_out = 0u;
    }
    /* The 0x5xxxxxxx peripheral window and the ICACHE are Secure only. */
    if ((addr >= PERIPH_BASE_S && addr < PERIPH_BASE_S + PERIPH_S_OFFSET) ||
        addr - ICACHE_BASE_S < ICACHE_SIZE) {
        *attr_out = MM_SAU_SECURE;
        return MM_TRUE;
    }
    if (addr < PERIPH_BASE_NS || addr >= PERIPH_BASE_S) return MM_FALSE;
    if (nrf54l_split_periph(addr)) return MM_FALSE;
    spu = spu_for_secure_addr(addr + PERIPH_S_OFFSET);
    if (spu == 0) return MM_FALSE;
    slave = (addr >> 12) & (SPU_PERIPH_COUNT - 1u);
    if ((spu->regs[(SPU_PERIPH_PERM / 4) + slave] & SPU_PERM_SECATTR) == 0u) {
        return MM_FALSE;
    }
    *attr_out = MM_SAU_SECURE;
    return MM_TRUE;
}

enum mm_sec_state nrf54l_dma_sec(mm_u32 base_ns)
{
    const struct spu_state *spu = spu_for_secure_addr(base_ns + PERIPH_S_OFFSET);
    mm_u32 perm;
    if (spu == 0) return MM_SECURE;
    perm = spu->regs[(SPU_PERIPH_PERM / 4) + ((base_ns >> 12) & (SPU_PERIPH_COUNT - 1u))];
    if ((perm & SPU_PERM_SECATTR) == 0u || (perm & SPU_PERM_DMASEC) == 0u) {
        return MM_NONSECURE;
    }
    return MM_SECURE;
}

void nrf54l_timers_init(struct mmio_bus *bus, struct mm_nvic *nvic)
{
    static const mm_u32 bases_ns[] = {
        0x40055000u, 0x40085000u, 0x400CA000u, 0x400CB000u,
        0x400CC000u, 0x400CD000u, 0x400CE000u
    };
    static const mm_u32 bases_s[] = {
        0x50055000u, 0x50085000u, 0x500CA000u, 0x500CB000u,
        0x500CC000u, 0x500CD000u, 0x500CE000u
    };
    static const int irqs[] = { 85, 133, 202, 203, 204, 205, 206 };
    if (bus == 0) return;
    nrf54l_mmio_set_nvic(nvic);
    nrf_timers_register(&g_timers, bus, nvic, bases_ns, bases_s, irqs, 7u);
    nrf_timer_set_cc_count(&g_timers, 1u, 8u); /* TIMER10 */
}

void nrf54l_timers_reset(void)
{
    nrf_timers_reset(&g_timers);
}

/* TIMER00 runs from PCLK128M, TIMER10 from PCLK32M, TIMER20..24 from PCLK16M. */
static const mm_u64 g_timer_pclk_hz[NRF_TIMER_MAX_INSTANCES] = {
    128000000ull, 32000000ull, 16000000ull, 16000000ull,
    16000000ull, 16000000ull, 16000000ull
};

void nrf54l_timers_tick(mm_u64 cycles)
{
    size_t i;
    mm_u64 cpu_hz = nrf54l_cpu_hz();
    nrf54l_grtc_tick(cycles);
    if (cpu_hz == 0u) return;
    for (i = 0; i < g_timers.count; ++i) {
        struct nrf_timer_state *t = &g_timers.timers[i];
        mm_u64 ticks;
        mm_u64 div;
        mm_u32 prescaler;
        mm_u32 mask;
        mm_u32 old;
        mm_u32 now;
        mm_u32 j;
        if (!t->running) continue;
        prescaler = t->regs[0x510u / 4u] & 0xFu;
        /* accum counts PCLK edges scaled by the CPU clock. */
        div = cpu_hz << prescaler;
        t->accum += cycles * g_timer_pclk_hz[i];
        ticks = t->accum / div;
        if (ticks == 0u) continue;
        t->accum -= ticks * div;
        mask = nrf_timer_bitmask(t);
        old = t->counter;
        now = (old + (mm_u32)ticks) & mask;
        t->counter = now;
        if (now == old) continue;
        for (j = 0; j < t->cc_count; ++j) {
            mm_u32 cc = t->cc[j] & mask;
            if (old <= now) {
                if (cc > old && cc <= now) {
                    nrf_timer_set_event(t, (mm_u32)j);
                }
            } else if (cc > old || cc <= now) {
                nrf_timer_set_event(t, (mm_u32)j);
            }
        }
    }
}
