#ifndef M33MU_NRF5340NET_UART_SPI_H
#define M33MU_NRF5340NET_UART_SPI_H

#include "m33mu/mmio.h"
#include "m33mu/nvic.h"

void mm_nrf5340net_usart_init(struct mmio_bus *bus, struct mm_nvic *nvic);
void mm_nrf5340net_usart_reset(void);
void mm_nrf5340net_usart_poll(void);

void mm_nrf5340net_spi_init(struct mmio_bus *bus, struct mm_nvic *nvic);
void mm_nrf5340net_spi_reset(void);
void mm_nrf5340net_spi_poll(void);

#endif /* M33MU_NRF5340NET_UART_SPI_H */
