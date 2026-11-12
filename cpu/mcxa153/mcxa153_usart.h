#ifndef M33MU_MCXA153_USART_H
#define M33MU_MCXA153_USART_H

struct mmio_bus;
struct mm_nvic;

void mm_mcxa153_usart_init(struct mmio_bus *bus, struct mm_nvic *nvic);
void mm_mcxa153_usart_poll(void);
void mm_mcxa153_usart_reset(void);

#endif /* M33MU_MCXA153_USART_H */
