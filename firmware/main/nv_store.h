/* NVS persistence: the gate state blob and the random schedule. */
#ifndef NV_STORE_H
#define NV_STORE_H

#include <stdbool.h>

#include "esp_err.h"
#include "rand_sched.h"
#include "safety_gate.h"

/* Load v2 state or migrate the v1 prefix without losing pause, counters or ids.
 * Legacy ids are quarantined separately. Inconsistent snapshot pairs disable
 * admission; false means no saved state exists.
 * The caller still validates it with gate_restore(). */
bool nv_load_gate(gate_state_t *out);
esp_err_t nv_save_gate(const gate_state_t *st);

/* Load the saved schedule. False when none exists or it is not valid. */
bool nv_load_sched(sched_cfg_t *out);
esp_err_t nv_save_sched(const sched_cfg_t *cfg);

#endif /* NV_STORE_H */
