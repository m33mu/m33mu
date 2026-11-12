#ifndef M33MU_NRF_UART_SPI_H
#define M33MU_NRF_UART_SPI_H

#include "m33mu/mmio.h"
#include "m33mu/nvic.h"
#include "m33mu/target_hal.h"

#define NRF_SERIAL_SIZE 0x1000u
#define NRF_SERIAL_MAX_INSTANCES 7u

struct nrf_serial_state;

struct nrf_serial_config {
    const mm_u32 *bases;
    const int *irqs;
    const mm_bool *has_uarte;
    size_t count;
    mm_bool (*clock_ready)(void);
    mm_bool open_on_enable;
    mm_bool open_during_init;
    mm_bool flushrx_reads_uart;
    mm_bool stop_spim_ends_bus;
    mm_bool starttx_sets_txdrdy;
    mm_bool endtx_only_when_nonzero;
    mm_bool explicit_event_window;
    /* One address per instance: no base + 0x10000000 alias (nRF5340 net) */
    mm_bool single_view;
};

struct nrf_serial_inst {
    struct nrf_serial_state *owner;
    mm_u32 base;
    mm_u32 regs[NRF_SERIAL_SIZE / 4u];
    mm_u32 bus_index;
    int irq;
    mm_bool has_uarte;
    mm_bool rx_running;
    struct mm_uart_io io;
    char label[16];
};

struct nrf_serial_state {
    struct nrf_serial_inst serials[NRF_SERIAL_MAX_INSTANCES];
    size_t count;
    mm_bool init_done;
    struct mm_nvic *nvic;
    const struct nrf_serial_config *cfg;
};

void nrf_serial_register_all(struct nrf_serial_state *state,
                             const struct nrf_serial_config *cfg,
                             struct mmio_bus *bus,
                             struct mm_nvic *nvic);
void nrf_serial_reset_all(struct nrf_serial_state *state);
void nrf_serial_usart_poll(struct nrf_serial_state *state);

#endif /* M33MU_NRF_UART_SPI_H */
