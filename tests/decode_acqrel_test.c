/* m33mu -- an ARMv8-M Emulator
 *
 * Copyright (C) 2026  wolfSSL Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 */

/* Load-acquire/store-release and exclusive encodings share hw1 E8C/E8D with
 * LDRD/STRD (P=0, W=0); they must not decode as a doubleword access. */

#include <stdio.h>
#include "m33mu/fetch.h"
#include "m33mu/decode.h"
#include "m33mu/mem.h"
#include "m33mu/cpu.h"

struct case_t {
    const char *name;
    mm_u16 hw1;
    mm_u16 hw2;
    enum mm_op_kind kind;
    mm_u8 rd;
    mm_u8 rn;
    mm_u8 rm;
    mm_u32 imm;
};

static int decode_insn(mm_u16 hw1, mm_u16 hw2, struct mm_decoded *out_dec)
{
    mm_u8 bytes[4];
    struct mm_mem mem;
    struct mm_cpu cpu;
    struct mm_fetch_result fetch;
    size_t i;

    bytes[0] = (mm_u8)(hw1 & 0xffu);
    bytes[1] = (mm_u8)((hw1 >> 8) & 0xffu);
    bytes[2] = (mm_u8)(hw2 & 0xffu);
    bytes[3] = (mm_u8)((hw2 >> 8) & 0xffu);
    mem.buffer = bytes;
    mem.length = sizeof(bytes);
    mem.base = 0;
    for (i = 0; i < 16; ++i) cpu.r[i] = 0;
    cpu.r[15] = 1u;
    cpu.xpsr = 0;
    fetch = mm_fetch_t32(&cpu, &mem);
    if (fetch.fault) return 1;
    *out_dec = mm_decode_t32(&fetch);
    return 0;
}

static const struct case_t cases[] = {
    /* name             hw1      hw2      kind              rd  rn  rm  imm */
    { "STL r2,[r1]",     0xe8c1u, 0x2fafu, MM_OP_STL,        2u, 1u, 0u, 0u },
    { "STLB r2,[r1]",    0xe8c1u, 0x2f8fu, MM_OP_STL,        2u, 1u, 0u, 0u },
    { "STLH r2,[r1]",    0xe8c1u, 0x2f9fu, MM_OP_STL,        2u, 1u, 0u, 0u },
    { "LDA r3,[r0]",     0xe8d0u, 0x3fafu, MM_OP_LDA,        3u, 0u, 0u, 0u },
    { "LDAB r3,[r0]",    0xe8d0u, 0x3f8fu, MM_OP_LDA,        3u, 0u, 0u, 0u },
    { "LDAH r3,[r0]",    0xe8d0u, 0x3f9fu, MM_OP_LDA,        3u, 0u, 0u, 0u },
    { "LDAEX r3,[r0]",   0xe8d0u, 0x3fefu, MM_OP_LDREX,      3u, 0u, 0u, 0u },
    { "LDAEXB r3,[r0]",  0xe8d0u, 0x3fcfu, MM_OP_LDREXB,     3u, 0u, 0u, 0u },
    { "LDAEXH r3,[r0]",  0xe8d0u, 0x3fdfu, MM_OP_LDREXH,     3u, 0u, 0u, 0u },
    { "STLEX r4,r2,[r1]",  0xe8c1u, 0x2fe4u, MM_OP_STREX,    4u, 1u, 2u, 0u },
    { "STLEXB r4,r2,[r1]", 0xe8c1u, 0x2fc4u, MM_OP_STREXB,   4u, 1u, 2u, 0u },
    { "STLEXH r4,r2,[r1]", 0xe8c1u, 0x2fd4u, MM_OP_STREXH,   4u, 1u, 2u, 0u },
    { "LDREXB r3,[r0]",  0xe8d0u, 0x3f4fu, MM_OP_LDREXB,     3u, 0u, 0u, 0u },
    { "LDREXH r3,[r0]",  0xe8d0u, 0x3f5fu, MM_OP_LDREXH,     3u, 0u, 0u, 0u },
    { "STREXB r4,r2,[r1]", 0xe8c1u, 0x2f44u, MM_OP_STREXB,   4u, 1u, 2u, 0u },
    { "STREXH r4,r2,[r1]", 0xe8c1u, 0x2f54u, MM_OP_STREXH,   4u, 1u, 2u, 0u },
    { "LDREX r3,[r0,#8]",  0xe850u, 0x3f02u, MM_OP_LDREX,    3u, 0u, 0u, 8u },
    { "STREX r4,r2,[r1,#4]", 0xe841u, 0x2401u, MM_OP_STREX,  4u, 1u, 2u, 4u },
    { "TBB [r0,r1]",     0xe8d0u, 0xf001u, MM_OP_TBB,        0u, 0u, 1u, 0u },
    { "TBH [r0,r1]",     0xe8d0u, 0xf011u, MM_OP_TBH,        0u, 0u, 1u, 0u },
};

/* LDRD/STRD keep decoding when P or W is set. */
static const struct case_t dual_cases[] = {
    { "STRD r2,r3,[r1,#8]",   0xe9c1u, 0x2302u, MM_OP_STRD,  2u, 1u, 3u, 0u },
    { "LDRD r2,r3,[r1,#-8]!", 0xe971u, 0x2302u, MM_OP_LDRD,  2u, 1u, 3u, 0u },
    { "LDRD r2,r3,[r1],#8",   0xe8f1u, 0x2302u, MM_OP_LDRD,  2u, 1u, 3u, 0u },
};

static const mm_u16 undef_cases[][2] = {
    { 0xe8c1u, 0x2fbfu }, /* op 1011 */
    { 0xe8d0u, 0x3f7fu }, /* LDREXD: not in ARMv8-M */
    { 0xe8dfu, 0x3fafu }, /* LDA with Rn = PC */
    { 0xe8cfu, 0x2fafu }, /* STL with Rn = PC */
    { 0xe8dfu, 0x3fefu }, /* LDAEX with Rn = PC */
};

static int check_case(const struct case_t *c, int check_rm, int check_imm)
{
    struct mm_decoded dec;

    if (decode_insn(c->hw1, c->hw2, &dec) != 0) {
        printf("FAIL %s: fetch fault\n", c->name);
        return 1;
    }
    if (dec.undefined || dec.kind != c->kind) {
        printf("FAIL %s: kind %d undefined %d\n", c->name, (int)dec.kind,
            (int)dec.undefined);
        return 1;
    }
    if (c->kind != MM_OP_TBB && c->kind != MM_OP_TBH &&
        (dec.rd != c->rd || dec.rn != c->rn)) {
        printf("FAIL %s: rd %u rn %u\n", c->name, dec.rd, dec.rn);
        return 1;
    }
    if (check_rm && dec.rm != c->rm) {
        printf("FAIL %s: rm %u\n", c->name, dec.rm);
        return 1;
    }
    if (check_imm && dec.imm != c->imm) {
        printf("FAIL %s: imm 0x%lx\n", c->name, (unsigned long)dec.imm);
        return 1;
    }
    return 0;
}

int main(void)
{
    struct mm_decoded dec;
    size_t i;
    int fails = 0;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        if ((cases[i].kind == MM_OP_LDA || cases[i].kind == MM_OP_STL) &&
            decode_insn(cases[i].hw1, cases[i].hw2, &dec) == 0) {
            mm_u8 want = (mm_u8)(1u << ((cases[i].hw2 >> 4) & 0x3u));
            if (dec.ra != want) {
                printf("FAIL %s: size %u\n", cases[i].name, dec.ra);
                fails++;
            }
        }
    }
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        int is_store_ex = (cases[i].kind == MM_OP_STREX ||
            cases[i].kind == MM_OP_STREXB || cases[i].kind == MM_OP_STREXH);
        fails += check_case(&cases[i], is_store_ex, 1);
    }
    for (i = 0; i < sizeof(dual_cases) / sizeof(dual_cases[0]); ++i) {
        fails += check_case(&dual_cases[i], 1, 0);
    }
    for (i = 0; i < sizeof(undef_cases) / sizeof(undef_cases[0]); ++i) {
        if (decode_insn(undef_cases[i][0], undef_cases[i][1], &dec) != 0 ||
            !dec.undefined) {
            printf("FAIL undef %04x %04x: kind %d\n", undef_cases[i][0],
                undef_cases[i][1], (int)dec.kind);
            fails++;
        }
    }
    if (fails != 0) {
        printf("decode_acqrel_test: %d failure(s)\n", fails);
        return 1;
    }
    printf("decode_acqrel_test: PASS\n");
    return 0;
}
