/* NVS persistence: the gate state blob and the random schedule. */
#ifndef NV_STORE_H
#define NV_STORE_H

#include <stdbool.h>

#include "esp_err.h"
#include "rand_sched.h"
#include "safety_gate.h"

/* Load the saved gate state. False when none exists or its size is wrong.
 * The caller still validates it with gate_restore(). */
bool nv_load_gate(gate_state_t *out);
esp_err_t nv_save_gate(const gate_state_t *st);

/* Load the saved schedule. False when none exists or it is not valid. */
bool nv_load_sched(sched_cfg_t *out);
esp_err_t nv_save_sched(const sched_cfg_t *cfg);

#endif /* NV_STORE_H */
