#ifndef M33MU_MCXN947_SECURE_H
#define M33MU_MCXN947_SECURE_H

#include "m33mu/types.h"
#include "m33mu/cpu.h"

struct mmio_bus;
struct mm_memmap;
struct mm_flash_persist;

enum mcxn947_lifecycle_state {
    MCXN947_LCS_CM = 0,
    MCXN947_LCS_DM = 1,
    MCXN947_LCS_SE = 2,
    MCXN947_LCS_RMA = 3
};

struct mcxn947_attestation_blob {
    mm_u8 measurement[32];
    mm_u8 cdi[32];
    mm_u8 pubkey[32];
    mm_u8 signature[32];
};

void mm_mcxn947_secure_reset(void);
mm_bool mm_mcxn947_secure_register_mmio(struct mmio_bus *bus);
void mm_mcxn947_secure_flash_bind(struct mm_memmap *map,
                                  mm_u8 *flash,
                                  mm_u32 flash_size,
                                  const struct mm_flash_persist *persist,
                                  mm_u32 flags);

mm_bool mm_mcxn947_secure_otp_read(mm_u32 index, mm_u32 *value_out);
mm_bool mm_mcxn947_secure_otp_program(mm_u32 index, mm_u32 value);
void mm_mcxn947_secure_rng_fill(mm_u8 *out, mm_u32 len);
void mm_mcxn947_secure_kdf(mm_u8 out[32], const mm_u8 *key, mm_u32 key_len,
                           const mm_u8 *data, mm_u32 len);
mm_bool mm_mcxn947_secure_els_clock_ready(void);
mm_bool mm_mcxn947_secure_rom_call_allowed(enum mm_sec_state sec);
enum mcxn947_lifecycle_state mm_mcxn947_secure_lifecycle(void);
mm_bool mm_mcxn947_secure_attest(struct mcxn947_attestation_blob *blob_out);
mm_bool mm_mcxn947_secure_measurement(mm_u8 out[32]);

#endif /* M33MU_MCXN947_SECURE_H */
