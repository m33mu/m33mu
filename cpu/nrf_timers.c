#include <string.h>
#include "nrf_timers.h"

#define TIMER_TASKS_START 0x000u
#define TIMER_TASKS_STOP  0x004u
#define TIMER_TASKS_COUNT 0x008u
#define TIMER_TASKS_CLEAR 0x00Cu
#define TIMER_TASKS_SHUTDOWN 0x010u
#define TIMER_EVENTS_COMPARE0 0x140u
#define TIMER_INTENSET 0x304u
#define TIMER_INTENCLR 0x308u
#define TIMER_BITMODE 0x508u
#define TIMER_PRESCALER 0x510u
#define TIMER_CC0 0x540u

mm_u32 nrf_timer_bitmask(const struct nrf_timer_state *t)
{
    mm_u32 mode = t->regs[TIMER_BITMODE / 4u] & 0x3u;
    if (mode == 0u) return 0xFFFFu;
    if (mode == 1u) return 0xFFu;
    if (mode == 2u) return 0xFFFFFFu;
    return 0xFFFFFFFFu;
}

static void nrf_timer_raise_irq(struct nrf_timer_state *t, mm_u32 ch)
{
    mm_u32 mask = (1u << ch);
    if (t->owner == 0 || t->owner->nvic == 0) return;
    if ((t->regs[TIMER_INTENSET / 4u] & mask) == 0u) return;
    mm_nvic_set_pending(t->owner->nvic, (mm_u32)t->irq, MM_TRUE);
}

void nrf_timer_set_event(struct nrf_timer_state *t, mm_u32 ch)
{
    mm_u32 off = TIMER_EVENTS_COMPARE0 + ch * 4u;
    t->regs[off / 4u] = 1u;
    nrf_timer_raise_irq(t, ch);
}

static mm_bool timer_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct nrf_timer_state *t = (struct nrf_timer_state *)opaque;
    if (t == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > NRF_TIMER_SIZE) return MM_FALSE;
    if (offset >= TIMER_CC0 && offset < TIMER_CC0 + t->cc_count * 4u && size_bytes == 4u) {
        *value_out = t->cc[(offset - TIMER_CC0) / 4u];
        return MM_TRUE;
    }
    if (offset == TIMER_TASKS_COUNT && size_bytes == 4u) {
        *value_out = t->counter;
        return MM_TRUE;
    }
    *value_out = t->regs[offset / 4u];
    return MM_TRUE;
}

static mm_bool timer_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct nrf_timer_state *t = (struct nrf_timer_state *)opaque;
    if (t == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > NRF_TIMER_SIZE) return MM_FALSE;
    if (offset == TIMER_TASKS_START && size_bytes == 4u) {
        if ((value & 1u) != 0u) t->running = MM_TRUE;
        return MM_TRUE;
    }
    if (offset == TIMER_TASKS_STOP && size_bytes == 4u) {
        if ((value & 1u) != 0u) t->running = MM_FALSE;
        return MM_TRUE;
    }
    if (offset == TIMER_TASKS_CLEAR && size_bytes == 4u) {
        if ((value & 1u) != 0u) t->counter = 0u;
        return MM_TRUE;
    }
    if (offset == TIMER_TASKS_COUNT && size_bytes == 4u) {
        if ((value & 1u) != 0u) {
            t->counter = (t->counter + 1u) & nrf_timer_bitmask(t);
        }
        return MM_TRUE;
    }
    if (offset == TIMER_TASKS_SHUTDOWN && size_bytes == 4u) {
        if ((value & 1u) != 0u) t->running = MM_FALSE;
        return MM_TRUE;
    }
    if (offset >= TIMER_EVENTS_COMPARE0 && offset < TIMER_EVENTS_COMPARE0 + t->cc_count * 4u && size_bytes == 4u) {
        if (value == 0u) {
            t->regs[offset / 4u] = 0u;
        }
        return MM_TRUE;
    }
    if (offset == TIMER_INTENSET && size_bytes == 4u) {
        t->regs[TIMER_INTENSET / 4u] |= value;
        return MM_TRUE;
    }
    if (offset == TIMER_INTENCLR && size_bytes == 4u) {
        t->regs[TIMER_INTENSET / 4u] &= ~value;
        return MM_TRUE;
    }
    if (offset >= TIMER_CC0 && offset < TIMER_CC0 + t->cc_count * 4u && size_bytes == 4u) {
        t->cc[(offset - TIMER_CC0) / 4u] = value;
        return MM_TRUE;
    }
    t->regs[offset / 4u] = value;
    return MM_TRUE;
}

void nrf_timers_register(struct nrf_timers_state *state,
                         struct mmio_bus *bus,
                         struct mm_nvic *nvic,
                         const mm_u32 *bases_ns,
                         const mm_u32 *bases_s,
                         const int *irqs,
                         size_t count)
{
    struct mmio_region reg;
    size_t i;
    if (state == 0 || bus == 0) return;
    memset(state, 0, sizeof(*state));
    state->count = count > NRF_TIMER_MAX_INSTANCES ? NRF_TIMER_MAX_INSTANCES : count;
    state->nvic = nvic;
    memset(&reg, 0, sizeof(reg));
    reg.size = NRF_TIMER_SIZE;
    reg.read = timer_read;
    reg.write = timer_write;
    for (i = 0; i < state->count; ++i) {
        state->timers[i].owner = state;
        state->timers[i].irq = irqs[i];
        state->timers[i].cc_count = NRF_TIMER_DEFAULT_CC;
        reg.base = bases_ns[i];
        reg.opaque = &state->timers[i];
        if (!mmio_bus_register_region(bus, &reg)) return;
        if (bases_s[i] == bases_ns[i]) continue;
        reg.base = bases_s[i];
        if (!mmio_bus_register_region(bus, &reg)) return;
    }
}

void nrf_timers_reset(struct nrf_timers_state *state)
{
    size_t i;
    struct mm_nvic *nvic;
    size_t count;
    int irqs[NRF_TIMER_MAX_INSTANCES];
    mm_u32 cc_counts[NRF_TIMER_MAX_INSTANCES];
    if (state == 0) return;
    nvic = state->nvic;
    count = state->count;
    for (i = 0; i < NRF_TIMER_MAX_INSTANCES; ++i) {
        irqs[i] = state->timers[i].irq;
        cc_counts[i] = state->timers[i].cc_count;
    }
    memset(state, 0, sizeof(*state));
    state->nvic = nvic;
    state->count = count;
    for (i = 0; i < state->count; ++i) {
        state->timers[i].owner = state;
        state->timers[i].irq = irqs[i];
        state->timers[i].cc_count = cc_counts[i];
    }
}

void nrf_timer_set_cc_count(struct nrf_timers_state *state, size_t index, mm_u32 count)
{
    if (state == 0 || index >= state->count) return;
    if (count > NRF_TIMER_MAX_CC) count = NRF_TIMER_MAX_CC;
    state->timers[index].cc_count = count;
}
