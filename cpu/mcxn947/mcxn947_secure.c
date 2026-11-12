/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "mcxn947/mcxn947_secure.h"
#include "mcxn947/mcxn947_mmio.h"
#include "m33mu/mmio.h"
#include "m33mu/memmap.h"

#ifdef M33MU_HAS_WOLFSSL
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/sha256.h>
#include <wolfssl/wolfcrypt/hmac.h>
#endif

#define MCXN947_PUF_BASE    0x40024000u
#define MCXN947_OTPC_BASE   0x40025000u
#define MCXN947_ELS_BASE    0x40026000u
#define MCXN947_PKC_BASE    0x4002B000u
#define MCXN947_PRINCE_BASE 0x4002C000u
#define MCXN947_SEC_ALIAS   0x10000000u
#define MCXN947_SEC_SIZE    0x1000u

#define SEC_REG_CMD        0x000u
#define SEC_REG_STATUS     0x004u
#define SEC_REG_ARG0       0x008u
#define SEC_REG_ARG1       0x00Cu
#define SEC_REG_ARG2       0x010u
#define SEC_REG_ARG3       0x014u
#define SEC_REG_RESULT0    0x020u
#define SEC_REG_RESULT1    0x024u
#define SEC_REG_RESULT2    0x028u
#define SEC_REG_RESULT3    0x02Cu
#define SEC_REG_KEYIN0     0x080u
#define SEC_REG_KEYIN1     0x084u
#define SEC_REG_KEYIN2     0x088u
#define SEC_REG_KEYIN3     0x08Cu
#define SEC_REG_DATA0      0x100u
#define SEC_REG_DATA1      0x104u
#define SEC_REG_DATA2      0x108u
#define SEC_REG_DATA3      0x10Cu

#define SEC_STATUS_BUSY    (1u << 0)
#define SEC_STATUS_DONE    (1u << 1)
#define SEC_STATUS_ERROR   (1u << 2)
#define SEC_STATUS_SECURE  (1u << 3)

#define PUF_CMD_ENROLL     0x1u
#define PUF_CMD_DERIVE     0x2u

#define ELS_CMD_GENERATE   0x1u
#define ELS_CMD_DERIVE     0x2u
#define ELS_CMD_IMPORT     0x3u
#define ELS_CMD_SIGN       0x4u
#define ELS_CMD_VERIFY     0x5u
#define ELS_CMD_ATTEST     0x6u
#define ELS_CMD_RNG        0x7u

#define PKC_CMD_SIGN       0x1u
#define PKC_CMD_VERIFY     0x2u
#define PKC_CMD_KEYGEN     0x3u

#define OTPC_CMD_PROGRAM   0x1u

#define PRINCE_CMD_CONFIG  0x1u

#define MCXN947_OTP_WORDS  128u
#define MCXN947_ELS_SLOTS  8u

struct mcxn947_secure_regs {
    mm_u32 cmd;
    mm_u32 status;
    mm_u32 arg[4];
    mm_u32 result[4];
    mm_u32 keyin[4];
    mm_u32 data[4];
};

struct mcxn947_secure_slot {
    mm_bool valid;
    mm_u32 permissions;
    mm_u8 key[32];
};

struct mcxn947_secure_persist {
    mm_bool initialized;
    mm_bool preserve_identity;
    mm_u8 uds[32];
    mm_u32 otp[MCXN947_OTP_WORDS];
    mm_u8 puf_master[32];
    struct mcxn947_secure_slot slots[MCXN947_ELS_SLOTS];
};

struct mcxn947_secure_state {
    struct mcxn947_secure_regs puf;
    struct mcxn947_secure_regs els;
    struct mcxn947_secure_regs pkc;
    struct mcxn947_secure_regs otpc;
    struct mcxn947_secure_regs prince;
    struct mcxn947_secure_persist persist;
    mm_u8 *flash;
    mm_u32 flash_size;
    mm_u32 lifecycle;
    mm_u32 att_seed;
    mm_u32 nonce;
    mm_bool puf_enrolled;
    mm_u8 measurement[32];
    mm_u8 cdi[32];
    mm_u8 attest_pubkey[32];
    mm_u8 attest_sig[32];
};

enum mcxn947_secure_block {
    MCXN947_BLOCK_PUF = 0,
    MCXN947_BLOCK_OTPC = 1,
    MCXN947_BLOCK_ELS = 2,
    MCXN947_BLOCK_PKC = 3,
    MCXN947_BLOCK_PRINCE = 4
};

static struct mcxn947_secure_state g_sec;

static mm_u32 read32le(const mm_u8 *buf)
{
    return ((mm_u32)buf[0]) |
           ((mm_u32)buf[1] << 8u) |
           ((mm_u32)buf[2] << 16u) |
           ((mm_u32)buf[3] << 24u);
}

static void write32le(mm_u8 *buf, mm_u32 value)
{
    buf[0] = (mm_u8)(value & 0xffu);
    buf[1] = (mm_u8)((value >> 8u) & 0xffu);
    buf[2] = (mm_u8)((value >> 16u) & 0xffu);
    buf[3] = (mm_u8)((value >> 24u) & 0xffu);
}

#ifdef M33MU_HAS_WOLFSSL
static void secure_sha256(mm_u8 out[32], const mm_u8 *data, mm_u32 len)
{
    wc_Sha256 sha;
    wc_InitSha256(&sha);
    wc_Sha256Update(&sha, data, len);
    wc_Sha256Final(&sha, out);
}

static void secure_hmac(mm_u8 out[32], const mm_u8 *key, mm_u32 key_len,
                        const mm_u8 *data, mm_u32 len)
{
    Hmac hmac;
    wc_HmacInit(&hmac, 0, INVALID_DEVID);
    wc_HmacSetKey(&hmac, WC_SHA256, key, key_len);
    wc_HmacUpdate(&hmac, data, len);
    wc_HmacFinal(&hmac, out);
    wc_HmacFree(&hmac);
}
#else
static void secure_sha256(mm_u8 out[32], const mm_u8 *data, mm_u32 len)
{
    mm_u32 acc;
    mm_u32 i;
    mm_u32 j;
    acc = 0x811C9DC5u;
    for (i = 0u; i < len; ++i) {
        acc ^= data[i];
        acc *= 16777619u;
        acc = (acc << 5u) | (acc >> 27u);
    }
    for (j = 0u; j < 32u; j += 4u) {
        acc ^= (0x9E3779B9u + j);
        acc *= 2246822519u;
        write32le(out + j, acc);
    }
}

static void secure_hmac(mm_u8 out[32], const mm_u8 *key, mm_u32 key_len,
                        const mm_u8 *data, mm_u32 len)
{
    mm_u8 buf[96];
    mm_u32 copy;
    copy = (key_len > 32u) ? 32u : key_len;
    memset(buf, 0, sizeof(buf));
    memcpy(buf, key, copy);
    if (len > 64u) len = 64u;
    memcpy(buf + 32u, data, len);
    secure_sha256(out, buf, 32u + len);
}
#endif

void mm_mcxn947_secure_kdf(mm_u8 out[32], const mm_u8 *key, mm_u32 key_len,
                           const mm_u8 *data, mm_u32 len)
{
    secure_hmac(out, key, key_len, data, len);
}

static void secure_mix_label(mm_u8 out[32], const mm_u8 *seed,
                             const char *label, mm_u32 a, mm_u32 b)
{
    mm_u8 buf[96];
    size_t label_len;
    memset(buf, 0, sizeof(buf));
    memcpy(buf, seed, 32u);
    label_len = strlen(label);
    if (label_len > 48u) label_len = 48u;
    memcpy(buf + 32u, label, label_len);
    write32le(buf + 80u, a);
    write32le(buf + 84u, b);
    secure_sha256(out, buf, 88u);
}

static mm_u32 secure_parse_hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return (mm_u32)(c - '0');
    if (c >= 'a' && c <= 'f') return (mm_u32)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (mm_u32)(c - 'A' + 10);
    return 0xFFFFFFFFu;
}

static mm_bool secure_parse_hex32(const char *hex, mm_u8 out[32])
{
    mm_u32 i;
    if (hex == 0) return MM_FALSE;
    if (strlen(hex) != 64u) return MM_FALSE;
    for (i = 0u; i < 32u; ++i) {
        mm_u32 hi = secure_parse_hex_nibble(hex[i * 2u]);
        mm_u32 lo = secure_parse_hex_nibble(hex[i * 2u + 1u]);
        if (hi > 0xFu || lo > 0xFu) return MM_FALSE;
        out[i] = (mm_u8)((hi << 4u) | lo);
    }
    return MM_TRUE;
}

static mm_u32 secure_env_u32(const char *name, mm_u32 fallback)
{
    const char *value;
    char *end;
    unsigned long parsed;
    value = getenv(name);
    if (value == 0 || value[0] == '\0') return fallback;
    parsed = strtoul(value, &end, 0);
    if (end == value) return fallback;
    return (mm_u32)parsed;
}

static mm_bool secure_env_bool(const char *name, mm_bool fallback)
{
    const char *value;
    value = getenv(name);
    if (value == 0 || value[0] == '\0') return fallback;
    if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0) return MM_FALSE;
    if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0) return MM_TRUE;
    return fallback;
}

static mm_u32 secure_lifecycle_from_env(void)
{
    const char *value;
    value = getenv("M33MU_MCXN947_LIFECYCLE");
    if (value == 0 || value[0] == '\0') return MCXN947_LCS_DM;
    if (strcmp(value, "cm") == 0) return MCXN947_LCS_CM;
    if (strcmp(value, "dm") == 0) return MCXN947_LCS_DM;
    if (strcmp(value, "se") == 0) return MCXN947_LCS_SE;
    if (strcmp(value, "rma") == 0) return MCXN947_LCS_RMA;
    return secure_env_u32("M33MU_MCXN947_LIFECYCLE", MCXN947_LCS_DM) & 0x3u;
}

static void secure_build_defaults(struct mcxn947_secure_state *s)
{
    const char *uds_hex;
    mm_u8 seed[32];
    uds_hex = getenv("M33MU_MCXN947_UDS_HEX");
    if (!s->persist.initialized || !s->persist.preserve_identity) {
        memset(&s->persist, 0, sizeof(s->persist));
        s->persist.preserve_identity = secure_env_bool("M33MU_MCXN947_PRESERVE_IDENTITY", MM_TRUE);
    }
    if (!s->persist.initialized) {
        memset(seed, 0, sizeof(seed));
        memcpy(seed, "m33mu-mcxn947-default-uds", 24u);
        if (uds_hex == 0 || !secure_parse_hex32(uds_hex, s->persist.uds)) {
            secure_sha256(s->persist.uds, seed, sizeof(seed));
        }
        secure_mix_label(s->persist.puf_master, s->persist.uds, "puf-master", 0u, 0u);
        s->persist.otp[0] = 0x4D43584Eu;
        s->persist.otp[1] = 0x00000947u;
        s->persist.otp[25] = 0u;
        s->persist.otp[43] = 0x01010101u;
        s->persist.initialized = MM_TRUE;
    }
    s->lifecycle = secure_lifecycle_from_env();
    s->att_seed = secure_env_u32("M33MU_MCXN947_ATTEST_SEED", 0x947A11u);
}

static void secure_measure_flash(struct mcxn947_secure_state *s)
{
    mm_u8 header[16];
    mm_u32 use_len;
    memset(header, 0, sizeof(header));
    write32le(header + 0u, s->flash_size);
    write32le(header + 4u, s->lifecycle);
    write32le(header + 8u, s->persist.otp[25]);
    write32le(header + 12u, s->att_seed);
    if (s->flash == 0 || s->flash_size == 0u) {
        secure_sha256(s->measurement, header, sizeof(header));
        return;
    }
    use_len = s->flash_size;
    if (use_len > 0x10000u) use_len = 0x10000u;
    secure_sha256(s->measurement, s->flash, use_len);
    secure_hmac(s->measurement, s->measurement, 32u, header, sizeof(header));
}

static void secure_refresh_identity(struct mcxn947_secure_state *s)
{
    mm_u8 material[64];
    secure_measure_flash(s);
    memset(material, 0, sizeof(material));
    memcpy(material, s->measurement, 32u);
    write32le(material + 32u, s->lifecycle);
    write32le(material + 36u, s->persist.otp[25]);
    write32le(material + 40u, s->att_seed);
    secure_hmac(s->cdi, s->persist.uds, 32u, material, 44u);
    secure_mix_label(s->attest_pubkey, s->cdi, "idk-pub", s->lifecycle, s->persist.otp[25]);
    secure_hmac(s->attest_sig, s->cdi, 32u, s->measurement, 32u);
}

static void secure_reset_regs(struct mcxn947_secure_regs *r)
{
    memset(r, 0, sizeof(*r));
    r->status = SEC_STATUS_SECURE;
}

static void secure_finish(struct mcxn947_secure_regs *r, mm_bool ok)
{
    r->status &= ~SEC_STATUS_BUSY;
    r->status &= ~SEC_STATUS_DONE;
    r->status &= ~SEC_STATUS_ERROR;
    r->status |= ok ? SEC_STATUS_DONE : SEC_STATUS_ERROR;
}

static void secure_regs_writeback(struct mcxn947_secure_regs *r,
                                  const mm_u8 digest[32])
{
    r->result[0] = read32le(digest + 0u);
    r->result[1] = read32le(digest + 4u);
    r->result[2] = read32le(digest + 8u);
    r->result[3] = read32le(digest + 12u);
    r->data[0] = read32le(digest + 16u);
    r->data[1] = read32le(digest + 20u);
    r->data[2] = read32le(digest + 24u);
    r->data[3] = read32le(digest + 28u);
}

static mm_bool secure_slot_in_range(mm_u32 slot)
{
    return slot < MCXN947_ELS_SLOTS ? MM_TRUE : MM_FALSE;
}

static void secure_exec_puf(struct mcxn947_secure_state *s)
{
    mm_u8 seed[32];
    mm_u32 slot;
    slot = s->puf.arg[0];
    if (s->puf.cmd == PUF_CMD_ENROLL) {
        s->puf_enrolled = MM_TRUE;
        s->puf.result[0] = 1u;
        s->puf.result[1] = 0x50554631u;
        secure_finish(&s->puf, MM_TRUE);
        return;
    }
    if (s->puf.cmd != PUF_CMD_DERIVE || !s->puf_enrolled || !secure_slot_in_range(slot)) {
        secure_finish(&s->puf, MM_FALSE);
        return;
    }
    memset(seed, 0, sizeof(seed));
    write32le(seed + 0u, slot);
    write32le(seed + 4u, s->puf.keyin[0]);
    write32le(seed + 8u, s->puf.keyin[1]);
    write32le(seed + 12u, s->puf.keyin[2]);
    write32le(seed + 16u, s->puf.keyin[3]);
    secure_hmac(s->persist.slots[slot].key, s->persist.puf_master, 32u, seed, 20u);
    s->persist.slots[slot].valid = MM_TRUE;
    s->persist.slots[slot].permissions = 0xFFFFFFFFu;
    s->puf.result[0] = 0xCAFE0000u | slot;
    s->puf.result[1] = read32le(s->persist.slots[slot].key + 0u);
    s->puf.result[2] = read32le(s->persist.slots[slot].key + 4u);
    s->puf.result[3] = read32le(s->persist.slots[slot].key + 8u);
    secure_finish(&s->puf, MM_TRUE);
}

static mm_bool secure_els_sign_from_slot(mm_u32 slot, const mm_u8 *msg,
                                         mm_u32 len, mm_u8 sig_out[32])
{
    if (!secure_slot_in_range(slot)) return MM_FALSE;
    if (!g_sec.persist.slots[slot].valid) return MM_FALSE;
    secure_hmac(sig_out, g_sec.persist.slots[slot].key, 32u, msg, len);
    return MM_TRUE;
}

static void secure_exec_els(struct mcxn947_secure_state *s)
{
    mm_u32 slot;
    mm_u8 msg[32];
    mm_u8 sig[32];
    struct mcxn947_attestation_blob blob;
    slot = s->els.arg[0];
    memset(msg, 0, sizeof(msg));
    write32le(msg + 0u, s->els.keyin[0]);
    write32le(msg + 4u, s->els.keyin[1]);
    write32le(msg + 8u, s->els.keyin[2]);
    write32le(msg + 12u, s->els.keyin[3]);
    if (!mm_mcxn947_secure_els_clock_ready()) {
        secure_finish(&s->els, MM_FALSE);
        return;
    }
    switch (s->els.cmd) {
    case ELS_CMD_GENERATE:
        if (!secure_slot_in_range(slot)) {
            secure_finish(&s->els, MM_FALSE);
            return;
        }
        secure_mix_label(s->persist.slots[slot].key, s->cdi, "els-generate", slot, s->nonce++);
        s->persist.slots[slot].valid = MM_TRUE;
        s->persist.slots[slot].permissions = 0xFFFFFFFFu;
        secure_regs_writeback(&s->els, s->persist.slots[slot].key);
        secure_finish(&s->els, MM_TRUE);
        return;
    case ELS_CMD_DERIVE:
        if (!secure_slot_in_range(slot)) {
            secure_finish(&s->els, MM_FALSE);
            return;
        }
        secure_hmac(s->persist.slots[slot].key, s->cdi, 32u, msg, 16u);
        s->persist.slots[slot].valid = MM_TRUE;
        s->persist.slots[slot].permissions = 0xFFFFFFFFu;
        secure_regs_writeback(&s->els, s->persist.slots[slot].key);
        secure_finish(&s->els, MM_TRUE);
        return;
    case ELS_CMD_IMPORT:
    {
        mm_u8 tail[32];
        if (!secure_slot_in_range(slot)) {
            secure_finish(&s->els, MM_FALSE);
            return;
        }
        memcpy(s->persist.slots[slot].key + 0u, &s->els.keyin[0], 4u);
        memcpy(s->persist.slots[slot].key + 4u, &s->els.keyin[1], 4u);
        memcpy(s->persist.slots[slot].key + 8u, &s->els.keyin[2], 4u);
        memcpy(s->persist.slots[slot].key + 12u, &s->els.keyin[3], 4u);
        secure_mix_label(tail, s->persist.slots[slot].key, "els-import", slot, 0u);
        memcpy(s->persist.slots[slot].key + 16u, tail, 16u);
        s->persist.slots[slot].valid = MM_TRUE;
        s->persist.slots[slot].permissions = 0xFFFFFFFFu;
        secure_regs_writeback(&s->els, s->persist.slots[slot].key);
        secure_finish(&s->els, MM_TRUE);
        return;
    }
    case ELS_CMD_SIGN:
        if (!secure_els_sign_from_slot(slot, msg, 16u, sig)) {
            secure_finish(&s->els, MM_FALSE);
            return;
        }
        secure_regs_writeback(&s->els, sig);
        secure_finish(&s->els, MM_TRUE);
        return;
    case ELS_CMD_VERIFY:
        if (!secure_els_sign_from_slot(slot, msg, 16u, sig)) {
            secure_finish(&s->els, MM_FALSE);
            return;
        }
        if (memcmp(sig, &s->els.data[0], 16u) == 0 &&
            memcmp(sig + 16u, &s->els.result[0], 16u) == 0) {
            s->els.result[0] = 1u;
            secure_finish(&s->els, MM_TRUE);
        } else {
            s->els.result[0] = 0u;
            secure_finish(&s->els, MM_FALSE);
        }
        return;
    case ELS_CMD_ATTEST:
        if (!mm_mcxn947_secure_attest(&blob)) {
            secure_finish(&s->els, MM_FALSE);
            return;
        }
        s->els.result[0] = read32le(blob.measurement + 0u);
        s->els.result[1] = read32le(blob.measurement + 4u);
        s->els.result[2] = read32le(blob.measurement + 8u);
        s->els.result[3] = read32le(blob.measurement + 12u);
        s->els.data[0] = read32le(blob.signature + 0u);
        s->els.data[1] = read32le(blob.signature + 4u);
        s->els.data[2] = read32le(blob.signature + 8u);
        s->els.data[3] = read32le(blob.signature + 12u);
        secure_finish(&s->els, MM_TRUE);
        return;
    case ELS_CMD_RNG:
        mm_mcxn947_secure_rng_fill((mm_u8 *)sig, sizeof(sig));
        secure_regs_writeback(&s->els, sig);
        secure_finish(&s->els, MM_TRUE);
        return;
    default:
        break;
    }
    secure_finish(&s->els, MM_FALSE);
}

static void secure_exec_pkc(struct mcxn947_secure_state *s)
{
    mm_u8 msg[32];
    mm_u8 sig[32];
    memset(msg, 0, sizeof(msg));
    write32le(msg + 0u, s->pkc.keyin[0]);
    write32le(msg + 4u, s->pkc.keyin[1]);
    write32le(msg + 8u, s->pkc.keyin[2]);
    write32le(msg + 12u, s->pkc.keyin[3]);
    switch (s->pkc.cmd) {
    case PKC_CMD_KEYGEN:
        secure_regs_writeback(&s->pkc, s->attest_pubkey);
        secure_finish(&s->pkc, MM_TRUE);
        return;
    case PKC_CMD_SIGN:
        secure_hmac(sig, s->cdi, 32u, msg, 16u);
        secure_regs_writeback(&s->pkc, sig);
        secure_finish(&s->pkc, MM_TRUE);
        return;
    case PKC_CMD_VERIFY:
        secure_hmac(sig, s->cdi, 32u, msg, 16u);
        s->pkc.result[0] = (memcmp(sig, &s->pkc.data[0], 16u) == 0 &&
                            memcmp(sig + 16u, &s->pkc.result[1], 12u) == 0) ? 1u : 0u;
        secure_finish(&s->pkc, s->pkc.result[0] ? MM_TRUE : MM_FALSE);
        return;
    default:
        break;
    }
    secure_finish(&s->pkc, MM_FALSE);
}

static void secure_exec_otpc(struct mcxn947_secure_state *s)
{
    mm_u32 index;
    index = s->otpc.arg[0];
    if (index >= MCXN947_OTP_WORDS) {
        secure_finish(&s->otpc, MM_FALSE);
        return;
    }
    if (s->otpc.cmd != OTPC_CMD_PROGRAM) {
        secure_finish(&s->otpc, MM_FALSE);
        return;
    }
    s->persist.otp[index] |= s->otpc.arg[1];
    if (index == 25u) {
        secure_refresh_identity(s);
    }
    s->otpc.result[0] = s->persist.otp[index];
    secure_finish(&s->otpc, MM_TRUE);
}

static void secure_exec_prince(struct mcxn947_secure_state *s)
{
    if (s->prince.cmd != PRINCE_CMD_CONFIG) {
        secure_finish(&s->prince, MM_FALSE);
        return;
    }
    s->persist.otp[26] = s->prince.arg[0];
    s->persist.otp[27] = s->prince.arg[1];
    s->prince.result[0] = s->persist.otp[26];
    s->prince.result[1] = s->persist.otp[27];
    secure_finish(&s->prince, MM_TRUE);
}

static void secure_exec(enum mcxn947_secure_block block)
{
    switch (block) {
    case MCXN947_BLOCK_PUF:
        secure_exec_puf(&g_sec);
        break;
    case MCXN947_BLOCK_OTPC:
        secure_exec_otpc(&g_sec);
        break;
    case MCXN947_BLOCK_ELS:
        secure_exec_els(&g_sec);
        break;
    case MCXN947_BLOCK_PKC:
        secure_exec_pkc(&g_sec);
        break;
    case MCXN947_BLOCK_PRINCE:
        secure_exec_prince(&g_sec);
        break;
    default:
        break;
    }
}

static mm_bool secure_access_ok(void)
{
    return (mmio_active_sec() == MM_SECURE) ? MM_TRUE : MM_FALSE;
}

static mm_bool secure_read_words(struct mcxn947_secure_regs *r, mm_u32 offset,
                                 mm_u32 size_bytes, mm_u32 *value_out)
{
    if (size_bytes != 4u || value_out == 0) return MM_FALSE;
    switch (offset) {
    case SEC_REG_CMD: *value_out = r->cmd; return MM_TRUE;
    case SEC_REG_STATUS: *value_out = r->status; return MM_TRUE;
    case SEC_REG_ARG0: *value_out = r->arg[0]; return MM_TRUE;
    case SEC_REG_ARG1: *value_out = r->arg[1]; return MM_TRUE;
    case SEC_REG_ARG2: *value_out = r->arg[2]; return MM_TRUE;
    case SEC_REG_ARG3: *value_out = r->arg[3]; return MM_TRUE;
    case SEC_REG_RESULT0: *value_out = r->result[0]; return MM_TRUE;
    case SEC_REG_RESULT1: *value_out = r->result[1]; return MM_TRUE;
    case SEC_REG_RESULT2: *value_out = r->result[2]; return MM_TRUE;
    case SEC_REG_RESULT3: *value_out = r->result[3]; return MM_TRUE;
    case SEC_REG_KEYIN0: *value_out = r->keyin[0]; return MM_TRUE;
    case SEC_REG_KEYIN1: *value_out = r->keyin[1]; return MM_TRUE;
    case SEC_REG_KEYIN2: *value_out = r->keyin[2]; return MM_TRUE;
    case SEC_REG_KEYIN3: *value_out = r->keyin[3]; return MM_TRUE;
    case SEC_REG_DATA0: *value_out = r->data[0]; return MM_TRUE;
    case SEC_REG_DATA1: *value_out = r->data[1]; return MM_TRUE;
    case SEC_REG_DATA2: *value_out = r->data[2]; return MM_TRUE;
    case SEC_REG_DATA3: *value_out = r->data[3]; return MM_TRUE;
    default:
        break;
    }
    return MM_FALSE;
}

static mm_bool secure_region_read(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                                  mm_u32 *value_out)
{
    struct mcxn947_secure_regs *r;
    mm_u32 idx;
    if (!secure_access_ok()) return MM_FALSE;
    r = (struct mcxn947_secure_regs *)opaque;
    if (r == 0) return MM_FALSE;
    if (secure_read_words(r, offset, size_bytes, value_out)) return MM_TRUE;
    if (r == &g_sec.otpc && size_bytes == 4u &&
        offset >= SEC_REG_DATA0 && offset < (SEC_REG_DATA0 + MCXN947_OTP_WORDS * 4u)) {
        idx = (offset - SEC_REG_DATA0) / 4u;
        if (idx < MCXN947_OTP_WORDS) {
            *value_out = g_sec.persist.otp[idx];
            return MM_TRUE;
        }
    }
    return MM_FALSE;
}

static mm_bool secure_region_write(void *opaque, mm_u32 offset, mm_u32 size_bytes,
                                   mm_u32 value)
{
    struct mcxn947_secure_regs *r;
    enum mcxn947_secure_block block;
    if (!secure_access_ok()) return MM_FALSE;
    if (size_bytes != 4u) return MM_FALSE;
    r = (struct mcxn947_secure_regs *)opaque;
    if (r == 0) return MM_FALSE;
    if (r == &g_sec.puf) block = MCXN947_BLOCK_PUF;
    else if (r == &g_sec.otpc) block = MCXN947_BLOCK_OTPC;
    else if (r == &g_sec.els) block = MCXN947_BLOCK_ELS;
    else if (r == &g_sec.pkc) block = MCXN947_BLOCK_PKC;
    else block = MCXN947_BLOCK_PRINCE;

    switch (offset) {
    case SEC_REG_CMD:
        r->cmd = value;
        r->status |= SEC_STATUS_BUSY;
        r->status &= ~(SEC_STATUS_DONE | SEC_STATUS_ERROR);
        secure_exec(block);
        return MM_TRUE;
    case SEC_REG_STATUS:
        r->status &= ~value;
        return MM_TRUE;
    case SEC_REG_ARG0: r->arg[0] = value; return MM_TRUE;
    case SEC_REG_ARG1: r->arg[1] = value; return MM_TRUE;
    case SEC_REG_ARG2: r->arg[2] = value; return MM_TRUE;
    case SEC_REG_ARG3: r->arg[3] = value; return MM_TRUE;
    case SEC_REG_KEYIN0: r->keyin[0] = value; return MM_TRUE;
    case SEC_REG_KEYIN1: r->keyin[1] = value; return MM_TRUE;
    case SEC_REG_KEYIN2: r->keyin[2] = value; return MM_TRUE;
    case SEC_REG_KEYIN3: r->keyin[3] = value; return MM_TRUE;
    case SEC_REG_DATA0: r->data[0] = value; return MM_TRUE;
    case SEC_REG_DATA1: r->data[1] = value; return MM_TRUE;
    case SEC_REG_DATA2: r->data[2] = value; return MM_TRUE;
    case SEC_REG_DATA3: r->data[3] = value; return MM_TRUE;
    default:
        break;
    }
    return MM_FALSE;
}

static mm_bool secure_register_pair(struct mmio_bus *bus, mm_u32 base,
                                    struct mcxn947_secure_regs *regs)
{
    struct mmio_region reg;
    memset(&reg, 0, sizeof(reg));
    reg.base = base;
    reg.size = MCXN947_SEC_SIZE;
    reg.opaque = regs;
    reg.read = secure_region_read;
    reg.write = secure_region_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = base + MCXN947_SEC_ALIAS;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    return MM_TRUE;
}

void mm_mcxn947_secure_reset(void)
{
    secure_build_defaults(&g_sec);
    secure_reset_regs(&g_sec.puf);
    secure_reset_regs(&g_sec.otpc);
    secure_reset_regs(&g_sec.els);
    secure_reset_regs(&g_sec.pkc);
    secure_reset_regs(&g_sec.prince);
    g_sec.puf.status |= SEC_STATUS_DONE;
    g_sec.otpc.status |= SEC_STATUS_DONE;
    g_sec.els.status |= SEC_STATUS_DONE;
    g_sec.pkc.status |= SEC_STATUS_DONE;
    g_sec.prince.status |= SEC_STATUS_DONE;
    g_sec.otpc.result[0] = g_sec.persist.otp[0];
    g_sec.prince.result[0] = g_sec.persist.otp[26];
    g_sec.prince.result[1] = g_sec.persist.otp[27];
    g_sec.puf_enrolled = MM_TRUE;
    secure_refresh_identity(&g_sec);
}

mm_bool mm_mcxn947_secure_register_mmio(struct mmio_bus *bus)
{
    if (bus == 0) return MM_FALSE;
    if (!secure_register_pair(bus, MCXN947_PUF_BASE, &g_sec.puf)) return MM_FALSE;
    if (!secure_register_pair(bus, MCXN947_OTPC_BASE, &g_sec.otpc)) return MM_FALSE;
    if (!secure_register_pair(bus, MCXN947_ELS_BASE, &g_sec.els)) return MM_FALSE;
    if (!secure_register_pair(bus, MCXN947_PKC_BASE, &g_sec.pkc)) return MM_FALSE;
    if (!secure_register_pair(bus, MCXN947_PRINCE_BASE, &g_sec.prince)) return MM_FALSE;
    return MM_TRUE;
}

void mm_mcxn947_secure_flash_bind(struct mm_memmap *map,
                                  mm_u8 *flash,
                                  mm_u32 flash_size,
                                  const struct mm_flash_persist *persist,
                                  mm_u32 flags)
{
    (void)map;
    (void)persist;
    (void)flags;
    g_sec.flash = flash;
    g_sec.flash_size = flash_size;
    secure_refresh_identity(&g_sec);
}

mm_bool mm_mcxn947_secure_otp_read(mm_u32 index, mm_u32 *value_out)
{
    if (value_out == 0 || index >= MCXN947_OTP_WORDS) return MM_FALSE;
    *value_out = g_sec.persist.otp[index];
    return MM_TRUE;
}

mm_bool mm_mcxn947_secure_otp_program(mm_u32 index, mm_u32 value)
{
    if (index >= MCXN947_OTP_WORDS) return MM_FALSE;
    g_sec.persist.otp[index] |= value;
    if (index == 25u) secure_refresh_identity(&g_sec);
    return MM_TRUE;
}

void mm_mcxn947_secure_rng_fill(mm_u8 *out, mm_u32 len)
{
    mm_u8 block[32];
    mm_u32 pos;
    mm_u32 chunk;
    if (out == 0) return;
    pos = 0u;
    while (pos < len) {
        secure_mix_label(block, g_sec.persist.uds, "rng", g_sec.att_seed, g_sec.nonce++);
        chunk = len - pos;
        if (chunk > sizeof(block)) chunk = sizeof(block);
        memcpy(out + pos, block, chunk);
        pos += chunk;
    }
}

mm_bool mm_mcxn947_secure_els_clock_ready(void)
{
    return mm_mcxn947_syscon_clock_bit_on(0x200u, 25u) &&
           mm_mcxn947_syscon_reset_bit_released(0x100u, 25u);
}

mm_bool mm_mcxn947_secure_rom_call_allowed(enum mm_sec_state sec)
{
    return (sec == MM_SECURE) ? MM_TRUE : MM_FALSE;
}

enum mcxn947_lifecycle_state mm_mcxn947_secure_lifecycle(void)
{
    return (enum mcxn947_lifecycle_state)(g_sec.lifecycle & 0x3u);
}

mm_bool mm_mcxn947_secure_measurement(mm_u8 out[32])
{
    if (out == 0) return MM_FALSE;
    secure_refresh_identity(&g_sec);
    memcpy(out, g_sec.measurement, 32u);
    return MM_TRUE;
}

mm_bool mm_mcxn947_secure_attest(struct mcxn947_attestation_blob *blob_out)
{
    if (blob_out == 0) return MM_FALSE;
    secure_refresh_identity(&g_sec);
    memcpy(blob_out->measurement, g_sec.measurement, 32u);
    memcpy(blob_out->cdi, g_sec.cdi, 32u);
    memcpy(blob_out->pubkey, g_sec.attest_pubkey, 32u);
    memcpy(blob_out->signature, g_sec.attest_sig, 32u);
    return MM_TRUE;
}
