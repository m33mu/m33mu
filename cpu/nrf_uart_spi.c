#include <string.h>
#include <stdio.h>
#include "nrf_uart_spi.h"
#include "m33mu/mmio.h"
#include "m33mu/memmap.h"
#include "m33mu/spi_bus.h"

#define UARTE_TASKS_STARTRX 0x000u
#define UARTE_TASKS_STOPRX  0x004u
#define UARTE_TASKS_STARTTX 0x008u
#define UARTE_TASKS_STOPTX  0x00Cu
#define UARTE_TASKS_FLUSHRX 0x02Cu

#define SPIM_TASKS_START 0x010u
#define SPIM_TASKS_STOP  0x014u

#define EVENTS_CTS     0x100u
#define EVENTS_NCTS    0x104u
#define EVENTS_RXDRDY  0x108u
#define EVENTS_ENDRX   0x110u
#define EVENTS_TXDRDY  0x11Cu
#define EVENTS_ENDTX   0x120u
#define EVENTS_ERROR   0x124u
#define EVENTS_RXTO    0x144u

#define SPIM_EVENTS_STOPPED 0x104u
#define SPIM_EVENTS_ENDRX   0x110u
#define SPIM_EVENTS_END     0x118u
#define SPIM_EVENTS_ENDTX   0x120u
#define SPIM_EVENTS_STARTED 0x14Cu

#define INTENSET 0x304u
#define INTENCLR 0x308u

#define ENABLE 0x500u
#define RXD_PTR 0x534u
#define RXD_MAXCNT 0x538u
#define RXD_AMOUNT 0x53Cu
#define TXD_PTR 0x544u
#define TXD_MAXCNT 0x548u
#define TXD_AMOUNT 0x54Cu
#define ORC 0x5C0u

#define ENABLE_SPIM  7u
#define ENABLE_UARTE 8u

#define INT_RXDRDY (1u << 2)
#define INT_ENDRX  (1u << 4)
#define INT_TXDRDY (1u << 7)
#define INT_ENDTX  (1u << 8)

/* nRF54 layout (cfg->dma_regs): EasyDMA tasks, events and pointers moved
 * into TASKS_DMA/EVENTS_DMA and the DMA.RX/DMA.TX register groups. */
#define DMA_UARTE_TASKS_FLUSHRX 0x01Cu
#define DMA_TASKS_RX_START      0x028u
#define DMA_TASKS_RX_STOP       0x02Cu
#define DMA_TASKS_TX_START      0x050u
#define DMA_TASKS_TX_STOP       0x054u
#define DMA_SPIM_TASKS_START    0x000u
#define DMA_SPIM_TASKS_STOP     0x004u

#define DMA_SPIM_EVENTS_STARTED 0x100u
#define DMA_SPIM_EVENTS_STOPPED 0x104u
#define DMA_SPIM_EVENTS_END     0x108u
#define DMA_EVENTS_TXDRDY       0x10Cu
#define DMA_EVENTS_RXDRDY       0x110u
#define DMA_EVENTS_RXTO         0x124u
#define DMA_EVENTS_TXSTOPPED    0x130u
#define DMA_EVENTS_RX_END       0x14Cu
#define DMA_EVENTS_RX_READY     0x150u
#define DMA_EVENTS_RX_BUSERROR  0x154u
#define DMA_EVENTS_TX_END       0x168u
#define DMA_EVENTS_TX_READY     0x16Cu
#define DMA_EVENTS_TX_BUSERROR  0x170u
#define DMA_EVENTS_FIRST        0x100u
#define DMA_EVENTS_LAST         0x17Cu

#define DMA_INTEN    0x300u
#define DMA_INTENSET 0x304u
#define DMA_INTENCLR 0x308u

#define DMA_RX_PTR    0x704u
#define DMA_RX_MAXCNT 0x708u
#define DMA_RX_AMOUNT 0x70Cu
#define DMA_TX_PTR    0x73Cu
#define DMA_TX_MAXCNT 0x740u
#define DMA_TX_AMOUNT 0x744u
#define DMA_RX_BUSERRORADDRESS 0x720u
#define DMA_TX_BUSERRORADDRESS 0x758u

#define DMA_INT_SPIM_STARTED (1u << 0)
#define DMA_INT_SPIM_STOPPED (1u << 1)
#define DMA_INT_SPIM_END     (1u << 2)
#define DMA_INT_TXDRDY       (1u << 3)
#define DMA_INT_RXDRDY       (1u << 4)
#define DMA_INT_RXTO         (1u << 9)
#define DMA_INT_TXSTOPPED    (1u << 12)
#define DMA_INT_RX_END       (1u << 19)
#define DMA_INT_RX_READY     (1u << 20)
#define DMA_INT_RX_BUSERROR  (1u << 21)
#define DMA_INT_TX_END       (1u << 26)
#define DMA_INT_TX_READY     (1u << 27)
#define DMA_INT_TX_BUSERROR  (1u << 28)

static struct mm_memmap *serial_map(void)
{
    return mm_memmap_current();
}

static mm_bool dma_read8(mm_u32 addr, mm_u8 *value_out)
{
    struct mm_memmap *map = serial_map();
    if (map == 0 || value_out == 0) return MM_FALSE;
    return mm_memmap_read8(map, mmio_active_sec(), addr, value_out);
}

static mm_bool dma_write8(mm_u32 addr, mm_u8 value)
{
    struct mm_memmap *map = serial_map();
    if (map == 0) return MM_FALSE;
    return mm_memmap_write8(map, mmio_active_sec(), addr, value);
}

static enum mm_sec_state serial_dma_sec(const struct nrf_serial_inst *s)
{
    if (s->owner->cfg->dma_sec != 0) {
        return s->owner->cfg->dma_sec(s->base);
    }
    return mmio_active_sec();
}

static mm_bool serial_dma_read8(const struct nrf_serial_inst *s, mm_u32 addr, mm_u8 *value_out)
{
    struct mm_memmap *map = serial_map();
    if (map == 0 || value_out == 0) return MM_FALSE;
    return mm_memmap_dma_read8(map, serial_dma_sec(s), addr, value_out);
}

static mm_bool serial_dma_write8(const struct nrf_serial_inst *s, mm_u32 addr, mm_u8 value)
{
    struct mm_memmap *map = serial_map();
    if (map == 0) return MM_FALSE;
    return mm_memmap_dma_write8(map, serial_dma_sec(s), addr, value);
}

static mm_bool serial_clock_ready(const struct nrf_serial_state *state)
{
    if (state == 0 || state->cfg == 0 || state->cfg->clock_ready == 0) {
        return MM_TRUE;
    }
    return state->cfg->clock_ready();
}

static void serial_raise_irq(struct nrf_serial_inst *s, mm_u32 mask)
{
    if (s == 0 || s->owner == 0 || s->owner->nvic == 0) return;
    if ((s->regs[INTENSET / 4u] & mask) == 0u) return;
    mm_nvic_set_pending(s->owner->nvic, (mm_u32)s->irq, MM_TRUE);
}

static void serial_event_set(struct nrf_serial_inst *s, mm_u32 offset, mm_u32 mask)
{
    s->regs[offset / 4u] = 1u;
    serial_raise_irq(s, mask);
}

static void serial_uarte_ensure_open(struct nrf_serial_inst *s)
{
    if (s == 0 || !s->has_uarte) return;
    if (s->io.fd >= 0) return;
    if (mm_uart_io_open(&s->io, s->base) && mm_tui_is_active()) {
        mm_tui_attach_uart(s->label, s->io.name);
    }
}

static void serial_spim_run(struct nrf_serial_inst *s)
{
    mm_u32 tx_cnt, rx_cnt, tx_ptr, rx_ptr, i, total;
    mm_u8 orc;
    if (s == 0) return;
    if (!serial_clock_ready(s->owner)) return;
    if ((s->regs[ENABLE / 4u] & 0xFu) != ENABLE_SPIM) return;
    tx_cnt = s->regs[TXD_MAXCNT / 4u];
    rx_cnt = s->regs[RXD_MAXCNT / 4u];
    tx_ptr = s->regs[TXD_PTR / 4u];
    rx_ptr = s->regs[RXD_PTR / 4u];
    orc = (mm_u8)(s->regs[ORC / 4u] & 0xFFu);
    total = (tx_cnt > rx_cnt) ? tx_cnt : rx_cnt;
    for (i = 0; i < total; ++i) {
        mm_u8 out = orc;
        mm_u8 in;
        if (i < tx_cnt) {
            (void)dma_read8(tx_ptr + i, &out);
        }
        in = mm_spi_bus_xfer((int)s->bus_index, out);
        if (i < rx_cnt) {
            (void)dma_write8(rx_ptr + i, in);
        }
    }
    s->regs[TXD_AMOUNT / 4u] = tx_cnt;
    s->regs[RXD_AMOUNT / 4u] = rx_cnt;
    serial_event_set(s, SPIM_EVENTS_ENDTX, INT_ENDTX);
    serial_event_set(s, SPIM_EVENTS_ENDRX, INT_ENDRX);
    serial_event_set(s, SPIM_EVENTS_END, INT_ENDTX | INT_ENDRX);
    mm_spi_bus_end((int)s->bus_index);
}

static void serial_uarte_try_rx(struct nrf_serial_inst *s)
{
    mm_u32 rx_cnt, rx_ptr, amount;
    if (s == 0 || !s->rx_running) return;
    if (!serial_clock_ready(s->owner)) return;
    if ((s->regs[ENABLE / 4u] & 0xFu) != ENABLE_UARTE) return;
    rx_cnt = s->regs[RXD_MAXCNT / 4u];
    rx_ptr = s->regs[RXD_PTR / 4u];
    amount = s->regs[RXD_AMOUNT / 4u];
    while (amount < rx_cnt && mm_uart_io_has_rx(&s->io)) {
        mm_u8 byte = mm_uart_io_read(&s->io);
        (void)dma_write8(rx_ptr + amount, byte);
        amount++;
        serial_event_set(s, EVENTS_RXDRDY, INT_RXDRDY);
    }
    s->regs[RXD_AMOUNT / 4u] = amount;
    if (amount >= rx_cnt && rx_cnt != 0u) {
        s->rx_running = MM_FALSE;
        serial_event_set(s, EVENTS_ENDRX, INT_ENDRX);
    }
}

static void serial_uarte_start_tx(struct nrf_serial_inst *s)
{
    mm_u32 tx_cnt, tx_ptr, i;
    if (s == 0) return;
    if (!serial_clock_ready(s->owner)) return;
    if ((s->regs[ENABLE / 4u] & 0xFu) != ENABLE_UARTE) return;
    serial_uarte_ensure_open(s);
    tx_cnt = s->regs[TXD_MAXCNT / 4u];
    tx_ptr = s->regs[TXD_PTR / 4u];
    for (i = 0; i < tx_cnt; ++i) {
        mm_u8 out = 0u;
        (void)dma_read8(tx_ptr + i, &out);
        mm_uart_io_queue_tx(&s->io, out);
        if (s->owner->cfg->starttx_sets_txdrdy) {
            serial_event_set(s, EVENTS_TXDRDY, INT_TXDRDY);
        }
    }
    (void)mm_uart_io_flush(&s->io);
    s->regs[TXD_AMOUNT / 4u] = tx_cnt;
    if (!s->owner->cfg->endtx_only_when_nonzero || tx_cnt != 0u) {
        serial_event_set(s, EVENTS_ENDTX, INT_ENDTX);
    }
}

static void serial_dma_rx_try(struct nrf_serial_inst *s)
{
    mm_u32 rx_cnt, rx_ptr, amount;
    if (s == 0 || !s->rx_running) return;
    if ((s->regs[ENABLE / 4u] & 0xFu) != ENABLE_UARTE) return;
    rx_cnt = s->regs[DMA_RX_MAXCNT / 4u];
    rx_ptr = s->regs[DMA_RX_PTR / 4u];
    amount = s->regs[DMA_RX_AMOUNT / 4u];
    while (amount < rx_cnt && mm_uart_io_has_rx(&s->io)) {
        mm_u8 byte = mm_uart_io_read(&s->io);
        if (!serial_dma_write8(s, rx_ptr + amount, byte)) {
            s->regs[DMA_RX_BUSERRORADDRESS / 4u] = rx_ptr + amount;
            s->regs[DMA_RX_AMOUNT / 4u] = amount;
            s->rx_running = MM_FALSE;
            serial_event_set(s, DMA_EVENTS_RX_BUSERROR, DMA_INT_RX_BUSERROR);
            return;
        }
        amount++;
        serial_event_set(s, DMA_EVENTS_RXDRDY, DMA_INT_RXDRDY);
    }
    s->regs[DMA_RX_AMOUNT / 4u] = amount;
    if (amount >= rx_cnt && rx_cnt != 0u) {
        s->rx_running = MM_FALSE;
        serial_event_set(s, DMA_EVENTS_RX_END, DMA_INT_RX_END);
    }
}

/* FLUSHRX moves what the RX FIFO still holds into the DMA buffer. */
static void serial_dma_flush_rx(struct nrf_serial_inst *s)
{
    mm_u32 rx_cnt = s->regs[DMA_RX_MAXCNT / 4u];
    mm_u32 rx_ptr = s->regs[DMA_RX_PTR / 4u];
    mm_u32 amount = 0u;
    s->rx_running = MM_FALSE;
    while (amount < rx_cnt && mm_uart_io_has_rx(&s->io)) {
        mm_u8 byte = mm_uart_io_read(&s->io);
        if (!serial_dma_write8(s, rx_ptr + amount, byte)) {
            s->regs[DMA_RX_BUSERRORADDRESS / 4u] = rx_ptr + amount;
            s->regs[DMA_RX_AMOUNT / 4u] = amount;
            serial_event_set(s, DMA_EVENTS_RX_BUSERROR, DMA_INT_RX_BUSERROR);
            return;
        }
        amount++;
    }
    s->regs[DMA_RX_AMOUNT / 4u] = amount;
    serial_event_set(s, DMA_EVENTS_RX_END, DMA_INT_RX_END);
}

static void serial_dma_tx_start(struct nrf_serial_inst *s)
{
    mm_u32 tx_cnt, tx_ptr, i;
    if (s == 0) return;
    if ((s->regs[ENABLE / 4u] & 0xFu) != ENABLE_UARTE) return;
    serial_uarte_ensure_open(s);
    tx_cnt = s->regs[DMA_TX_MAXCNT / 4u];
    tx_ptr = s->regs[DMA_TX_PTR / 4u];
    serial_event_set(s, DMA_EVENTS_TX_READY, DMA_INT_TX_READY);
    for (i = 0; i < tx_cnt; ++i) {
        mm_u8 out = 0u;
        if (!serial_dma_read8(s, tx_ptr + i, &out)) {
            s->regs[DMA_TX_BUSERRORADDRESS / 4u] = tx_ptr + i;
            (void)mm_uart_io_flush(&s->io);
            s->regs[DMA_TX_AMOUNT / 4u] = i;
            serial_event_set(s, DMA_EVENTS_TX_BUSERROR, DMA_INT_TX_BUSERROR);
            return;
        }
        mm_uart_io_queue_tx(&s->io, out);
        serial_event_set(s, DMA_EVENTS_TXDRDY, DMA_INT_TXDRDY);
    }
    (void)mm_uart_io_flush(&s->io);
    s->regs[DMA_TX_AMOUNT / 4u] = tx_cnt;
    serial_event_set(s, DMA_EVENTS_TX_END, DMA_INT_TX_END);
}

static void serial_dma_spim_run(struct nrf_serial_inst *s)
{
    mm_u32 tx_cnt, rx_cnt, tx_ptr, rx_ptr, i, total;
    mm_u8 orc;
    if (s == 0) return;
    if ((s->regs[ENABLE / 4u] & 0xFu) != ENABLE_SPIM) return;
    tx_cnt = s->regs[DMA_TX_MAXCNT / 4u];
    rx_cnt = s->regs[DMA_RX_MAXCNT / 4u];
    tx_ptr = s->regs[DMA_TX_PTR / 4u];
    rx_ptr = s->regs[DMA_RX_PTR / 4u];
    orc = (mm_u8)(s->regs[ORC / 4u] & 0xFFu);
    total = (tx_cnt > rx_cnt) ? tx_cnt : rx_cnt;
    serial_event_set(s, DMA_SPIM_EVENTS_STARTED, DMA_INT_SPIM_STARTED);
    serial_event_set(s, DMA_EVENTS_RX_READY, DMA_INT_RX_READY);
    serial_event_set(s, DMA_EVENTS_TX_READY, DMA_INT_TX_READY);
    for (i = 0; i < total; ++i) {
        mm_u8 out = orc;
        mm_u8 in;
        if (i < tx_cnt && !serial_dma_read8(s, tx_ptr + i, &out)) {
            s->regs[DMA_TX_BUSERRORADDRESS / 4u] = tx_ptr + i;
            s->regs[DMA_TX_AMOUNT / 4u] = i;
            s->regs[DMA_RX_AMOUNT / 4u] = (i < rx_cnt) ? i : rx_cnt;
            serial_event_set(s, DMA_EVENTS_TX_BUSERROR, DMA_INT_TX_BUSERROR);
            mm_spi_bus_end((int)s->bus_index);
            return;
        }
        in = mm_spi_bus_xfer((int)s->bus_index, out);
        if (i < rx_cnt && !serial_dma_write8(s, rx_ptr + i, in)) {
            s->regs[DMA_RX_BUSERRORADDRESS / 4u] = rx_ptr + i;
            s->regs[DMA_TX_AMOUNT / 4u] = (i < tx_cnt) ? i + 1u : tx_cnt;
            s->regs[DMA_RX_AMOUNT / 4u] = i;
            serial_event_set(s, DMA_EVENTS_RX_BUSERROR, DMA_INT_RX_BUSERROR);
            mm_spi_bus_end((int)s->bus_index);
            return;
        }
    }
    s->regs[DMA_TX_AMOUNT / 4u] = tx_cnt;
    s->regs[DMA_RX_AMOUNT / 4u] = rx_cnt;
    serial_event_set(s, DMA_EVENTS_TX_END, DMA_INT_TX_END);
    serial_event_set(s, DMA_EVENTS_RX_END, DMA_INT_RX_END);
    serial_event_set(s, DMA_SPIM_EVENTS_END, DMA_INT_SPIM_END);
    mm_spi_bus_end((int)s->bus_index);
}

static mm_bool serial_dma_write(struct nrf_serial_inst *s, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    mm_u32 mode = s->regs[ENABLE / 4u] & 0xFu;
    mm_bool trigger = (size_bytes == 4u && (value & 1u) != 0u) ? MM_TRUE : MM_FALSE;

    if (mode == ENABLE_SPIM && (offset == DMA_SPIM_TASKS_START || offset == DMA_SPIM_TASKS_STOP)) {
        if (trigger && offset == DMA_SPIM_TASKS_START) {
            serial_dma_spim_run(s);
        } else if (trigger) {
            if (s->owner->cfg->stop_spim_ends_bus) {
                mm_spi_bus_end((int)s->bus_index);
            }
            serial_event_set(s, DMA_SPIM_EVENTS_STOPPED, DMA_INT_SPIM_STOPPED);
        }
        return MM_TRUE;
    }
    if (offset == DMA_TASKS_RX_START) {
        if (trigger && mode == ENABLE_UARTE) {
            serial_uarte_ensure_open(s);
            s->rx_running = MM_TRUE;
            s->regs[DMA_RX_AMOUNT / 4u] = 0u;
            serial_event_set(s, DMA_EVENTS_RX_READY, DMA_INT_RX_READY);
            serial_dma_rx_try(s);
        }
        return MM_TRUE;
    }
    if (offset == DMA_TASKS_RX_STOP) {
        /* STOP ends with RXTO only; FLUSHRX or a full buffer raises END. */
        if (trigger && s->rx_running) {
            serial_dma_rx_try(s);
            s->rx_running = MM_FALSE;
            serial_event_set(s, DMA_EVENTS_RXTO, DMA_INT_RXTO);
        }
        return MM_TRUE;
    }
    if (offset == DMA_TASKS_TX_START) {
        if (trigger) {
            serial_dma_tx_start(s);
        }
        return MM_TRUE;
    }
    if (offset == DMA_TASKS_TX_STOP) {
        if (trigger) {
            serial_event_set(s, DMA_EVENTS_TXSTOPPED, DMA_INT_TXSTOPPED);
        }
        return MM_TRUE;
    }
    if (offset == DMA_UARTE_TASKS_FLUSHRX) {
        if (trigger && mode == ENABLE_UARTE) {
            serial_dma_flush_rx(s);
        }
        return MM_TRUE;
    }
    if (offset >= DMA_EVENTS_FIRST && offset <= DMA_EVENTS_LAST) {
        if (size_bytes == 4u && value == 0u) {
            s->regs[offset / 4u] = 0u;
        }
        return MM_TRUE;
    }
    if (offset == DMA_INTEN && size_bytes == 4u) {
        s->regs[DMA_INTENSET / 4u] = value;
        return MM_TRUE;
    }
    if (offset == DMA_INTENSET && size_bytes == 4u) {
        s->regs[DMA_INTENSET / 4u] |= value;
        return MM_TRUE;
    }
    if (offset == DMA_INTENCLR && size_bytes == 4u) {
        s->regs[DMA_INTENSET / 4u] &= ~value;
        return MM_TRUE;
    }
    if (offset == DMA_RX_AMOUNT || offset == DMA_TX_AMOUNT ||
        offset == DMA_RX_BUSERRORADDRESS || offset == DMA_TX_BUSERRORADDRESS) {
        return MM_TRUE;
    }
    return MM_FALSE;
}

static mm_bool serial_is_event_reg(const struct nrf_serial_state *state, mm_u32 offset)
{
    if (state != 0 && state->cfg != 0 && state->cfg->explicit_event_window) {
        return offset == EVENTS_RXDRDY || offset == EVENTS_ENDRX || offset == EVENTS_TXDRDY ||
               offset == EVENTS_ENDTX || offset == EVENTS_ERROR || offset == EVENTS_RXTO ||
               offset == SPIM_EVENTS_STOPPED || offset == SPIM_EVENTS_ENDRX ||
               offset == SPIM_EVENTS_END || offset == SPIM_EVENTS_ENDTX ||
               offset == SPIM_EVENTS_STARTED;
    }
    return offset >= EVENTS_CTS && offset <= EVENTS_RXTO;
}

static mm_bool serial_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct nrf_serial_inst *s = (struct nrf_serial_inst *)opaque;
    if (s == 0 || value_out == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > NRF_SERIAL_SIZE) return MM_FALSE;
    if (s->owner->cfg->dma_regs) {
        if (offset == DMA_INTEN || offset == DMA_INTENCLR) {
            offset = DMA_INTENSET;
        }
        *value_out = s->regs[offset / 4u];
        return MM_TRUE;
    }
    if (s->owner->cfg->explicit_event_window) {
        if (serial_is_event_reg(s->owner, offset) && size_bytes == 4u) {
            *value_out = s->regs[offset / 4u];
            return MM_TRUE;
        }
    } else if (size_bytes == 4u && (offset == RXD_AMOUNT || offset == TXD_AMOUNT)) {
        *value_out = s->regs[offset / 4u];
        return MM_TRUE;
    }
    *value_out = s->regs[offset / 4u];
    return MM_TRUE;
}

static mm_bool serial_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct nrf_serial_inst *s = (struct nrf_serial_inst *)opaque;
    if (s == 0 || size_bytes == 0 || size_bytes > 4u) return MM_FALSE;
    if ((offset + size_bytes) > NRF_SERIAL_SIZE) return MM_FALSE;

    if (s->owner->cfg->dma_regs && serial_dma_write(s, offset, size_bytes, value)) {
        return MM_TRUE;
    }
    if (s->owner->cfg->dma_regs && offset != ENABLE) {
        s->regs[offset / 4u] = value;
        return MM_TRUE;
    }
    if (offset == UARTE_TASKS_STARTRX && size_bytes == 4u) {
        if ((value & 1u) != 0u) {
            serial_uarte_ensure_open(s);
            s->rx_running = MM_TRUE;
            s->regs[RXD_AMOUNT / 4u] = 0u;
            serial_uarte_try_rx(s);
        }
        return MM_TRUE;
    }
    if (offset == UARTE_TASKS_STOPRX && size_bytes == 4u) {
        if ((value & 1u) != 0u) s->rx_running = MM_FALSE;
        return MM_TRUE;
    }
    if (offset == UARTE_TASKS_STARTTX && size_bytes == 4u) {
        if ((value & 1u) != 0u) {
            if (!s->owner->cfg->starttx_sets_txdrdy) {
                serial_uarte_ensure_open(s);
            }
            serial_uarte_start_tx(s);
        }
        return MM_TRUE;
    }
    if (offset == UARTE_TASKS_STOPTX && size_bytes == 4u) {
        return MM_TRUE;
    }
    if (offset == UARTE_TASKS_FLUSHRX && size_bytes == 4u) {
        if ((value & 1u) != 0u) {
            if (s->owner->cfg->flushrx_reads_uart) {
                serial_uarte_try_rx(s);
            } else {
                s->regs[RXD_AMOUNT / 4u] = 0u;
            }
        }
        return MM_TRUE;
    }
    if (offset == SPIM_TASKS_START && size_bytes == 4u) {
        if ((value & 1u) != 0u) serial_spim_run(s);
        return MM_TRUE;
    }
    if (offset == SPIM_TASKS_STOP && size_bytes == 4u) {
        if ((value & 1u) != 0u && s->owner->cfg->stop_spim_ends_bus) {
            mm_spi_bus_end((int)s->bus_index);
        }
        return MM_TRUE;
    }
    if (serial_is_event_reg(s->owner, offset) && size_bytes == 4u) {
        if (value == 0u) {
            s->regs[offset / 4u] = 0u;
        }
        return MM_TRUE;
    }
    if (offset == INTENSET && size_bytes == 4u) {
        s->regs[INTENSET / 4u] |= value;
        return MM_TRUE;
    }
    if (offset == INTENCLR && size_bytes == 4u) {
        s->regs[INTENSET / 4u] &= ~value;
        return MM_TRUE;
    }
    if (offset == ENABLE && size_bytes == 4u) {
        s->regs[offset / 4u] = value;
        if (s->has_uarte) {
            mm_u32 mode = value & 0xFu;
            if (mode == ENABLE_UARTE && s->owner->cfg->open_on_enable) {
                serial_uarte_ensure_open(s);
            } else if (mode != ENABLE_UARTE && s->io.fd >= 0) {
                mm_uart_io_close(&s->io);
            }
        }
        return MM_TRUE;
    }
    s->regs[offset / 4u] = value;
    return MM_TRUE;
}

void nrf_serial_register_all(struct nrf_serial_state *state,
                             const struct nrf_serial_config *cfg,
                             struct mmio_bus *bus,
                             struct mm_nvic *nvic)
{
    struct mmio_region reg;
    size_t i;
    if (state == 0 || cfg == 0 || bus == 0 ||
        (state->init_done && cfg->count > 0u && mmio_bus_maps(bus, cfg->bases[0]))) return;
    memset(state, 0, sizeof(*state));
    state->cfg = cfg;
    state->nvic = nvic;
    state->count = cfg->count > NRF_SERIAL_MAX_INSTANCES ? NRF_SERIAL_MAX_INSTANCES : cfg->count;
    memset(&reg, 0, sizeof(reg));
    reg.size = NRF_SERIAL_SIZE;
    reg.read = serial_read;
    reg.write = serial_write;
    for (i = 0; i < state->count; ++i) {
        struct nrf_serial_inst *s = &state->serials[i];
        s->owner = state;
        s->base = cfg->bases[i];
        s->bus_index = (mm_u32)i;
        s->irq = cfg->irqs[i];
        s->has_uarte = cfg->has_uarte[i];
        s->regs[ORC / 4u] = 0xFFu;
        if (s->has_uarte) {
            mm_uart_io_init(&s->io);
            sprintf(s->label, "UARTE%u", (unsigned)i);
            mm_uart_instance_register(s->base, s->label);
            if (cfg->open_during_init) {
                serial_uarte_ensure_open(s);
            }
        }
        reg.base = cfg->bases[i];
        reg.opaque = s;
        if (!mmio_bus_register_region(bus, &reg)) return;
        if (cfg->single_view) continue;
        reg.base = cfg->bases[i] + 0x10000000u;
        if (!mmio_bus_register_region(bus, &reg)) return;
    }
    state->init_done = MM_TRUE;
}

void nrf_serial_reset_all(struct nrf_serial_state *state)
{
    size_t i;
    if (state == 0) return;
    for (i = 0; i < state->count; ++i) {
        if (state->serials[i].has_uarte) {
            mm_uart_io_close(&state->serials[i].io);
        }
    }
    for (i = 0; i < state->count; ++i) {
        struct nrf_serial_inst *s = &state->serials[i];
        memset(s, 0, sizeof(*s));
        s->owner = state;
        s->base = state->cfg->bases[i];
        s->bus_index = (mm_u32)i;
        s->irq = state->cfg->irqs[i];
        s->has_uarte = state->cfg->has_uarte[i];
        s->regs[ORC / 4u] = 0xFFu;
        if (s->has_uarte) {
            mm_uart_io_init(&s->io);
            sprintf(s->label, "UARTE%u", (unsigned)i);
            mm_uart_instance_register(s->base, s->label);
        }
    }
}

void nrf_serial_usart_poll(struct nrf_serial_state *state)
{
    size_t i;
    if (state == 0 || !state->init_done) return;
    for (i = 0; i < state->count; ++i) {
        struct nrf_serial_inst *s = &state->serials[i];
        if (!s->has_uarte) continue;
        if (mm_uart_io_poll(&s->io)) {
            if (state->cfg->dma_regs) {
                serial_dma_rx_try(s);
            } else {
                serial_uarte_try_rx(s);
            }
        }
    }
}
