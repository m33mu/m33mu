/* m33mu -- an ARMv8-M Emulator
 *
 * Copyright (C) 2025  Daniele Lacamera <root@danielinux.net>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include "m33mu/execute.h"
#include "m33mu/nvic.h"
#include "m33mu/vector.h"

static mm_u32 g_last_exc_num;
static mm_u32 g_last_return_pc;
static int g_enter_exception_calls;
static mm_u32 g_last_pc_write_value;
static int g_handle_pc_write_calls;
static mm_u32 g_last_ufsr_bits;
static int g_raise_usage_fault_calls;

static mm_bool stub_handle_pc_write(struct mm_cpu *cpu,
                                    struct mm_memmap *map,
                                    struct mm_scs *scs,
                                    mm_u32 value,
                                    mm_u8 *it_pattern,
                                    mm_u8 *it_remaining,
                                    mm_u8 *it_cond)
{
    (void)cpu;
    (void)map;
    (void)scs;
    (void)it_pattern;
    (void)it_remaining;
    (void)it_cond;
    g_last_pc_write_value = value;
    g_handle_pc_write_calls++;
    return MM_TRUE;
}

static mm_bool stub_raise_mem_fault(struct mm_cpu *cpu,
                                    struct mm_memmap *map,
                                    struct mm_scs *scs,
                                    mm_u32 fault_pc,
                                    mm_u32 fault_xpsr,
                                    mm_u32 addr,
                                    mm_bool is_exec)
{
    (void)cpu;
    (void)map;
    (void)scs;
    (void)fault_pc;
    (void)fault_xpsr;
    (void)addr;
    (void)is_exec;
    return MM_FALSE;
}

static mm_bool stub_raise_usage_fault(struct mm_cpu *cpu,
                                      struct mm_memmap *map,
                                      struct mm_scs *scs,
                                      mm_u32 fault_pc,
                                      mm_u32 fault_xpsr,
                                      mm_u32 ufsr_bits)
{
    (void)cpu;
    (void)map;
    (void)scs;
    (void)fault_pc;
    (void)fault_xpsr;
    g_last_ufsr_bits = ufsr_bits;
    g_raise_usage_fault_calls++;
    return MM_FALSE;
}

static mm_bool stub_exc_return_unstack(struct mm_cpu *cpu,
                                       struct mm_memmap *map,
                                       struct mm_scs *scs,
                                       mm_u32 exc_ret)
{
    (void)cpu;
    (void)map;
    (void)scs;
    (void)exc_ret;
    return MM_FALSE;
}

static mm_bool stub_enter_exception(struct mm_cpu *cpu,
                                    struct mm_memmap *map,
                                    struct mm_scs *scs,
                                    mm_u32 exc_num,
                                    mm_u32 return_pc,
                                    mm_u32 xpsr_in)
{
    (void)cpu;
    (void)map;
    (void)scs;
    g_last_exc_num = exc_num;
    g_last_return_pc = return_pc;
    g_enter_exception_calls++;
    (void)xpsr_in;
    return MM_FALSE;
}

static void setup_ram_map(struct mm_memmap *map, mm_u8 *ram, size_t ram_len)
{
    struct mmio_region regions[1];
    struct mm_target_cfg cfg;

    memset(regions, 0, sizeof(regions));
    mm_memmap_init(map, regions, 1u);
    memset(&cfg, 0, sizeof(cfg));
    cfg.ram_base_s = 0x20000000u;
    cfg.ram_size_s = (mm_u32)ram_len;
    cfg.ram_base_ns = 0x20000000u;
    cfg.ram_size_ns = (mm_u32)ram_len;
    (void)mm_memmap_configure_ram(map, &cfg, ram, MM_FALSE);
}

static int exec_one_it(struct mm_cpu *cpu,
                       struct mm_memmap *map,
                       struct mm_scs *scs,
                       struct mm_nvic *nvic,
                       struct mm_decoded *dec,
                       mm_u8 it_pattern_init,
                       mm_u8 it_remaining_init,
                       mm_u8 it_cond_init);

static int exec_one(struct mm_cpu *cpu,
                    struct mm_memmap *map,
                    struct mm_scs *scs,
                    struct mm_nvic *nvic,
                    struct mm_decoded *dec)
{
    return exec_one_it(cpu, map, scs, nvic, dec, 0u, 0u, 0u);
}

static int exec_one_it(struct mm_cpu *cpu,
                       struct mm_memmap *map,
                       struct mm_scs *scs,
                       struct mm_nvic *nvic,
                       struct mm_decoded *dec,
                       mm_u8 it_pattern_init,
                       mm_u8 it_remaining_init,
                       mm_u8 it_cond_init)
{
    struct mm_fetch_result fetch;
    struct mm_gdb_stub gdb;
    struct mm_execute_ctx ctx;
    mm_u8 it_pattern = it_pattern_init;
    mm_u8 it_remaining = it_remaining_init;
    mm_u8 it_cond = it_cond_init;
    mm_bool done = MM_FALSE;

    memset(&fetch, 0, sizeof(fetch));
    memset(&gdb, 0, sizeof(gdb));
    memset(&ctx, 0, sizeof(ctx));
    if (it_remaining_init > 0u) {
        cpu->xpsr = itstate_set(cpu->xpsr, 0x18u);
    }
    fetch.pc_fetch = cpu->r[15] & ~1u;
    ctx.cpu = cpu;
    ctx.map = map;
    ctx.scs = scs;
    ctx.gdb = &gdb;
    ctx.fetch = &fetch;
    ctx.dec = dec;
    ctx.nvic = nvic;
    ctx.it_pattern = &it_pattern;
    ctx.it_remaining = &it_remaining;
    ctx.it_cond = &it_cond;
    ctx.done = &done;
    ctx.handle_pc_write = stub_handle_pc_write;
    ctx.raise_mem_fault = stub_raise_mem_fault;
    ctx.raise_usage_fault = stub_raise_usage_fault;
    ctx.exc_return_unstack = stub_exc_return_unstack;
    ctx.enter_exception = stub_enter_exception;

    if (mm_execute_decoded(&ctx) != MM_EXEC_OK) {
        return 1;
    }
    return done ? 1 : 0;
}

static int test_ldrsb_reg_wide_shift_execute(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_decoded dec;
    mm_u8 ram[128];

    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));
    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.r[15] = 1u;
    cpu.r[0] = 0x20000000u;
    cpu.r[1] = 4u;
    if (!mm_memmap_write(&map, cpu.sec_state, cpu.r[0] + 4u, 1u, 0x11u)) return 1;
    if (!mm_memmap_write(&map, cpu.sec_state, cpu.r[0] + 16u, 1u, 0x80u)) return 1;

    memset(&dec, 0, sizeof(dec));
    dec.kind = MM_OP_LDRSB_REG;
    dec.rn = 0u;
    dec.rm = 1u;
    dec.rd = 2u;
    dec.imm = 2u;

    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0) return 1;
    if (cpu.r[2] != 0xFFFFFF80u) return 1;
    return 0;
}

static int test_mrs_msp_reads_banked_msp(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_decoded dec;
    mm_u8 ram[32];

    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));
    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.r[15] = 1u;
    cpu.control_s = 0x2u;
    cpu.msp_s = 0x20001000u;
    cpu.psp_s = 0x20002000u;
    cpu.r[13] = cpu.psp_s;

    memset(&dec, 0, sizeof(dec));
    dec.kind = MM_OP_MRS;
    dec.rd = 0u;
    dec.imm = 0x08u;

    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0) return 1;
    if (cpu.r[0] != cpu.msp_s) return 1;
    return 0;
}

static int test_adc_sbc_16bit_preserve_flags_inside_it(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_decoded dec;
    mm_u8 ram[32];
    mm_u32 xpsr_before;

    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));
    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.r[15] = 1u;
    cpu.r[0] = 1u;
    cpu.r[1] = 2u;
    cpu.xpsr = 0xF0000000u;

    memset(&dec, 0, sizeof(dec));
    dec.kind = MM_OP_ADCS_REG;
    dec.len = 2u;
    dec.rd = 0u;
    dec.rn = 0u;
    dec.rm = 1u;
    xpsr_before = cpu.xpsr;
    if (exec_one_it(&cpu, &map, &scs, 0, &dec, 0u, 1u, 0u) != 0) {
        printf("adcs_it: exec_one_it failed xpsr=0x%08lx\n", (unsigned long)cpu.xpsr);
        return 1;
    }
    if (cpu.r[0] != 4u) {
        printf("adcs_it: result=0x%08lx\n", (unsigned long)cpu.r[0]);
        return 1;
    }
    if ((cpu.xpsr & 0xF0000000u) != (xpsr_before & 0xF0000000u)) {
        printf("adcs_it: nzcv_before=0x%08lx nzcv_after=0x%08lx full_after=0x%08lx\n",
               (unsigned long)(xpsr_before & 0xF0000000u),
               (unsigned long)(cpu.xpsr & 0xF0000000u),
               (unsigned long)cpu.xpsr);
        return 1;
    }

    cpu.r[0] = 5u;
    cpu.r[1] = 1u;
    cpu.xpsr = 0xA0000000u;
    dec.kind = MM_OP_SBCS_REG;
    xpsr_before = cpu.xpsr;
    if (exec_one_it(&cpu, &map, &scs, 0, &dec, 0u, 1u, 0u) != 0) {
        printf("sbcs_it: exec_one_it failed xpsr=0x%08lx\n", (unsigned long)cpu.xpsr);
        return 1;
    }
    if (cpu.r[0] != 4u) {
        printf("sbcs_it: result=0x%08lx\n", (unsigned long)cpu.r[0]);
        return 1;
    }
    if ((cpu.xpsr & 0xF0000000u) != (xpsr_before & 0xF0000000u)) {
        printf("sbcs_it: nzcv_before=0x%08lx nzcv_after=0x%08lx full_after=0x%08lx\n",
               (unsigned long)(xpsr_before & 0xF0000000u),
               (unsigned long)(cpu.xpsr & 0xF0000000u),
               (unsigned long)cpu.xpsr);
        return 1;
    }
    return 0;
}

static int test_blx_uses_pc_write_for_invstate_checked_target(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_decoded dec;
    mm_u8 ram[32];

    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));
    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.r[15] = 0x101u;
    cpu.r[3] = 0x08001000u;
    g_last_pc_write_value = 0u;
    g_handle_pc_write_calls = 0;

    memset(&dec, 0, sizeof(dec));
    dec.kind = MM_OP_BLX;
    dec.len = 2u;
    dec.rm = 3u;

    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0) return 1;
    if (g_handle_pc_write_calls != 1) return 1;
    if (g_last_pc_write_value != 0x08001000u) return 1;
    if (cpu.r[14] != 0x103u) return 1;
    return 0;
}

static int test_mrs_psr_uses_shadow_q_ge_fields(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_decoded dec;
    mm_u8 ram[32];

    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));
    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.r[15] = 1u;
    cpu.xpsr = 0x980F0000u;
    cpu.q_flag = MM_FALSE;
    cpu.ge_flags = 0u;

    memset(&dec, 0, sizeof(dec));
    dec.kind = MM_OP_MRS;
    dec.rd = 0u;
    dec.imm = 0x00u;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0) return 1;
    if (cpu.r[0] != 0x90000000u) return 1;

    cpu.q_flag = MM_TRUE;
    cpu.ge_flags = 0x5u;
    dec.rd = 1u;
    dec.imm = 0x03u;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0) return 1;
    if (cpu.r[1] != 0x98050000u) return 1;
    return 0;
}

static int test_vabs_clears_sign_bit_for_neg_zero_and_nan(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_decoded dec;
    mm_u8 ram[32];

    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));
    scs.fpu_present = MM_TRUE;
    scs.cpacr_s = 0x00F00000u;
    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.r[15] = 1u;

    memset(&dec, 0, sizeof(dec));
    dec.kind = MM_OP_VABS;
    dec.rd = 0u;
    dec.rm = 1u;

    cpu.s[1] = 0x80000000u;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0) return 1;
    if (cpu.s[0] != 0x00000000u) return 1;

    cpu.s[1] = 0xFFC00001u;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0) return 1;
    if (cpu.s[0] != 0x7FC00001u) return 1;
    return 0;
}

static int test_mvn_reg_wide_and_mul_w_suppress_flags_mid_it(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_decoded dec;
    mm_u8 ram[32];
    mm_u32 xpsr_before;

    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));
    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.r[15] = 1u;
    cpu.r[1] = 0u;
    cpu.r[2] = 2u;

    memset(&dec, 0, sizeof(dec));
    dec.kind = MM_OP_MVN_REG;
    dec.len = 4u;
    dec.raw = (1u << 20);
    dec.rd = 0u;
    dec.rm = 1u;
    xpsr_before = 0x60000000u;
    cpu.xpsr = xpsr_before;
    if (exec_one_it(&cpu, &map, &scs, 0, &dec, 0u, 2u, 0u) != 0) {
        printf("mvn_w_it: exec_one_it failed xpsr=0x%08lx\n", (unsigned long)cpu.xpsr);
        return 1;
    }
    if (cpu.r[0] != 0xFFFFFFFFu) {
        printf("mvn_w_it: result=0x%08lx\n", (unsigned long)cpu.r[0]);
        return 1;
    }
    if ((cpu.xpsr & 0xF0000000u) != (xpsr_before & 0xF0000000u)) {
        printf("mvn_w_it: nzcv_before=0x%08lx nzcv_after=0x%08lx full_after=0x%08lx\n",
               (unsigned long)(xpsr_before & 0xF0000000u),
               (unsigned long)(cpu.xpsr & 0xF0000000u),
               (unsigned long)cpu.xpsr);
        return 1;
    }

    dec.kind = MM_OP_MUL_W;
    dec.rd = 0u;
    dec.rn = 1u;
    dec.rm = 2u;
    dec.imm = 1u;
    cpu.r[1] = 0x80000000u;
    cpu.r[2] = 2u;
    xpsr_before = 0x20000000u;
    cpu.xpsr = xpsr_before;
    if (exec_one_it(&cpu, &map, &scs, 0, &dec, 0u, 2u, 0u) != 0) {
        printf("mul_w_it: exec_one_it failed xpsr=0x%08lx\n", (unsigned long)cpu.xpsr);
        return 1;
    }
    if (cpu.r[0] != 0u) {
        printf("mul_w_it: result=0x%08lx\n", (unsigned long)cpu.r[0]);
        return 1;
    }
    if ((cpu.xpsr & 0xF0000000u) != (xpsr_before & 0xF0000000u)) {
        printf("mul_w_it: nzcv_before=0x%08lx nzcv_after=0x%08lx full_after=0x%08lx\n",
               (unsigned long)(xpsr_before & 0xF0000000u),
               (unsigned long)(cpu.xpsr & 0xF0000000u),
               (unsigned long)cpu.xpsr);
        return 1;
    }
    return 0;
}

static int test_msr_splim_masks_low_bits(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_decoded dec;
    mm_u8 ram[32];

    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));
    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.r[15] = 1u;
    cpu.r[0] = 0x20000003u;

    memset(&dec, 0, sizeof(dec));
    dec.kind = MM_OP_MSR;
    dec.rm = 0u;
    dec.imm = 0x0Au;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0) return 1;
    if (cpu.msplim_s != 0x20000000u) return 1;

    cpu.r[0] = 0x20000007u;
    dec.imm = 0x0Bu;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0) return 1;
    if (cpu.psplim_s != 0x20000000u) return 1;
    return 0;
}

static int test_unprivileged_t_forms_execute(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_decoded dec;
    mm_u8 ram[128];
    mm_u32 value;

    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));

    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.r[15] = 1u;
    cpu.r[0] = 0x20000000u;
    memset(&dec, 0, sizeof(dec));
    dec.rn = 0u;
    dec.rd = 1u;

    dec.kind = MM_OP_STRT;
    dec.imm = 4u;
    cpu.r[1] = 0x11223344u;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0) return 1;
    if (!mm_memmap_read(&map, cpu.sec_state, cpu.r[0] + 4u, 4u, &value) || value != 0x11223344u) return 1;

    dec.kind = MM_OP_LDRT;
    cpu.r[1] = 0u;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0 || cpu.r[1] != 0x11223344u) return 1;

    dec.kind = MM_OP_STRBT;
    dec.imm = 8u;
    cpu.r[1] = 0xA5u;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0) return 1;

    dec.kind = MM_OP_LDRBT;
    cpu.r[1] = 0u;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0 || cpu.r[1] != 0xA5u) return 1;

    dec.kind = MM_OP_STRHT;
    dec.imm = 12u;
    cpu.r[1] = 0xBEEFu;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0) return 1;

    dec.kind = MM_OP_LDRHT;
    cpu.r[1] = 0u;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0 || cpu.r[1] != 0xBEEFu) return 1;

    if (!mm_memmap_write(&map, cpu.sec_state, cpu.r[0] + 16u, 1u, 0x80u)) return 1;
    dec.kind = MM_OP_LDRSBT;
    dec.imm = 16u;
    cpu.r[1] = 0u;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0 || cpu.r[1] != 0xFFFFFF80u) return 1;

    if (!mm_memmap_write(&map, cpu.sec_state, cpu.r[0] + 20u, 2u, 0x8001u)) return 1;
    dec.kind = MM_OP_LDRSHT;
    dec.imm = 20u;
    cpu.r[1] = 0u;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0 || cpu.r[1] != 0xFFFF8001u) return 1;

    return 0;
}

static int test_wide_hints_execute(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_decoded dec;
    mm_u8 ram[32];

    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));

    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.r[15] = 1u;
    memset(&dec, 0, sizeof(dec));

    dec.kind = MM_OP_YIELD_W;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0) return 1;

    dec.kind = MM_OP_SEV_W;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0 || cpu.event_reg != MM_TRUE) return 1;

    cpu.event_reg = MM_FALSE;
    dec.kind = MM_OP_SEVL_W;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0 || cpu.event_reg != MM_TRUE) return 1;

    cpu.event_reg = MM_FALSE;
    dec.kind = MM_OP_WFE_W;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0 || cpu.sleeping != MM_TRUE || cpu.sleep_wfe != MM_TRUE) return 1;

    cpu.sleeping = MM_FALSE;
    cpu.sleep_wfe = MM_FALSE;
    dec.kind = MM_OP_WFI_W;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0 || cpu.sleeping != MM_TRUE || cpu.sleep_wfe != MM_FALSE) return 1;

    return 0;
}

static int test_bkpt_enters_debugmon(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_decoded dec;
    mm_u8 ram[32];

    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));
    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.r[15] = 0x101u;
    dec.kind = MM_OP_BKPT;
    dec.imm = 0;
    g_last_exc_num = 0;
    g_last_return_pc = 0;
    g_enter_exception_calls = 0;

    if (exec_one(&cpu, &map, &scs, 0, &dec) == 0) {
        return 1;
    }
    if (g_enter_exception_calls != 1) return 1;
    if (g_last_exc_num != MM_VECT_DEBUGMON) return 1;
    if (g_last_return_pc != 0x102u) return 1;
    if ((scs.dfsr & (1u << 1)) == 0u) return 1;
    return 0;
}

static int test_ldrexh_strexh_execute(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_decoded dec;
    mm_u8 ram[64];
    mm_u32 value = 0;

    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));
    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.r[15] = 1u;
    cpu.r[0] = 0x20000000u;
    if (!mm_memmap_write(&map, cpu.sec_state, cpu.r[0], 2u, 0xABCDu)) return 1;

    memset(&dec, 0, sizeof(dec));
    dec.kind = MM_OP_LDREXH;
    dec.rn = 0u;
    dec.rd = 1u;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0) return 1;
    if (cpu.r[1] != 0xABCDu) return 1;

    dec.kind = MM_OP_STREXH;
    dec.rn = 0u;
    dec.rm = 2u;
    dec.rd = 3u;
    cpu.r[2] = 0x1234u;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0) return 1;
    if (cpu.r[3] != 0u) return 1;
    if (!mm_memmap_read(&map, cpu.sec_state, cpu.r[0], 2u, &value)) return 1;
    if ((value & 0xffffu) != 0x1234u) return 1;
    return 0;
}

static int test_ldrex_strex_word_offset_execute(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_decoded dec;
    mm_u8 ram[64];
    mm_u32 value = 0;

    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));
    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.r[15] = 1u;
    cpu.r[0] = 0x20000000u;
    if (!mm_memmap_write(&map, cpu.sec_state, cpu.r[0], 4u, 0x11111111u)) return 1;
    if (!mm_memmap_write(&map, cpu.sec_state, cpu.r[0] + 8u, 4u, 0xA5A5A5A5u)) return 1;

    memset(&dec, 0, sizeof(dec));
    dec.kind = MM_OP_LDREX;
    dec.rn = 0u;
    dec.rd = 1u;
    dec.imm = 8u;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0) return 1;
    if (cpu.r[1] != 0xA5A5A5A5u) return 1;

    dec.kind = MM_OP_STREX;
    dec.rn = 0u;
    dec.rm = 2u;
    dec.rd = 3u;
    dec.imm = 8u;
    cpu.r[2] = 0x12345678u;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0) return 1;
    if (cpu.r[3] != 0u) return 1;
    if (!mm_memmap_read(&map, cpu.sec_state, cpu.r[0], 4u, &value)) return 1;
    if (value != 0x11111111u) return 1;
    if (!mm_memmap_read(&map, cpu.sec_state, cpu.r[0] + 8u, 4u, &value)) return 1;
    if (value != 0x12345678u) return 1;
    return 0;
}

static int test_exclusive_unaligned_raise_usagefault(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_decoded dec;
    mm_u8 ram[64];

    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));
    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.r[15] = 1u;
    cpu.r[0] = 0x20000001u;
    cpu.r[2] = 0x11223344u;

    g_last_ufsr_bits = 0u;
    g_raise_usage_fault_calls = 0;

    memset(&dec, 0, sizeof(dec));
    dec.kind = MM_OP_LDREX;
    dec.rn = 0u;
    dec.rd = 1u;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 1) return 1;
    if (g_raise_usage_fault_calls != 1 || g_last_ufsr_bits != (1u << 24)) return 1;

    g_last_ufsr_bits = 0u;
    g_raise_usage_fault_calls = 0;
    dec.kind = MM_OP_STREX;
    dec.rm = 2u;
    dec.rd = 3u;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 1) return 1;
    if (g_raise_usage_fault_calls != 1 || g_last_ufsr_bits != (1u << 24)) return 1;

    cpu.r[0] = 0x20000001u;
    g_last_ufsr_bits = 0u;
    g_raise_usage_fault_calls = 0;
    memset(&dec, 0, sizeof(dec));
    dec.kind = MM_OP_LDREXH;
    dec.rn = 0u;
    dec.rd = 1u;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 1) return 1;
    if (g_raise_usage_fault_calls != 1 || g_last_ufsr_bits != (1u << 24)) return 1;

    g_last_ufsr_bits = 0u;
    g_raise_usage_fault_calls = 0;
    dec.kind = MM_OP_STREXH;
    dec.rm = 2u;
    dec.rd = 3u;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 1) return 1;
    if (g_raise_usage_fault_calls != 1 || g_last_ufsr_bits != (1u << 24)) return 1;

    return 0;
}

static int test_acquire_release_execute(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_decoded dec;
    mm_u8 ram[64];
    mm_u32 value = 0;
    mm_u8 sizes[3] = { 1u, 2u, 4u };
    int i;

    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));
    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.r[15] = 1u;

    /* STLB/LDAB at an odd address: byte accesses have no alignment rule */
    cpu.r[0] = 0x20000001u;
    cpu.r[2] = 0x1234A5u;
    memset(&dec, 0, sizeof(dec));
    dec.kind = MM_OP_STL;
    dec.rn = 0u;
    dec.rd = 2u;
    dec.ra = 1u;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0) return 1;
    if (!mm_memmap_read(&map, cpu.sec_state, 0x20000000u, 4u, &value)) return 1;
    if (value != 0x0000A500u) return 1;
    dec.kind = MM_OP_LDA;
    dec.rd = 1u;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0) return 1;
    if (cpu.r[1] != 0xA5u) return 1;

    /* aligned word and halfword round trips */
    for (i = 1; i < 3; ++i) {
        cpu.r[0] = 0x20000010u;
        cpu.r[2] = 0xCAFEBABEu;
        dec.kind = MM_OP_STL;
        dec.rd = 2u;
        dec.ra = sizes[i];
        if (exec_one(&cpu, &map, &scs, 0, &dec) != 0) return 1;
        dec.kind = MM_OP_LDA;
        dec.rd = 1u;
        if (exec_one(&cpu, &map, &scs, 0, &dec) != 0) return 1;
        if (cpu.r[1] != (sizes[i] == 2u ? 0xBABEu : 0xCAFEBABEu)) return 1;
    }

    /* misaligned halfword/word acquire-release faults with UNALIGN_TRP clear */
    scs.ccr = 0u;
    for (i = 1; i < 3; ++i) {
        cpu.r[0] = 0x20000011u;
        g_last_ufsr_bits = 0u;
        g_raise_usage_fault_calls = 0;
        dec.kind = MM_OP_LDA;
        dec.ra = sizes[i];
        if (exec_one(&cpu, &map, &scs, 0, &dec) != 1) return 1;
        if (g_raise_usage_fault_calls != 1 || g_last_ufsr_bits != (1u << 24)) return 1;
        g_last_ufsr_bits = 0u;
        g_raise_usage_fault_calls = 0;
        dec.kind = MM_OP_STL;
        if (exec_one(&cpu, &map, &scs, 0, &dec) != 1) return 1;
        if (g_raise_usage_fault_calls != 1 || g_last_ufsr_bits != (1u << 24)) return 1;
    }
    return 0;
}

static int test_sub_imm_pc_align_execute(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_decoded dec;
    mm_u8 ram[32];

    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));
    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.r[15] = 0x1003u;

    memset(&dec, 0, sizeof(dec));
    dec.kind = MM_OP_SUB_IMM;
    dec.rn = 15u;
    dec.rd = 0u;
    dec.imm = 4u;
    dec.len = 2u;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0) return 1;
    if (cpu.r[0] != 0x1000u) return 1;

    cpu.r[1] = 0u;
    dec.kind = MM_OP_SUB_IMM_NF;
    dec.rd = 1u;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0) return 1;
    if (cpu.r[1] != 0x1000u) return 1;
    return 0;
}

static int test_svc_handler_mode_escalates_to_hardfault_when_not_highest_priority(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_nvic nvic;
    struct mm_decoded dec;
    mm_u8 ram[32];

    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    memset(&nvic, 0, sizeof(nvic));
    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));
    mm_nvic_init(&nvic);
    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_HANDLER;
    cpu.r[15] = 0x101u;
    cpu.xpsr = MM_VECT_PENDSV; /* placeholder overwritten by mode-specific state below */
    cpu.xpsr = 16u; /* IRQ0 active */
    nvic.priority[0] = 0x00u;
    scs.shpr2_s = 0x80000000u; /* SVC priority 0x80 */
    g_last_exc_num = 0;
    g_enter_exception_calls = 0;

    memset(&dec, 0, sizeof(dec));
    dec.kind = MM_OP_SVC;
    if (exec_one(&cpu, &map, &scs, &nvic, &dec) == 0) {
        return 1;
    }
    if (g_enter_exception_calls != 1) return 1;
    if (g_last_exc_num != MM_VECT_HARDFAULT) return 1;
    return 0;
}

static int test_shift_and_mov_wide_preserve_v_flag(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_decoded dec;
    mm_u8 ram[32];
    size_t i;
    static const enum mm_op_kind kinds[] = {
        MM_OP_MOV_IMM,
        MM_OP_LSL_REG,
        MM_OP_LSL_IMM,
        MM_OP_LSR_REG,
        MM_OP_LSR_IMM,
        MM_OP_ROR_IMM,
        MM_OP_ASR_REG,
        MM_OP_ASR_IMM,
    };

    memset(&scs, 0, sizeof(scs));
    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));

    for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
        memset(&cpu, 0, sizeof(cpu));
        memset(&dec, 0, sizeof(dec));
        cpu.sec_state = MM_SECURE;
        cpu.mode = MM_THREAD;
        cpu.r[15] = 1u;
        cpu.xpsr = (1u << 28) | (1u << 29); /* V=1, C=1 */
        cpu.r[1] = 0x40000000u;
        cpu.r[2] = 1u;

        dec.kind = kinds[i];
        dec.len = 4u;
        dec.raw = (1u << 20); /* setflags */
        dec.rd = 0u;
        dec.rn = 1u;
        dec.rm = 2u;
        dec.imm = 1u;
        if (kinds[i] == MM_OP_MOV_IMM) {
            dec.rm = 0u;
            dec.rn = 0u;
            dec.imm = 0u;
        }

        if (exec_one(&cpu, &map, &scs, 0, &dec) != 0) return 1;
        if ((cpu.xpsr & (1u << 28)) == 0u) return 1;
    }
    return 0;
}

static int test_asr_reg_wide_updates_flags(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_decoded dec;
    mm_u8 ram[32];

    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));
    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.r[15] = 1u;
    cpu.xpsr = (1u << 28); /* V=1 should be preserved */
    cpu.r[1] = 0x80000001u;
    cpu.r[2] = 1u;

    memset(&dec, 0, sizeof(dec));
    dec.kind = MM_OP_ASR_REG;
    dec.len = 4u;
    dec.raw = (1u << 20); /* setflags */
    dec.rd = 0u;
    dec.rn = 1u;
    dec.rm = 2u;

    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0) return 1;
    if (cpu.r[0] != 0xC0000000u) {
        printf("asr_reg_wide: result=0x%08lx\n", (unsigned long)cpu.r[0]);
        return 1;
    }
    if ((cpu.xpsr & (1u << 31)) == 0u) {
        printf("asr_reg_wide: missing N xpsr=0x%08lx\n", (unsigned long)cpu.xpsr);
        return 1;
    }
    if ((cpu.xpsr & (1u << 30)) != 0u) {
        printf("asr_reg_wide: unexpected Z xpsr=0x%08lx\n", (unsigned long)cpu.xpsr);
        return 1;
    }
    if ((cpu.xpsr & (1u << 29)) == 0u) {
        printf("asr_reg_wide: missing C xpsr=0x%08lx\n", (unsigned long)cpu.xpsr);
        return 1;
    }
    if ((cpu.xpsr & (1u << 28)) == 0u) {
        printf("asr_reg_wide: missing preserved V xpsr=0x%08lx\n", (unsigned long)cpu.xpsr);
        return 1;
    }
    return 0;
}

static int test_ror_reg_wide_updates_flags(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_decoded dec;
    mm_u8 ram[32];

    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));
    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.r[15] = 1u;
    cpu.xpsr = (1u << 28); /* V=1 should be preserved */
    cpu.r[1] = 0x80000001u;
    cpu.r[2] = 1u;

    memset(&dec, 0, sizeof(dec));
    dec.kind = MM_OP_ROR_REG;
    dec.len = 4u;
    dec.raw = (1u << 20); /* setflags */
    dec.rd = 0u;
    dec.rn = 1u;
    dec.rm = 2u;

    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0) return 1;
    if (cpu.r[0] != 0xC0000000u) {
        printf("ror_reg_wide: result=0x%08lx\n", (unsigned long)cpu.r[0]);
        return 1;
    }
    if ((cpu.xpsr & (1u << 31)) == 0u || (cpu.xpsr & (1u << 30)) != 0u ||
        (cpu.xpsr & (1u << 29)) == 0u || (cpu.xpsr & (1u << 28)) == 0u) {
        printf("ror_reg_wide: xpsr=0x%08lx\n", (unsigned long)cpu.xpsr);
        return 1;
    }
    return 0;
}

static int test_psplim_unprivileged_nonsecure_write_ignored(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_decoded dec;
    mm_u8 ram[32];

    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));
    cpu.sec_state = MM_NONSECURE;
    cpu.mode = MM_THREAD;
    cpu.priv_ns = MM_TRUE;
    cpu.control_ns = 0x1u;
    if (mm_cpu_get_privileged(&cpu) != MM_FALSE) return 1;
    cpu.r[15] = 1u;
    cpu.r[0] = 0x20001023u;
    cpu.psplim_ns = 0x20000000u;

    memset(&dec, 0, sizeof(dec));
    dec.kind = MM_OP_MSR;
    dec.rm = 0u;
    dec.imm = 0x0bu; /* PSPLIM */
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0) return 1;
    if (cpu.psplim_ns != 0x20000000u) {
        printf("psplim_unpriv_ns: psplim_ns=0x%08lx\n", (unsigned long)cpu.psplim_ns);
        return 1;
    }

    dec.imm = 0x8bu; /* PSPLIM_NS */
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0) return 1;
    if (cpu.psplim_ns != 0x20000000u) {
        printf("psplim_ns_unpriv_ns: psplim_ns=0x%08lx\n", (unsigned long)cpu.psplim_ns);
        return 1;
    }
    return 0;
}

static int test_cps_faultmask_affects_current_security_bank(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_decoded dec;
    mm_u8 ram[32];

    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));
    cpu.mode = MM_THREAD;
    cpu.r[15] = 1u;

    memset(&dec, 0, sizeof(dec));
    dec.kind = MM_OP_CPS;

    cpu.sec_state = MM_SECURE;
    dec.imm = 0x11u; /* CPSID f */
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0) return 1;
    if (cpu.faultmask_s != 1u || cpu.faultmask_ns != 0u) {
        printf("cps_faultmask_secure_set: s=%lu ns=%lu\n",
               (unsigned long)cpu.faultmask_s, (unsigned long)cpu.faultmask_ns);
        return 1;
    }
    dec.imm = 0x01u; /* CPSIE f */
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0) return 1;
    if (cpu.faultmask_s != 0u) {
        printf("cps_faultmask_secure_clear: s=%lu\n", (unsigned long)cpu.faultmask_s);
        return 1;
    }

    cpu.sec_state = MM_NONSECURE;
    dec.imm = 0x11u; /* CPSID f */
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0) return 1;
    if (cpu.faultmask_ns != 1u || cpu.faultmask_s != 0u) {
        printf("cps_faultmask_ns_set: s=%lu ns=%lu\n",
               (unsigned long)cpu.faultmask_s, (unsigned long)cpu.faultmask_ns);
        return 1;
    }
    return 0;
}

static int test_wide_s_ops_suppress_flags_mid_it_consistently(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_decoded dec;
    mm_u8 ram[32];
    mm_u32 xpsr_before;

    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));
    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.r[15] = 1u;
    cpu.r[1] = 5u;
    cpu.r[2] = 3u;
    cpu.r[3] = 0x12345678u;
    xpsr_before = 0xB0000000u;

    memset(&dec, 0, sizeof(dec));
    dec.len = 4u;
    dec.raw = (1u << 20);
    dec.rd = 0u;
    dec.rn = 1u;
    dec.rm = 2u;
    dec.imm = 0u;

    dec.kind = MM_OP_RSB_IMM;
    dec.imm = 1u;
    cpu.xpsr = xpsr_before;
    if (exec_one_it(&cpu, &map, &scs, 0, &dec, 0u, 2u, 0u) != 0) return 1;
    if ((cpu.xpsr & 0xF0000000u) != xpsr_before) {
        printf("mid_it_rsb_imm: xpsr=0x%08lx\n", (unsigned long)cpu.xpsr);
        return 1;
    }

    dec.kind = MM_OP_ADD_REG;
    dec.imm = 0u;
    cpu.xpsr = xpsr_before;
    if (exec_one_it(&cpu, &map, &scs, 0, &dec, 0u, 2u, 0u) != 0) return 1;
    if ((cpu.xpsr & 0xF0000000u) != xpsr_before) {
        printf("mid_it_add_reg: xpsr=0x%08lx\n", (unsigned long)cpu.xpsr);
        return 1;
    }

    dec.kind = MM_OP_ADC_IMM;
    dec.imm = 1u;
    cpu.xpsr = xpsr_before | (1u << 29);
    if (exec_one_it(&cpu, &map, &scs, 0, &dec, 0u, 2u, 0u) != 0) return 1;
    if ((cpu.xpsr & 0xF0000000u) != (xpsr_before | (1u << 29))) {
        printf("mid_it_adc_imm: xpsr=0x%08lx\n", (unsigned long)cpu.xpsr);
        return 1;
    }

    dec.kind = MM_OP_MVN_IMM;
    dec.imm = 0u;
    cpu.xpsr = xpsr_before;
    if (exec_one_it(&cpu, &map, &scs, 0, &dec, 0u, 2u, 0u) != 0) return 1;
    if ((cpu.xpsr & 0xF0000000u) != xpsr_before) {
        printf("mid_it_mvn_imm: xpsr=0x%08lx\n", (unsigned long)cpu.xpsr);
        return 1;
    }

    dec.kind = MM_OP_SUB_REG;
    dec.imm = 0u;
    cpu.xpsr = xpsr_before;
    if (exec_one_it(&cpu, &map, &scs, 0, &dec, 0u, 2u, 0u) != 0) return 1;
    if ((cpu.xpsr & 0xF0000000u) != xpsr_before) {
        printf("mid_it_sub_reg: xpsr=0x%08lx\n", (unsigned long)cpu.xpsr);
        return 1;
    }

    dec.kind = MM_OP_RSB_REG;
    cpu.xpsr = xpsr_before;
    if (exec_one_it(&cpu, &map, &scs, 0, &dec, 0u, 2u, 0u) != 0) return 1;
    if ((cpu.xpsr & 0xF0000000u) != xpsr_before) {
        printf("mid_it_rsb_reg: xpsr=0x%08lx\n", (unsigned long)cpu.xpsr);
        return 1;
    }

    return 0;
}

static int test_cps_unprivileged_thread_is_nop(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_decoded dec;
    mm_u8 ram[32];

    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));
    cpu.sec_state = MM_NONSECURE;
    cpu.mode = MM_THREAD;
    cpu.priv_ns = MM_TRUE;
    cpu.control_ns = 0x1u;
    if (mm_cpu_get_privileged(&cpu) != MM_FALSE) return 1;
    cpu.r[15] = 1u;
    cpu.primask_ns = 0u;
    cpu.faultmask_ns = 0u;

    memset(&dec, 0, sizeof(dec));
    dec.kind = MM_OP_CPS;
    dec.imm = 0x13u; /* CPSID if */
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0) return 1;
    if (cpu.primask_ns != 0u || cpu.faultmask_ns != 0u) {
        printf("cps_unpriv_nop: primask=%lu faultmask=%lu\n",
               (unsigned long)cpu.primask_ns, (unsigned long)cpu.faultmask_ns);
        return 1;
    }
    return 0;
}

static int test_ldrd_ldm_unaligned_raise_usagefault(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_decoded dec;
    mm_u8 ram[64];

    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));
    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.r[15] = 1u;
    cpu.r[0] = 0x20000002u;

    g_last_ufsr_bits = 0u;
    g_raise_usage_fault_calls = 0;

    memset(&dec, 0, sizeof(dec));
    dec.kind = MM_OP_LDRD;
    dec.rn = 0u;
    dec.rd = 2u;
    dec.rm = 3u;
    dec.imm = 0u;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 1) return 1;
    if (g_raise_usage_fault_calls != 1 || g_last_ufsr_bits != (1u << 24)) {
        printf("ldrd_unaligned: calls=%d bits=0x%08lx\n",
               g_raise_usage_fault_calls, (unsigned long)g_last_ufsr_bits);
        return 1;
    }

    g_last_ufsr_bits = 0u;
    g_raise_usage_fault_calls = 0;
    memset(&dec, 0, sizeof(dec));
    dec.kind = MM_OP_STM;
    dec.rn = 0u;
    dec.imm = (1u << 24) | 0x0003u; /* IA, regs r0/r1 */
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 1) return 1;
    if (g_raise_usage_fault_calls != 1 || g_last_ufsr_bits != (1u << 24)) {
        printf("stm_unaligned: calls=%d bits=0x%08lx\n",
               g_raise_usage_fault_calls, (unsigned long)g_last_ufsr_bits);
        return 1;
    }
    return 0;
}

static int test_regular_load_store_unalign_trp_raise_usagefault(void)
{
    struct mm_cpu cpu;
    struct mm_memmap map;
    struct mm_scs scs;
    struct mm_decoded dec;
    mm_u8 ram[64];
    mm_u32 value = 0;

    memset(&cpu, 0, sizeof(cpu));
    memset(&scs, 0, sizeof(scs));
    memset(ram, 0, sizeof(ram));
    setup_ram_map(&map, ram, sizeof(ram));
    cpu.sec_state = MM_SECURE;
    cpu.mode = MM_THREAD;
    cpu.r[15] = 1u;
    cpu.r[0] = 0x20000001u;
    cpu.r[1] = 0x11223344u;

    if (!mm_memmap_write(&map, cpu.sec_state, 0x20000001u, 4u, 0xA5A55A5Au)) {
        return 1;
    }

    memset(&dec, 0, sizeof(dec));
    dec.kind = MM_OP_LDR_IMM;
    dec.rn = 0u;
    dec.rd = 2u;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 0) return 1;
    if (cpu.r[2] != 0xA5A55A5Au) return 1;

    scs.ccr |= (1u << 3); /* CCR.UNALIGN_TRP */
    g_last_ufsr_bits = 0u;
    g_raise_usage_fault_calls = 0;
    cpu.r[2] = 0u;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 1) return 1;
    if (g_raise_usage_fault_calls != 1 || g_last_ufsr_bits != (1u << 24)) {
        printf("ldr_unalign_trp: calls=%d bits=0x%08lx\n",
               g_raise_usage_fault_calls, (unsigned long)g_last_ufsr_bits);
        return 1;
    }
    if (cpu.r[2] != 0u) return 1;

    g_last_ufsr_bits = 0u;
    g_raise_usage_fault_calls = 0;
    memset(&dec, 0, sizeof(dec));
    dec.kind = MM_OP_STR_IMM;
    dec.rn = 0u;
    dec.rd = 1u;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 1) return 1;
    if (g_raise_usage_fault_calls != 1 || g_last_ufsr_bits != (1u << 24)) {
        printf("str_unalign_trp: calls=%d bits=0x%08lx\n",
               g_raise_usage_fault_calls, (unsigned long)g_last_ufsr_bits);
        return 1;
    }
    if (!mm_memmap_read(&map, cpu.sec_state, 0x20000001u, 4u, &value)) return 1;
    if (value != 0xA5A55A5Au) return 1;

    g_last_ufsr_bits = 0u;
    g_raise_usage_fault_calls = 0;
    cpu.r[0] = 0x20000003u;
    memset(&dec, 0, sizeof(dec));
    dec.kind = MM_OP_LDRH_IMM;
    dec.rn = 0u;
    dec.rd = 2u;
    if (exec_one(&cpu, &map, &scs, 0, &dec) != 1) return 1;
    if (g_raise_usage_fault_calls != 1 || g_last_ufsr_bits != (1u << 24)) {
        printf("ldrh_unalign_trp: calls=%d bits=0x%08lx\n",
               g_raise_usage_fault_calls, (unsigned long)g_last_ufsr_bits);
        return 1;
    }
    return 0;
}

int main(void)
{
    if (test_ldrsb_reg_wide_shift_execute() != 0) {
        printf("FAIL: ldrsb_reg_wide_shift_execute\n");
        return 1;
    }
    if (test_mrs_msp_reads_banked_msp() != 0) {
        printf("FAIL: mrs_msp_reads_banked_msp\n");
        return 1;
    }
    if (test_adc_sbc_16bit_preserve_flags_inside_it() != 0) {
        printf("FAIL: adc_sbc_16bit_preserve_flags_inside_it\n");
        return 1;
    }
    if (test_blx_uses_pc_write_for_invstate_checked_target() != 0) {
        printf("FAIL: blx_uses_pc_write_for_invstate_checked_target\n");
        return 1;
    }
    if (test_mrs_psr_uses_shadow_q_ge_fields() != 0) {
        printf("FAIL: mrs_psr_uses_shadow_q_ge_fields\n");
        return 1;
    }
    if (test_vabs_clears_sign_bit_for_neg_zero_and_nan() != 0) {
        printf("FAIL: vabs_clears_sign_bit_for_neg_zero_and_nan\n");
        return 1;
    }
    if (test_mvn_reg_wide_and_mul_w_suppress_flags_mid_it() != 0) {
        printf("FAIL: mvn_reg_wide_and_mul_w_suppress_flags_mid_it\n");
        return 1;
    }
    if (test_msr_splim_masks_low_bits() != 0) {
        printf("FAIL: msr_splim_masks_low_bits\n");
        return 1;
    }
    if (test_unprivileged_t_forms_execute() != 0) {
        printf("FAIL: unprivileged_t_forms_execute\n");
        return 1;
    }
    if (test_wide_hints_execute() != 0) {
        printf("FAIL: wide_hints_execute\n");
        return 1;
    }
    if (test_bkpt_enters_debugmon() != 0) {
        printf("FAIL: bkpt_enters_debugmon\n");
        return 1;
    }
    if (test_ldrexh_strexh_execute() != 0) {
        printf("FAIL: ldrexh_strexh_execute\n");
        return 1;
    }
    if (test_ldrex_strex_word_offset_execute() != 0) {
        printf("FAIL: ldrex_strex_word_offset_execute\n");
        return 1;
    }
    if (test_exclusive_unaligned_raise_usagefault() != 0) {
        printf("FAIL: exclusive_unaligned_raise_usagefault\n");
        return 1;
    }
    if (test_acquire_release_execute() != 0) {
        printf("FAIL: acquire_release_execute\n");
        return 1;
    }
    if (test_sub_imm_pc_align_execute() != 0) {
        printf("FAIL: sub_imm_pc_align_execute\n");
        return 1;
    }
    if (test_svc_handler_mode_escalates_to_hardfault_when_not_highest_priority() != 0) {
        printf("FAIL: svc_handler_mode_escalates_to_hardfault_when_not_highest_priority\n");
        return 1;
    }
    if (test_shift_and_mov_wide_preserve_v_flag() != 0) {
        printf("FAIL: shift_and_mov_wide_preserve_v_flag\n");
        return 1;
    }
    if (test_asr_reg_wide_updates_flags() != 0) {
        printf("FAIL: asr_reg_wide_updates_flags\n");
        return 1;
    }
    if (test_ror_reg_wide_updates_flags() != 0) {
        printf("FAIL: ror_reg_wide_updates_flags\n");
        return 1;
    }
    if (test_psplim_unprivileged_nonsecure_write_ignored() != 0) {
        printf("FAIL: psplim_unprivileged_nonsecure_write_ignored\n");
        return 1;
    }
    if (test_cps_faultmask_affects_current_security_bank() != 0) {
        printf("FAIL: cps_faultmask_affects_current_security_bank\n");
        return 1;
    }
    if (test_wide_s_ops_suppress_flags_mid_it_consistently() != 0) {
        printf("FAIL: wide_s_ops_suppress_flags_mid_it_consistently\n");
        return 1;
    }
    if (test_cps_unprivileged_thread_is_nop() != 0) {
        printf("FAIL: cps_unprivileged_thread_is_nop\n");
        return 1;
    }
    if (test_ldrd_ldm_unaligned_raise_usagefault() != 0) {
        printf("FAIL: ldrd_ldm_unaligned_raise_usagefault\n");
        return 1;
    }
    if (test_regular_load_store_unalign_trp_raise_usagefault() != 0) {
        printf("FAIL: regular_load_store_unalign_trp_raise_usagefault\n");
        return 1;
    }
    return 0;
}
