/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "m33mu/memmap.h"
#include "m33mu/mem_prot.h"
#include "m33mu/mmio.h"
#include "m33mu/scs.h"
#include "m33mu/cpu_db.h"
#include "m33mu/optbytes.h"
#include "m33mu/target_hal.h"
#include "stm32l552/stm32l552_mmio.h"

#define FLASH_SIZE_L5      0x80000u
#define RAM_SIZE_L5        0x40000u

#define MPCBB1_NS          0x40032C00u
#define MPCBB1_S           0x50032C00u
#define MPCBB2_S           0x50033000u
#define MPCBB_CR           0x000u
#define MPCBB_LCKVTR1      0x010u
#define MPCBB_VCTR(n)      (0x100u + 4u * (n))

#define FLASH_REG_NS       0x40022000u
#define FLASH_REG_S        0x50022000u
#define FLASH_NSKEYR       0x008u
#define FLASH_NSCR         0x028u
#define FLASH_OPTR         0x040u
#define FLASH_SECWM1R1     0x050u
#define FLASH_SECWM2R1     0x060u
#define FLASH_SECBB2R1     0x0A0u

#define TZSC_S             0x50032400u
#define TZSC_SECCFGR2      0x014u
#define RNG_NS             0x420C0800u
#define RNG_S              0x520C0800u
#define HASH_NS            0x420C0400u

#define OPTR_EXPECTED      0x9F4F70AAu
#define OPTR_TZEN          (1u << 31)

#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

struct l5_rig {
    struct mm_memmap map;
    struct mmio_region regions[256];
    struct mm_target_cfg cfg;
    struct mm_scs scs;
    struct mm_prot_ctx prot;
};

static mm_u8 g_flash[FLASH_SIZE_L5];
static mm_u8 g_ram[RAM_SIZE_L5];
static struct l5_rig g_rig;

static enum mm_sec_state rig_bus_attr(void *opaque, mm_u32 addr)
{
    return mm_prot_bus_attr_for_addr((const struct mm_prot_ctx *)opaque, addr);
}

static int rig_init(struct l5_rig *r, mm_bool tz)
{
    mm_u32 i;

    memset(r, 0, sizeof(*r));
    memset(g_ram, 0, sizeof(g_ram));
    if (!mm_cpu_lookup("stm32l552", &r->cfg)) {
        printf("FAIL: no stm32l552 in the cpu table\n");
        return 1;
    }
    if (!tz) {
        r->cfg.tz_attr_for_addr = 0;
    }
    mm_memmap_init(&r->map, r->regions, sizeof(r->regions) / sizeof(r->regions[0]));
    if (!mm_memmap_configure_flash(&r->map, &r->cfg, g_flash, MM_TRUE) ||
        !mm_memmap_configure_ram(&r->map, &r->cfg, g_ram, MM_TRUE)) {
        printf("FAIL: memory map\n");
        return 1;
    }
    mm_stm32l552_mmio_reset();
    if (!mm_target_register_mmio(&r->cfg, &r->map.mmio)) {
        printf("FAIL: MMIO registration\n");
        return 1;
    }
    mm_target_flash_bind(&r->cfg, &r->map, g_flash, FLASH_SIZE_L5, 0);

    mm_scs_init(&r->scs, 0);
    /* SAU as a TrustZone runtime leaves it: flash, SRAM and peripherals NS. */
    r->scs.sau_ctrl = 0x1u;
    r->scs.sau_rbar[0] = 0x08000000u;
    r->scs.sau_rlar[0] = 0x0807FFE0u | 0x1u;
    r->scs.sau_rbar[1] = 0x20000000u;
    r->scs.sau_rlar[1] = 0x2003FFE0u | 0x1u;
    r->scs.sau_rbar[2] = 0x40000000u;
    r->scs.sau_rlar[2] = 0x4FFFFFE0u | 0x1u;

    mm_prot_init(&r->prot, &r->scs, &r->cfg, 0);
    mm_memmap_set_interceptor(&r->map, mm_prot_interceptor, &r->prot);
    mm_memmap_set_bus_attr(&r->map, rig_bus_attr, &r->prot);
    mm_prot_add_region(&r->prot, r->cfg.flash_base_s, r->cfg.flash_size_s,
                       MM_PROT_PERM_READ | MM_PROT_PERM_WRITE | MM_PROT_PERM_EXEC, MM_SECURE);
    mm_prot_add_region(&r->prot, r->cfg.flash_base_ns, r->cfg.flash_size_ns,
                       MM_PROT_PERM_READ | MM_PROT_PERM_WRITE | MM_PROT_PERM_EXEC, MM_NONSECURE);
    for (i = 0; i < r->cfg.ram_region_count; ++i) {
        const struct mm_ram_region *rr = &r->cfg.ram_regions[i];
        mm_prot_add_region(&r->prot, rr->base_s, rr->size,
                           MM_PROT_PERM_READ | MM_PROT_PERM_WRITE | MM_PROT_PERM_EXEC, MM_SECURE);
        mm_prot_add_region(&r->prot, rr->base_ns, rr->size,
                           MM_PROT_PERM_READ | MM_PROT_PERM_WRITE | MM_PROT_PERM_EXEC, MM_NONSECURE);
    }
    mm_prot_add_region(&r->prot, 0x40000000u, 0x20000000u,
                       MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_SECURE);
    mm_prot_add_region(&r->prot, 0x40000000u, 0x20000000u,
                       MM_PROT_PERM_READ | MM_PROT_PERM_WRITE, MM_NONSECURE);
    return 0;
}

static void clear_faults(struct l5_rig *r)
{
    r->scs.securefault_pending = MM_FALSE;
    r->scs.sau_sfsr = 0u;
    r->scs.sau_sfar = 0u;
    r->scs.cfsr_ns = 0u;
    r->scs.cfsr_s = 0u;
    r->scs.mmfar_ns = 0u;
    r->scs.mmfar_s = 0u;
}

/* A NS access the model refuses with a SecureFault reporting `addr`. */
static int ns_faults(struct l5_rig *r, mm_bool write, mm_u32 addr)
{
    mm_u32 v = 0u;
    mm_bool ok;

    clear_faults(r);
    ok = write ? mm_memmap_write(&r->map, MM_NONSECURE, addr, 4u, 0x5A5A5A5Au)
               : mm_memmap_read(&r->map, MM_NONSECURE, addr, 4u, &v);
    if (ok) {
        printf("NS %s 0x%08lx was allowed\n", write ? "write" : "read", (unsigned long)addr);
        return 0;
    }
    if (!r->scs.securefault_pending || r->scs.sau_sfar != addr ||
        (r->scs.sau_sfsr & (1u << 3)) == 0u) {
        printf("NS access 0x%08lx refused without AUVIOL SecureFault\n", (unsigned long)addr);
        return 0;
    }
    clear_faults(r);
    return 1;
}

static int ns_ok(struct l5_rig *r, mm_bool write, mm_u32 addr)
{
    mm_u32 v = 0u;
    mm_bool ok;

    clear_faults(r);
    ok = write ? mm_memmap_write(&r->map, MM_NONSECURE, addr, 4u, 0x5A5A5A5Au)
               : mm_memmap_read(&r->map, MM_NONSECURE, addr, 4u, &v);
    return (ok && !r->scs.securefault_pending) ? 1 : 0;
}

static mm_u32 rd(struct l5_rig *r, enum mm_sec_state sec, mm_u32 addr)
{
    mm_u32 v = 0xDEADBEEFu;
    if (!mm_memmap_read(&r->map, sec, addr, 4u, &v)) {
        printf("read 0x%08lx failed\n", (unsigned long)addr);
    }
    return v;
}

static mm_bool wr(struct l5_rig *r, enum mm_sec_state sec, mm_u32 addr, mm_u32 value)
{
    return mm_memmap_write(&r->map, sec, addr, 4u, value);
}

/* Runs first: the option bytes are loaded once per process, as at power-on. */
static int test_optr_env_override(void)
{
    struct l5_rig *r = &g_rig;

    setenv("M33MU_STM32L5_OPTR", "0x1F4F70BB", 1);
    if (rig_init(r, MM_TRUE)) return 1;
    unsetenv("M33MU_STM32L5_OPTR");
    /* TZEN=0 and RDP level 1, as provisioned; DBANK follows the geometry. */
    CHECK(rd(r, MM_SECURE, FLASH_REG_S + FLASH_OPTR) == 0x1F4F70BBu);
    /* TZEN=0: the flash filter and TZSC enforcement are off. */
    CHECK(wr(r, MM_SECURE, TZSC_S + TZSC_SECCFGR2, 1u << 14));
    CHECK(ns_ok(r, MM_FALSE, RNG_NS));
    CHECK(rd(r, MM_NONSECURE, 0x08000000u) == 0x11111111u);

    CHECK(mm_optbytes_set("OPTR", OPTR_EXPECTED));
    CHECK(rd(r, MM_SECURE, FLASH_REG_S + FLASH_OPTR) == OPTR_EXPECTED);
    return 0;
}

static int test_optr_reset_and_tzen_sticky(void)
{
    struct l5_rig *r = &g_rig;
    mm_u32 v;

    if (rig_init(r, MM_TRUE)) return 1;
    CHECK(rd(r, MM_SECURE, FLASH_REG_S + FLASH_OPTR) == OPTR_EXPECTED);
    CHECK(rd(r, MM_NONSECURE, FLASH_REG_NS + FLASH_OPTR) == OPTR_EXPECTED);
    /* Firmware cannot clear TZEN; other fields are stored. */
    CHECK(wr(r, MM_SECURE, FLASH_REG_S + FLASH_OPTR, OPTR_EXPECTED & ~OPTR_TZEN & ~0xFFu));
    v = rd(r, MM_SECURE, FLASH_REG_S + FLASH_OPTR);
    CHECK((v & OPTR_TZEN) != 0u);
    CHECK((v & 0xFFu) == 0u);
    /* Option bytes survive a system reset. */
    if (rig_init(r, MM_TRUE)) return 1;
    CHECK(rd(r, MM_SECURE, FLASH_REG_S + FLASH_OPTR) == v);
    CHECK(mm_optbytes_set("OPTR", OPTR_EXPECTED));

    /* A session with TrustZone disabled reads TZEN=0. */
    if (rig_init(r, MM_FALSE)) return 1;
    CHECK(rd(r, MM_SECURE, FLASH_REG_S + FLASH_OPTR) == (OPTR_EXPECTED & ~OPTR_TZEN));
    return 0;
}

static int test_mpcbb_enforced(void)
{
    struct l5_rig *r = &g_rig;

    if (rig_init(r, MM_TRUE)) return 1;
    /* Out of reset every block of both SRAMs is Secure. */
    CHECK(ns_faults(r, MM_TRUE, 0x20000000u));
    CHECK(ns_faults(r, MM_FALSE, 0x2002FF00u));
    CHECK(ns_faults(r, MM_TRUE, 0x20030000u));
    CHECK(ns_faults(r, MM_TRUE, 0x2003FFFCu));
    CHECK(rd(r, MM_SECURE, MPCBB1_S + MPCBB_VCTR(23)) == 0xFFFFFFFFu);
    CHECK(rd(r, MM_SECURE, MPCBB2_S + MPCBB_VCTR(7)) == 0xFFFFFFFFu);

    /* VCTR0 covers 32 blocks of 256 bytes: 0x20000000..0x20001FFF. */
    CHECK(wr(r, MM_SECURE, MPCBB1_S + MPCBB_VCTR(0), 0u));
    CHECK(ns_ok(r, MM_TRUE, 0x20000000u));
    CHECK(ns_ok(r, MM_TRUE, 0x20001FFCu));
    CHECK(ns_faults(r, MM_TRUE, 0x20002000u));
    /* One block back to Secure. */
    CHECK(wr(r, MM_SECURE, MPCBB1_S + MPCBB_VCTR(0), 1u << 1));
    CHECK(ns_ok(r, MM_TRUE, 0x200000FCu));
    CHECK(ns_faults(r, MM_TRUE, 0x20000100u));
    CHECK(ns_ok(r, MM_TRUE, 0x20000200u));
    /* SRAM2 block 255 through MPCBB2 VCTR7 bit 31. */
    CHECK(wr(r, MM_SECURE, MPCBB2_S + MPCBB_VCTR(7), 0x7FFFFFFFu));
    CHECK(ns_ok(r, MM_TRUE, 0x2003FF00u));
    CHECK(ns_faults(r, MM_TRUE, 0x2003FE00u));

    /* Non-secure software, or the NS alias, cannot reprogram the MPCBB. */
    mmio_set_active_sec(MM_NONSECURE);
    CHECK(mmio_bus_write(&r->map.mmio, MPCBB1_NS + MPCBB_VCTR(1), 4u, 0u));
    CHECK(mmio_bus_write(&r->map.mmio, MPCBB1_S + MPCBB_VCTR(1), 4u, 0u));
    mmio_set_active_sec(MM_SECURE);
    CHECK(mmio_bus_write(&r->map.mmio, MPCBB1_NS + MPCBB_VCTR(1), 4u, 0u));
    CHECK(rd(r, MM_SECURE, MPCBB1_S + MPCBB_VCTR(1)) == 0xFFFFFFFFu);
    CHECK(ns_faults(r, MM_TRUE, 0x20002000u));
    return 0;
}

static int test_mpcbb_locks(void)
{
    struct l5_rig *r = &g_rig;

    if (rig_init(r, MM_TRUE)) return 1;
    /* Superblock 2 locked: VCTR2 keeps its value, the lock cannot be undone. */
    CHECK(wr(r, MM_SECURE, MPCBB1_S + MPCBB_LCKVTR1, 1u << 2));
    CHECK(wr(r, MM_SECURE, MPCBB1_S + MPCBB_VCTR(2), 0u));
    CHECK(rd(r, MM_SECURE, MPCBB1_S + MPCBB_VCTR(2)) == 0xFFFFFFFFu);
    CHECK(wr(r, MM_SECURE, MPCBB1_S + MPCBB_LCKVTR1, 0u));
    CHECK(rd(r, MM_SECURE, MPCBB1_S + MPCBB_LCKVTR1) == (1u << 2));
    CHECK(wr(r, MM_SECURE, MPCBB1_S + MPCBB_VCTR(3), 0u));
    CHECK(rd(r, MM_SECURE, MPCBB1_S + MPCBB_VCTR(3)) == 0u);
    /* CR.LCK freezes CR and the lock registers. */
    CHECK(wr(r, MM_SECURE, MPCBB1_S + MPCBB_CR, 1u));
    CHECK(wr(r, MM_SECURE, MPCBB1_S + MPCBB_CR, 0u));
    CHECK(rd(r, MM_SECURE, MPCBB1_S + MPCBB_CR) == 1u);
    CHECK(wr(r, MM_SECURE, MPCBB1_S + MPCBB_LCKVTR1, 1u << 4));
    CHECK(rd(r, MM_SECURE, MPCBB1_S + MPCBB_LCKVTR1) == (1u << 2));
    CHECK(wr(r, MM_SECURE, MPCBB1_S + MPCBB_VCTR(4), 0u));
    CHECK(rd(r, MM_SECURE, MPCBB1_S + MPCBB_VCTR(4)) == 0u);
    /* A reset clears the locks and makes every block Secure again. */
    mm_stm32l552_mmio_reset();
    CHECK(rd(r, MM_SECURE, MPCBB1_S + MPCBB_CR) == 0u);
    CHECK(rd(r, MM_SECURE, MPCBB1_S + MPCBB_LCKVTR1) == 0u);
    CHECK(rd(r, MM_SECURE, MPCBB1_S + MPCBB_VCTR(3)) == 0xFFFFFFFFu);
    return 0;
}

static int test_flash_secwm_secbb(void)
{
    struct l5_rig *r = &g_rig;
    mm_u32 cr;

    if (rig_init(r, MM_TRUE)) return 1;
    /* Bank 1 entirely Secure, bank 2 Non-secure out of reset. */
    CHECK(rd(r, MM_SECURE, FLASH_REG_S + FLASH_SECWM1R1) == 0x007F0000u);
    CHECK(rd(r, MM_SECURE, FLASH_REG_S + FLASH_SECWM2R1) == 0x0000007Fu);
    CHECK(rd(r, MM_NONSECURE, 0x08000000u) == 0u);
    CHECK(rd(r, MM_NONSECURE, 0x0803F800u) == 0u);
    CHECK(rd(r, MM_NONSECURE, 0x08040000u) == 0x22222222u);
    CHECK(rd(r, MM_SECURE, 0x0C000000u) == 0x11111111u);
    /* Secure software reads the non-secure bank (wolfBoot's update path). */
    CHECK(rd(r, MM_SECURE, 0x0C040000u) == 0x22222222u);
    CHECK(rd(r, MM_SECURE, 0x0805F800u) == 0x33333333u);

    /* SECBB2R1 bit 0 claims bank 2 page 0; NS software cannot touch SECBB. */
    mmio_set_active_sec(MM_NONSECURE);
    CHECK(mmio_bus_write(&r->map.mmio, FLASH_REG_NS + FLASH_SECBB2R1, 4u, 1u));
    CHECK(rd(r, MM_NONSECURE, 0x08040000u) == 0x22222222u);
    CHECK(wr(r, MM_SECURE, FLASH_REG_S + FLASH_SECBB2R1, 1u));
    CHECK(rd(r, MM_NONSECURE, 0x08040000u) == 0u);
    CHECK(rd(r, MM_NONSECURE, 0x08040800u) == 0x44444444u);
    CHECK(wr(r, MM_SECURE, FLASH_REG_S + FLASH_SECBB2R1, 0u));
    CHECK(rd(r, MM_NONSECURE, 0x08040000u) == 0x22222222u);

    /* Watermark 2 over pages 0..1 of bank 2; NS cannot reprogram it. */
    mmio_set_active_sec(MM_NONSECURE);
    CHECK(mmio_bus_write(&r->map.mmio, FLASH_REG_NS + FLASH_SECWM2R1, 4u, 0x00010000u));
    CHECK(rd(r, MM_SECURE, FLASH_REG_S + FLASH_SECWM2R1) == 0x0000007Fu);
    CHECK(wr(r, MM_SECURE, FLASH_REG_S + FLASH_SECWM2R1, 0x00010000u));
    CHECK(rd(r, MM_NONSECURE, 0x08040800u) == 0u);
    CHECK(rd(r, MM_NONSECURE, 0x08041000u) == 0xFFFFFFFFu);
    CHECK(mm_optbytes_set("SECWM2", 0x0000007Fu));

    /* NS programming: a secure page ignores it, a non-secure page takes it. */
    CHECK(wr(r, MM_NONSECURE, FLASH_REG_NS + FLASH_NSKEYR, 0x45670123u));
    CHECK(wr(r, MM_NONSECURE, FLASH_REG_NS + FLASH_NSKEYR, 0xCDEF89ABu));
    CHECK(wr(r, MM_NONSECURE, FLASH_REG_NS + FLASH_NSCR, 1u));
    CHECK(wr(r, MM_NONSECURE, 0x08000100u, 0u));
    CHECK(g_flash[0x100] == 0x11u);
    CHECK(wr(r, MM_NONSECURE, 0x08041000u, 0x12345678u));
    CHECK(rd(r, MM_NONSECURE, 0x08041000u) == 0x12345678u);
    /* NS page erase of bank 1 page 0 (secure): nothing erased. */
    cr = (1u << 1) | (1u << 16);
    CHECK(wr(r, MM_NONSECURE, FLASH_REG_NS + FLASH_NSCR, cr));
    CHECK(g_flash[0] == 0x11u);
    /* NS page erase of bank 2 page 2 (BKER): erased. */
    cr = (1u << 1) | (2u << 3) | (1u << 11) | (1u << 16);
    CHECK(wr(r, MM_NONSECURE, FLASH_REG_NS + FLASH_NSCR, cr));
    CHECK(rd(r, MM_NONSECURE, 0x08041000u) == 0xFFFFFFFFu);

    /* --secwm1=127:0 provisions bank 1 Non-secure. */
    mm_stm32l552_flash_set_secwm(0, 0x7Fu, 0u);
    CHECK(rd(r, MM_NONSECURE, 0x08000000u) == 0x11111111u);
    mm_stm32l552_flash_set_secwm(0, 0u, 0x7Fu);
    CHECK(rd(r, MM_NONSECURE, 0x08000000u) == 0u);

    /* TrustZone disabled for the session: no filter. */
    if (rig_init(r, MM_FALSE)) return 1;
    CHECK(rd(r, MM_NONSECURE, 0x08000000u) == 0x11111111u);
    return 0;
}

static int test_tzsc_rng(void)
{
    struct l5_rig *r = &g_rig;

    if (rig_init(r, MM_TRUE)) return 1;
    /* TZSC resets with every peripheral Non-secure. */
    CHECK(ns_ok(r, MM_FALSE, RNG_NS));
    CHECK(wr(r, MM_SECURE, TZSC_S + TZSC_SECCFGR2, 1u << 14));
    CHECK(ns_faults(r, MM_FALSE, RNG_NS));
    CHECK(ns_faults(r, MM_FALSE, RNG_NS + 8u));
    CHECK(ns_faults(r, MM_TRUE, RNG_NS));
    CHECK(ns_ok(r, MM_FALSE, HASH_NS));
    /* Secure software still reaches it through either alias. */
    clear_faults(r);
    (void)rd(r, MM_SECURE, RNG_S);
    (void)rd(r, MM_SECURE, RNG_NS);
    CHECK(!r->scs.securefault_pending);
    CHECK(wr(r, MM_SECURE, TZSC_S + TZSC_SECCFGR2, 1u << 13));
    CHECK(ns_ok(r, MM_FALSE, RNG_NS));
    CHECK(ns_faults(r, MM_FALSE, HASH_NS));
    return 0;
}

int main(void)
{
    int failures = 0;

    memset(g_flash, 0xFF, sizeof(g_flash));
    memset(g_flash, 0x11, 0x800u);
    memset(g_flash + 0x3F800u, 0x55, 0x800u);
    memset(g_flash + 0x40000u, 0x22, 0x800u);
    memset(g_flash + 0x40800u, 0x44, 0x800u);
    memset(g_flash + 0x5F800u, 0x33, 0x800u);

    failures += test_optr_env_override();
    failures += test_optr_reset_and_tzen_sticky();
    failures += test_mpcbb_enforced();
    failures += test_mpcbb_locks();
    failures += test_flash_secwm_secbb();
    failures += test_tzsc_rng();
    if (failures != 0) {
        printf("stm32l552_security_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("stm32l552_security_test: ok\n");
    return 0;
}
