/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/* MCXW716C model: FMU0 command handshake, flash stores gated on PEWEN, the
 * array unreadable mid-command, --persist write-through, and the SPC/MCM
 * status bits the SDK clock code and wolfBoot HAL poll. */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "m33mu/mmio.h"
#include "m33mu/memmap.h"
#include "m33mu/target.h"
#include "m33mu/flash_persist.h"
#include "mcxw71c/cpu_config.h"
#include "mcxw71c/mcxw71c_mmio.h"

#define FLASH_SIZE   0x20000u
#define FMU_FSTAT    0x50020000u
#define FMU_FCCOB0   0x50020010u
#define FMU_FCCOB2   0x50020018u
#define SPC_SC       0x40016010u
#define SPC_SRAMCTL  0x40016040u
#define MCM_CPCR2    0xE0080034u

#define CCIF   0x00000080u
#define ACCERR 0x00000020u
#define FAIL   0x00000001u
#define PEWEN1 0x01000000u
#define PERDY  0x80000000u

static struct mm_memmap map;
static struct mmio_region regions[64];
static mm_u8 flash[FLASH_SIZE];
static mm_u8 ram[MCXW71C_RAM_SIZE];
static struct mm_target_cfg cfg;
static struct mm_flash_persist persist;

static int failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            failures++;                                                   \
        }                                                                 \
    } while (0)

static mm_u32 rd(mm_u32 addr)
{
    mm_u32 v = 0xDEADBEEFu;
    (void)mmio_bus_read(&map.mmio, addr, 4u, &v);
    return v;
}

static void wr(mm_u32 addr, mm_u32 v)
{
    (void)mmio_bus_write(&map.mmio, addr, 4u, v);
}

static mm_bool flash_store(mm_u32 addr, mm_u32 v)
{
    return mm_memmap_write(&map, MM_SECURE, addr, 4u, v);
}

static mm_bool flash_load(mm_u32 addr, mm_u32 *v)
{
    return mm_memmap_read(&map, MM_SECURE, addr, 4u, v);
}

static void setup(const struct mm_flash_persist *p)
{
    memset(&cfg, 0, sizeof(cfg));
    memset(flash, 0xFF, sizeof(flash));
    cfg.flash_base_s = MCXW71C_FLASH_BASE_S;
    cfg.flash_size_s = sizeof(flash);
    cfg.flash_base_ns = MCXW71C_FLASH_BASE_NS;
    cfg.flash_size_ns = sizeof(flash);
    cfg.ram_base_s = MCXW71C_RAM_BASE_S;
    cfg.ram_size_s = MCXW71C_RAM_SIZE;
    cfg.ram_base_ns = MCXW71C_RAM_BASE_NS;
    cfg.ram_size_ns = MCXW71C_RAM_SIZE;
    mm_memmap_init(&map, regions, sizeof(regions) / sizeof(regions[0]));
    (void)mm_memmap_configure_flash(&map, &cfg, flash, MM_TRUE);
    (void)mm_memmap_configure_ram(&map, &cfg, ram, MM_TRUE);
    mm_mcxw71c_mmio_reset();
    if (!mm_mcxw71c_register_mmio(&map.mmio)) {
        printf("FAIL: register_mmio\n");
        failures++;
    }
    mm_mcxw71c_flash_bind(&map, flash, sizeof(flash), p, 0u);
    mmio_set_active_sec(MM_SECURE);
}

static void launch(mm_u32 cmd)
{
    wr(FMU_FSTAT, 0x00010074u);
    wr(FMU_FCCOB0, cmd);
    wr(FMU_FSTAT, CCIF);
}

static void program_phrase(mm_u32 addr, const mm_u32 *w)
{
    mm_u32 i;
    launch(0x24u);
    for (i = 0; i < 4u; i++) {
        (void)flash_store(addr + 4u * i, w[i]);
    }
    wr(FMU_FSTAT, PERDY);
}

static void test_program(void)
{
    static const mm_u32 w[4] = { 0x11223344u, 0x55667788u, 0x99AABBCCu, 0xDDEEFF00u };
    static const mm_u32 z[4] = { 0x0000FFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
    mm_u32 v = 0;

    CHECK(rd(FMU_FSTAT) == CCIF);
    /* Without a launched command the array is read-only. */
    CHECK(!flash_store(0x10001000u, 0x0u));
    CHECK(flash[0x1000] == 0xFFu);

    launch(0x24u);
    CHECK(rd(FMU_FSTAT) == PEWEN1);
    /* No read-while-write: the array faults while the command is open. */
    CHECK(!flash_load(0x00001000u, &v));
    CHECK(flash_store(0x10001000u, w[0]));
    CHECK(flash_store(0x10001004u, w[1]));
    CHECK(flash_store(0x10001008u, w[2]));
    CHECK((rd(FMU_FSTAT) & PERDY) == 0u);
    CHECK(flash_store(0x1000100Cu, w[3]));
    CHECK(rd(FMU_FSTAT) == PERDY);
    CHECK(flash[0x1000] == 0xFFu);
    wr(FMU_FSTAT, PERDY);
    CHECK(rd(FMU_FSTAT) == CCIF);
    CHECK(flash_load(0x00001000u, &v) && v == w[0]);
    CHECK(flash_load(0x1000100Cu, &v) && v == w[3]);

    /* Programming only clears bits. */
    program_phrase(0x00001000u, z);
    CHECK(flash_load(0x00001000u, &v) && v == (w[0] & z[0]));

    /* Verify-erase phrase reports FAIL on programmed data. */
    wr(FMU_FCCOB2, 0x00001000u);
    launch(0x04u);
    CHECK(rd(FMU_FSTAT) == (CCIF | FAIL));
    wr(FMU_FCCOB2, 0x00001010u);
    launch(0x04u);
    CHECK(rd(FMU_FSTAT) == CCIF);
}

static void test_erase(void)
{
    mm_u32 i;
    mm_u32 v = 0;
    int ok = 1;

    memset(flash + 0x2000, 0x00, 0x4000);
    launch(0x42u);
    for (i = 0; i < 4u; i++) {
        (void)flash_store(0x00002FF0u + 4u * i, 0u);
    }
    wr(FMU_FSTAT, PERDY);
    CHECK(rd(FMU_FSTAT) == CCIF);
    for (i = 0x2000u; i < 0x4000u; i++) {
        ok &= flash[i] == 0xFFu;
    }
    CHECK(ok);
    CHECK(flash[0x4000] == 0x00u);

    launch(0x40u);
    CHECK(rd(FMU_FSTAT) == CCIF);
    CHECK(flash_load(0x10004000u, &v) && v == 0xFFFFFFFFu);
}

static void test_errors(void)
{
    mm_u32 v = 0;

    launch(0x7Fu);
    CHECK(rd(FMU_FSTAT) == (CCIF | ACCERR));
    /* Launch ignored while ACCERR is pending. */
    wr(FMU_FCCOB0, 0x42u);
    wr(FMU_FSTAT, CCIF);
    CHECK(rd(FMU_FSTAT) == (CCIF | ACCERR));
    wr(FMU_FSTAT, ACCERR);
    CHECK(rd(FMU_FSTAT) == CCIF);

    /* Byte store and a store outside the phrase abort the command. */
    launch(0x24u);
    CHECK(mm_memmap_write(&map, MM_SECURE, 0x10003000u, 1u, 0u));
    CHECK(rd(FMU_FSTAT) == (CCIF | ACCERR));
    wr(FMU_FSTAT, ACCERR);
    launch(0x24u);
    CHECK(flash_store(0x10003000u, 0u));
    CHECK(flash_store(0x10003010u, 0u));
    CHECK(rd(FMU_FSTAT) == (CCIF | ACCERR));
    CHECK(flash_load(0x00003000u, &v) && v == 0xFFFFFFFFu);
    wr(FMU_FSTAT, ACCERR);

    /* FCCOB is frozen while a command is open. */
    launch(0x24u);
    wr(FMU_FCCOB0, 0x42u);
    CHECK(rd(FMU_FCCOB0) == 0x24u);
}

static void test_persist(void)
{
    static const mm_u32 w[4] = { 0xA1A2A3A4u, 0xB1B2B3B4u, 0xC1C2C3C4u, 0xD1D2D3D4u };
    char path[] = "/tmp/m33mu-mcxw71c-fmu-XXXXXX";
    const char *paths[1];
    mm_u32 offsets[1] = { 0u };
    unsigned char buf[16];
    FILE *f;
    int fd;

    fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd < 0) {
        return;
    }
    close(fd);
    paths[0] = path;
    setup(0);
    mm_flash_persist_build(&persist, flash, sizeof(flash), paths, offsets, 1);
    mm_mcxw71c_flash_bind(&map, flash, sizeof(flash), &persist, 0u);
    program_phrase(0x10000400u, w);
    f = fopen(path, "rb");
    CHECK(f != 0);
    if (f != 0) {
        CHECK(fseek(f, 0x400, SEEK_SET) == 0);
        CHECK(fread(buf, 1, sizeof(buf), f) == sizeof(buf));
        CHECK(memcmp(buf, w, sizeof(buf)) == 0);
        fclose(f);
    }
    unlink(path);
}

static void test_power(void)
{
    CHECK((rd(SPC_SC) & 1u) == 0u);
    wr(SPC_SC, 1u);
    CHECK((rd(SPC_SC) & 1u) == 0u);
    wr(SPC_SRAMCTL, 0x40000002u);
    CHECK(rd(SPC_SRAMCTL) == 0xC0000002u);
    wr(SPC_SRAMCTL, 0x00000002u);
    CHECK(rd(SPC_SRAMCTL) == 0x00000002u);
    wr(MCM_CPCR2, 0x1u);
    CHECK(rd(MCM_CPCR2) == 0u);
}

int main(void)
{
    setup(0);
    test_program();
    test_erase();
    test_errors();
    test_power();
    test_persist();
    if (failures != 0) {
        printf("mcxw71c_fmu_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("mcxw71c_fmu_test: PASS\n");
    return 0;
}
