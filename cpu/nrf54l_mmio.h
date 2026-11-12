/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef M33MU_NRF54L_MMIO_H
#define M33MU_NRF54L_MMIO_H

#include "m33mu/mmio.h"
#include "m33mu/nvic.h"
#include "m33mu/sau.h"
#include "m33mu/types.h"

struct mm_memmap;
struct mm_flash_persist;

/* RRAM and RAM have no secure alias: SPU and MPC00 decide the security of
 * the one address, peripherals answer at 0x4xxxxxxx (NS) / 0x5xxxxxxx (S). */
#define NRF54L_FLASH_BASE 0x00000000u
#define NRF54L_RAM_BASE   0x20000000u

#define NRF54L_GPIO_MAX         4
#define NRF54L_MPC_OVERRIDE_MAX 12u

/* SPU FEATURE words a part implements, as offsets in the SPU block. */
struct nrf54l_spu_feature {
    mm_u32 offset;
    mm_u32 count;
};

/* Everything that differs between the nRF54L parts sharing nrf54l_mmio.c.
 * The per-part descriptors under cpu/nrf54l15 and cpu/nrf54lm20 initialise
 * this positionally, so add new fields to every descriptor. */
struct nrf54l_mmio_variant {
    /* Secure-only RRAMC, CRACEN wrapper, CRACENCORE and TAMPC bases. */
    mm_u32 rramc_base;
    mm_u32 cracen_base;
    mm_u32 cracencore_base;
    mm_u32 tampc_base;

    /* GPIO ports present, counting from P0. */
    int gpio_count;

    /* MPC00 OVERRIDE[n] regions implemented, and the one that blocks all
     * access to its range instead of applying PERM. */
    mm_u32 mpc_override_count;
    mm_u32 mpc_block_override;

    /* RAM at NRF54L_RAM_BASE, in bytes. */
    mm_u32 ram_size;

    /* FICR INFO.RAM and INFO.RRAM, in KB. */
    mm_u32 ficr_info_ram;
    mm_u32 ficr_info_rram;

    /* CLOCK_POWER interrupt number. */
    mm_u32 clock_irq;

    /* Implemented SPU FEATURE entries (DPPIC, GPIOTE, GPIO pins, GRTC). */
    const struct nrf54l_spu_feature *spu_features;
    mm_u32 spu_feature_count;
};

/* Entry points. register_mmio, mmio_reset and flash_bind each latch the
 * variant, so they may be called in any order. flash_bind also installs
 * MPC00 as the memory access filter for RRAM and RAM. */
mm_bool nrf54l_register_mmio(const struct nrf54l_mmio_variant *v, struct mmio_bus *bus);
void nrf54l_mmio_reset(const struct nrf54l_mmio_variant *v);
void nrf54l_flash_bind(const struct nrf54l_mmio_variant *v,
                       struct mm_memmap *map,
                       mm_u8 *flash,
                       mm_u32 flash_size,
                       const struct mm_flash_persist *persist,
                       mm_u32 flags);

mm_u64 nrf54l_cpu_hz(void);
void nrf54l_mmio_set_nvic(struct mm_nvic *nvic);
void nrf54l_grtc_tick(mm_u64 cycles);

/* IDAU for the peripheral windows: 0x5xxxxxxx is Secure, and a peripheral
 * whose SPU PERIPH[n].PERM.SECATTR is Secure does not answer at 0x4xxxxxxx.
 * RRAM and RAM are left to the SAU (MPC00 filters them separately). */
mm_bool nrf54l_tz_attr_for_addr(mm_u32 addr,
                                enum mm_sau_attr *attr_out,
                                mm_u32 *region_out);

/* Security of the EasyDMA transactions of the peripheral at `base_ns`:
 * non-secure once the SPU makes the peripheral or its DMA non-secure. */
enum mm_sec_state nrf54l_dma_sec(mm_u32 base_ns);

/* TIMER00/10/20..24 and the GRTC, identical on every nRF54L part. */
void nrf54l_timers_init(struct mmio_bus *bus, struct mm_nvic *nvic);
void nrf54l_timers_reset(void);
void nrf54l_timers_tick(mm_u64 cycles);

#endif /* M33MU_NRF54L_MMIO_H */
