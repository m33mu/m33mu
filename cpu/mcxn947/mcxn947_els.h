#ifndef M33MU_MCXN947_ELS_H
#define M33MU_MCXN947_ELS_H

#include "m33mu/types.h"

struct mmio_bus;
struct mm_memmap;

#define MCXN947_ELS_BASE_NS   0x40054000u
#define MCXN947_ELS_BASE_S    0x50054000u
#define MCXN947_ELS_KEYSLOTS  20u

void mm_mcxn947_els_reset(void);
mm_bool mm_mcxn947_els_register_mmio(struct mmio_bus *bus);
void mm_mcxn947_els_bind_memmap(struct mm_memmap *map);
mm_bool mm_mcxn947_els_slot_active(mm_u32 slot);

#endif /* M33MU_MCXN947_ELS_H */
