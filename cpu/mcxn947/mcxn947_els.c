/* m33mu -- an ARMv8-M Emulator
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/* NXP MCXN947 EdgeLock Secure Enclave (S50 ELS): the SFR command interface
 * with the commands wolfBoot issues through mcuxClEls (RND_REQ, KDELETE,
 * HKDF, KEYGEN P-256, ECSIGN); commands complete synchronously. */
#include <string.h>
#include "mcxn947/mcxn947_els.h"
#include "mcxn947/mcxn947_mmio.h"
#include "mcxn947/mcxn947_secure.h"
#include "m33mu/mmio.h"
#include "m33mu/memmap.h"

#ifdef M33MU_HAS_WOLFSSL
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/ecc.h>
#include <wolfssl/wolfcrypt/random.h>
#endif

#define ELS_SIZE              0x1A0u

#define ELS_STATUS            0x000u
#define ELS_CTRL              0x004u
#define ELS_CMDCFG0           0x008u
#define ELS_CFG               0x00Cu
#define ELS_KIDX0             0x010u
#define ELS_KIDX1             0x014u
#define ELS_KPROPIN           0x018u
#define ELS_DMA_SRC0          0x020u
#define ELS_DMA_SRC0_LEN      0x024u
#define ELS_DMA_SRC1          0x028u
#define ELS_DMA_SRC2          0x030u
#define ELS_DMA_SRC2_LEN      0x034u
#define ELS_DMA_RES0          0x038u
#define ELS_DMA_RES0_LEN      0x03Cu
#define ELS_INT_ENABLE        0x040u
#define ELS_INT_STATUS_CLR    0x044u
#define ELS_INT_STATUS_SET    0x048u
#define ELS_ERR_STATUS        0x04Cu
#define ELS_ERR_STATUS_CLR    0x050u
#define ELS_VERSION           0x054u
#define ELS_PRNG_DATOUT       0x05Cu
#define ELS_CMDCRC_CTRL       0x060u
#define ELS_CMDCRC            0x064u
#define ELS_SESSION_ID        0x068u
#define ELS_DMA_FIN_ADDR      0x070u
#define ELS_MASTER_ID         0x074u
#define ELS_KIDX2             0x078u
#define ELS_KS0               0x150u

#define ELS_STATUS_BUSY       (1u << 0)
#define ELS_STATUS_IRQ        (1u << 1)
#define ELS_STATUS_ERR        (1u << 2)
#define ELS_STATUS_PRNG_RDY   (1u << 3)
#define ELS_STATUS_ENT_LVL    (3u << 8)

#define ELS_CTRL_EN           (1u << 0)
#define ELS_CTRL_START        (1u << 1)
#define ELS_CTRL_RESET        (1u << 2)
#define ELS_CTRL_CMD_SHIFT    3u
#define ELS_CTRL_CMD_MASK     (0x1Fu << ELS_CTRL_CMD_SHIFT)

#define ELS_ERR_BUS           (1u << 0)
#define ELS_ERR_OPN           (1u << 1)
#define ELS_ERR_ALG           (1u << 2)

#define ELS_KS_KACT           (1u << 5)
#define ELS_KIDX_MASK         0x1Fu

#define ELS_CMD_ECSIGN        4u
#define ELS_CMD_KEYGEN        8u
#define ELS_CMD_KDELETE       11u
#define ELS_CMD_HKDF          17u
#define ELS_CMD_RND_REQ       24u

#define ELS_KEYGEN_KGSRC_RANDOM (1u << 2)
#define ELS_ECSIGN_ECHASHCHL    (1u << 0)

/* ROM DICE leaves HKDF(UDS, wolfBoot hash) in this slot before boot */
#define ELS_ROM_CDI_SLOT      7u
#define ELS_ROM_CDI_PROPS     0x80020060u /* S | UHKDF | KBASE | KACT | 256-bit */

#define ELS_VERSION_VALUE     0x00000300u

struct els_slot {
    mm_bool active;
    mm_u32 props;
    mm_u8 key[32];
};

struct els_state {
    mm_u32 regs[ELS_SIZE / 4u];
    mm_u32 err;
    mm_bool enabled;
    mm_bool irq;
    struct els_slot slots[MCXN947_ELS_KEYSLOTS];
    struct mm_memmap *map;
};

static struct els_state g_els;

static void els_set_error(struct els_state *e, mm_u32 err)
{
    e->err |= err;
}

static mm_bool els_slot_ok(mm_u32 slot)
{
    return slot < MCXN947_ELS_KEYSLOTS ? MM_TRUE : MM_FALSE;
}

static void els_slot_set(struct els_state *e, mm_u32 slot, const mm_u8 key[32], mm_u32 props)
{
    e->slots[slot].active = MM_TRUE;
    e->slots[slot].props = props | ELS_KS_KACT;
    memcpy(e->slots[slot].key, key, 32u);
}

static void els_slot_clear(struct els_state *e, mm_u32 slot)
{
    memset(&e->slots[slot], 0, sizeof(e->slots[slot]));
}

static mm_bool els_dma_read(struct els_state *e, mm_u32 addr, mm_u8 *out, mm_u32 len)
{
    mm_u32 i;
    mm_u32 word;
    if (e->map == 0 || (addr & 3u) != 0u || (len & 3u) != 0u) return MM_FALSE;
    for (i = 0u; i < len; i += 4u) {
        if (!mm_memmap_read(e->map, MM_SECURE, addr + i, 4u, &word)) return MM_FALSE;
        out[i] = (mm_u8)(word & 0xFFu);
        out[i + 1u] = (mm_u8)((word >> 8) & 0xFFu);
        out[i + 2u] = (mm_u8)((word >> 16) & 0xFFu);
        out[i + 3u] = (mm_u8)((word >> 24) & 0xFFu);
    }
    return MM_TRUE;
}

static mm_bool els_dma_write(struct els_state *e, mm_u32 addr, const mm_u8 *in, mm_u32 len)
{
    mm_u32 i;
    mm_u32 word;
    if (e->map == 0 || (addr & 3u) != 0u || (len & 3u) != 0u) return MM_FALSE;
    for (i = 0u; i < len; i += 4u) {
        word = (mm_u32)in[i] | ((mm_u32)in[i + 1u] << 8) |
               ((mm_u32)in[i + 2u] << 16) | ((mm_u32)in[i + 3u] << 24);
        if (!mm_memmap_write_ram_raw(e->map, addr + i, 4u, word)) return MM_FALSE;
    }
    e->regs[ELS_DMA_FIN_ADDR / 4u] = addr + len;
    return MM_TRUE;
}

static void els_cmd_rnd_req(struct els_state *e)
{
    mm_u8 block[64];
    mm_u32 dst = e->regs[ELS_DMA_RES0 / 4u];
    mm_u32 len = e->regs[ELS_DMA_RES0_LEN / 4u];
    mm_u32 chunk;
    if (len == 0u || (len & 3u) != 0u || (dst & 3u) != 0u) {
        els_set_error(e, ELS_ERR_BUS);
        return;
    }
    while (len > 0u) {
        chunk = (len > sizeof(block)) ? (mm_u32)sizeof(block) : len;
        mm_mcxn947_secure_rng_fill(block, chunk);
        if (!els_dma_write(e, dst, block, chunk)) {
            els_set_error(e, ELS_ERR_BUS);
            return;
        }
        dst += chunk;
        len -= chunk;
    }
}

static void els_cmd_kdelete(struct els_state *e)
{
    mm_u32 slot = e->regs[ELS_KIDX0 / 4u] & ELS_KIDX_MASK;
    if (!els_slot_ok(slot)) {
        els_set_error(e, ELS_ERR_OPN);
        return;
    }
    els_slot_clear(e, slot);
}

static void els_cmd_hkdf(struct els_state *e)
{
    mm_u32 src = e->regs[ELS_KIDX0 / 4u] & ELS_KIDX_MASK;
    mm_u32 dst = e->regs[ELS_KIDX1 / 4u] & ELS_KIDX_MASK;
    mm_u8 data[32];
    mm_u8 out[32];
    if (!els_slot_ok(src) || !els_slot_ok(dst) || !e->slots[src].active) {
        els_set_error(e, ELS_ERR_OPN);
        return;
    }
    if (!els_dma_read(e, e->regs[ELS_DMA_SRC0 / 4u], data, sizeof(data))) {
        els_set_error(e, ELS_ERR_BUS);
        return;
    }
    mm_mcxn947_secure_kdf(out, e->slots[src].key, 32u, data, sizeof(data));
    els_slot_set(e, dst, out, e->regs[ELS_KPROPIN / 4u]);
}

#ifdef M33MU_HAS_WOLFSSL
/* Any 32-byte scalar whose top byte is below 0xFF is a valid P-256 private key */
static void els_scalar_from_material(mm_u8 priv[32], const mm_u8 *material, const char *label)
{
    mm_mcxn947_secure_kdf(priv, material, 32u, (const mm_u8 *)label, (mm_u32)strlen(label));
    if (priv[0] == 0xFFu) priv[0] = 0x7Fu;
}

static mm_bool els_ecc_load(ecc_key *key, const mm_u8 priv[32])
{
    if (wc_ecc_init(key) != 0) return MM_FALSE;
    if (wc_ecc_import_private_key_ex(priv, 32u, 0, 0u, key, ECC_SECP256R1) != 0) {
        wc_ecc_free(key);
        return MM_FALSE;
    }
    if (wc_ecc_make_pub(key, 0) != 0) {
        wc_ecc_free(key);
        return MM_FALSE;
    }
    return MM_TRUE;
}

static void els_cmd_keygen(struct els_state *e)
{
    mm_u32 dst = e->regs[ELS_KIDX0 / 4u] & ELS_KIDX_MASK;
    mm_u32 src = e->regs[ELS_KIDX1 / 4u] & ELS_KIDX_MASK;
    mm_u32 cfg = e->regs[ELS_CMDCFG0 / 4u];
    mm_u8 material[32];
    mm_u8 priv[32];
    mm_u8 pub[64];
    word32 qx_len = 32u;
    word32 qy_len = 32u;
    ecc_key key;
    if (!els_slot_ok(dst) || !els_slot_ok(src)) {
        els_set_error(e, ELS_ERR_OPN);
        return;
    }
    if ((cfg & ELS_KEYGEN_KGSRC_RANDOM) != 0u) {
        mm_mcxn947_secure_rng_fill(material, sizeof(material));
    }
    else if (e->slots[src].active) {
        memcpy(material, e->slots[src].key, sizeof(material));
    }
    else {
        els_set_error(e, ELS_ERR_OPN);
        return;
    }
    els_scalar_from_material(priv, material, "els-keygen");
    if (!els_ecc_load(&key, priv)) {
        els_set_error(e, ELS_ERR_ALG);
        return;
    }
    memset(pub, 0, sizeof(pub));
    if (wc_ecc_export_public_raw(&key, pub, &qx_len, pub + 32u, &qy_len) != 0) {
        wc_ecc_free(&key);
        els_set_error(e, ELS_ERR_ALG);
        return;
    }
    wc_ecc_free(&key);
    if (!els_dma_write(e, e->regs[ELS_DMA_RES0 / 4u], pub, sizeof(pub))) {
        els_set_error(e, ELS_ERR_BUS);
        return;
    }
    els_slot_set(e, dst, priv, e->regs[ELS_KPROPIN / 4u]);
}

static void els_cmd_ecsign(struct els_state *e)
{
    mm_u32 slot = e->regs[ELS_KIDX0 / 4u] & ELS_KIDX_MASK;
    mm_u32 cfg = e->regs[ELS_CMDCFG0 / 4u];
    mm_u8 hash[32];
    mm_u8 sig_der[80];
    mm_u8 sig[64];
    mm_u8 r[32];
    mm_u8 s[32];
    word32 sig_len = (word32)sizeof(sig_der);
    word32 r_len = 32u;
    word32 s_len = 32u;
    ecc_key key;
    WC_RNG rng;
    int rc;
    if (!els_slot_ok(slot) || !e->slots[slot].active || (cfg & ELS_ECSIGN_ECHASHCHL) != 0u) {
        els_set_error(e, ELS_ERR_OPN);
        return;
    }
    if (!els_dma_read(e, e->regs[ELS_DMA_SRC0 / 4u], hash, sizeof(hash))) {
        els_set_error(e, ELS_ERR_BUS);
        return;
    }
    if (!els_ecc_load(&key, e->slots[slot].key)) {
        els_set_error(e, ELS_ERR_ALG);
        return;
    }
    if (wc_InitRng(&rng) != 0) {
        wc_ecc_free(&key);
        els_set_error(e, ELS_ERR_ALG);
        return;
    }
    rc = wc_ecc_sign_hash(hash, sizeof(hash), sig_der, &sig_len, &rng, &key);
    wc_FreeRng(&rng);
    wc_ecc_free(&key);
    if (rc == 0) {
        rc = wc_ecc_sig_to_rs(sig_der, sig_len, r, &r_len, s, &s_len);
    }
    if (rc != 0 || r_len > 32u || s_len > 32u) {
        els_set_error(e, ELS_ERR_ALG);
        return;
    }
    memset(sig, 0, sizeof(sig));
    memcpy(sig + (32u - r_len), r, r_len);
    memcpy(sig + 32u + (32u - s_len), s, s_len);
    if (!els_dma_write(e, e->regs[ELS_DMA_RES0 / 4u], sig, sizeof(sig))) {
        els_set_error(e, ELS_ERR_BUS);
    }
}
#else
static void els_cmd_keygen(struct els_state *e)
{
    els_set_error(e, ELS_ERR_OPN);
}

static void els_cmd_ecsign(struct els_state *e)
{
    els_set_error(e, ELS_ERR_OPN);
}
#endif /* M33MU_HAS_WOLFSSL */

/* SYSCON AHBCLKCTRL2/PRESETCTRL2 bit 18 gate the ELS; the boot ROM leaves
 * them enabled, which the wolfBoot HAL relies on. */
static mm_bool els_clock_ready(void)
{
    return (mm_mcxn947_syscon_clock_bit_on(0x208u, 18u) &&
            mm_mcxn947_syscon_reset_bit_released(0x108u, 18u)) ? MM_TRUE : MM_FALSE;
}

static void els_execute(struct els_state *e, mm_u32 cmd)
{
    if (!e->enabled || !els_clock_ready()) {
        els_set_error(e, ELS_ERR_OPN);
    }
    else {
        switch (cmd) {
        case ELS_CMD_RND_REQ: els_cmd_rnd_req(e); break;
        case ELS_CMD_KDELETE: els_cmd_kdelete(e); break;
        case ELS_CMD_HKDF:    els_cmd_hkdf(e); break;
        case ELS_CMD_KEYGEN:  els_cmd_keygen(e); break;
        case ELS_CMD_ECSIGN:  els_cmd_ecsign(e); break;
        default:              els_set_error(e, ELS_ERR_OPN); break;
        }
    }
    e->irq = MM_TRUE;
}

static void els_provision_rom_cdi(struct els_state *e)
{
    struct mcxn947_attestation_blob blob;
    if (mm_mcxn947_secure_attest(&blob)) {
        els_slot_set(e, ELS_ROM_CDI_SLOT, blob.cdi, ELS_ROM_CDI_PROPS);
    }
}

static void els_hw_reset(struct els_state *e)
{
    struct mm_memmap *map = e->map;
    memset(e, 0, sizeof(*e));
    e->map = map;
    e->regs[ELS_VERSION / 4u] = ELS_VERSION_VALUE;
    els_provision_rom_cdi(e);
}

static mm_u32 els_status(const struct els_state *e)
{
    mm_u32 v = 0u;
    if (e->irq) v |= ELS_STATUS_IRQ;
    if (e->err != 0u) v |= ELS_STATUS_ERR;
    if (e->enabled) v |= ELS_STATUS_PRNG_RDY | ELS_STATUS_ENT_LVL;
    return v;
}

static mm_bool els_read(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 *value_out)
{
    struct els_state *e = (struct els_state *)opaque;
    mm_u8 word[4];
    mm_u32 slot;
    if (e == 0 || value_out == 0 || size_bytes != 4u) return MM_FALSE;
    if ((offset & 3u) != 0u || offset + size_bytes > ELS_SIZE) return MM_FALSE;
    switch (offset) {
    case ELS_STATUS:
        *value_out = els_status(e);
        return MM_TRUE;
    case ELS_ERR_STATUS:
        *value_out = e->err;
        return MM_TRUE;
    case ELS_PRNG_DATOUT:
        mm_mcxn947_secure_rng_fill(word, sizeof(word));
        *value_out = (mm_u32)word[0] | ((mm_u32)word[1] << 8) |
                     ((mm_u32)word[2] << 16) | ((mm_u32)word[3] << 24);
        return MM_TRUE;
    case ELS_INT_STATUS_CLR:
    case ELS_INT_STATUS_SET:
    case ELS_ERR_STATUS_CLR:
        *value_out = 0u;
        return MM_TRUE;
    default:
        break;
    }
    if (offset >= ELS_KS0) {
        slot = (offset - ELS_KS0) / 4u;
        *value_out = (slot < MCXN947_ELS_KEYSLOTS) ? e->slots[slot].props : 0u;
        return MM_TRUE;
    }
    *value_out = e->regs[offset / 4u];
    return MM_TRUE;
}

static mm_bool els_write(void *opaque, mm_u32 offset, mm_u32 size_bytes, mm_u32 value)
{
    struct els_state *e = (struct els_state *)opaque;
    if (e == 0 || size_bytes != 4u) return MM_FALSE;
    if ((offset & 3u) != 0u || offset + size_bytes > ELS_SIZE) return MM_FALSE;
    switch (offset) {
    case ELS_CTRL:
        if ((value & ELS_CTRL_RESET) != 0u) {
            els_hw_reset(e);
            return MM_TRUE;
        }
        e->enabled = ((value & ELS_CTRL_EN) != 0u) ? MM_TRUE : MM_FALSE;
        e->regs[ELS_CTRL / 4u] = value & ~(ELS_CTRL_START | ELS_CTRL_RESET);
        if ((value & ELS_CTRL_START) != 0u) {
            els_execute(e, (value & ELS_CTRL_CMD_MASK) >> ELS_CTRL_CMD_SHIFT);
        }
        return MM_TRUE;
    case ELS_KIDX0:
    case ELS_KIDX1:
    case ELS_KIDX2:
        e->regs[offset / 4u] = value & ELS_KIDX_MASK;
        return MM_TRUE;
    case ELS_INT_STATUS_CLR:
        if ((value & 1u) != 0u) e->irq = MM_FALSE;
        return MM_TRUE;
    case ELS_INT_STATUS_SET:
        if ((value & 1u) != 0u) e->irq = MM_TRUE;
        return MM_TRUE;
    case ELS_ERR_STATUS_CLR:
        if ((value & 1u) != 0u) e->err = 0u;
        return MM_TRUE;
    case ELS_STATUS:
    case ELS_ERR_STATUS:
    case ELS_VERSION:
    case ELS_PRNG_DATOUT:
    case ELS_CMDCRC:
    case ELS_DMA_FIN_ADDR:
        return MM_TRUE;
    default:
        break;
    }
    if (offset >= ELS_KS0) return MM_TRUE;
    e->regs[offset / 4u] = value;
    return MM_TRUE;
}

void mm_mcxn947_els_reset(void)
{
    g_els.map = 0;
    els_hw_reset(&g_els);
}

void mm_mcxn947_els_bind_memmap(struct mm_memmap *map)
{
    g_els.map = map;
    els_provision_rom_cdi(&g_els);
}

mm_bool mm_mcxn947_els_slot_active(mm_u32 slot)
{
    return (els_slot_ok(slot) && g_els.slots[slot].active) ? MM_TRUE : MM_FALSE;
}

mm_bool mm_mcxn947_els_register_mmio(struct mmio_bus *bus)
{
    struct mmio_region reg;
    if (bus == 0) return MM_FALSE;
    memset(&reg, 0, sizeof(reg));
    reg.base = MCXN947_ELS_BASE_NS;
    reg.size = ELS_SIZE;
    reg.opaque = &g_els;
    reg.read = els_read;
    reg.write = els_write;
    if (!mmio_bus_register_region(bus, &reg)) return MM_FALSE;
    reg.base = MCXN947_ELS_BASE_S;
    return mmio_bus_register_region(bus, &reg);
}
