#ifndef M33MU_NRF_TIMERS_H
#define M33MU_NRF_TIMERS_H

#include "m33mu/mmio.h"
#include "m33mu/nvic.h"

#define NRF_TIMER_SIZE 0x1000u
#define NRF_TIMER_MAX_CC 8u
#define NRF_TIMER_DEFAULT_CC 6u
#define NRF_TIMER_MAX_INSTANCES 7u

struct nrf_timers_state;

struct nrf_timer_state {
    struct nrf_timers_state *owner;
    mm_u32 regs[NRF_TIMER_SIZE / 4u];
    mm_u32 cc[NRF_TIMER_MAX_CC];
    mm_u32 counter;
    mm_u64 accum;
    mm_bool running;
    int irq;
    mm_u32 cc_count;
};

struct nrf_timers_state {
    struct nrf_timer_state timers[NRF_TIMER_MAX_INSTANCES];
    size_t count;
    struct mm_nvic *nvic;
};

mm_u32 nrf_timer_bitmask(const struct nrf_timer_state *t);
void nrf_timer_set_event(struct nrf_timer_state *t, mm_u32 ch);
void nrf_timers_register(struct nrf_timers_state *state,
                         struct mmio_bus *bus,
                         struct mm_nvic *nvic,
                         const mm_u32 *bases_ns,
                         const mm_u32 *bases_s,
                         const int *irqs,
                         size_t count);
void nrf_timers_reset(struct nrf_timers_state *state);
/* Instances default to NRF_TIMER_DEFAULT_CC compare channels. */
void nrf_timer_set_cc_count(struct nrf_timers_state *state, size_t index, mm_u32 count);

#endif /* M33MU_NRF_TIMERS_H */
