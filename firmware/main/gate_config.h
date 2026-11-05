/* Compiled-in configuration (Kconfig): gate limits, hopper names, device id. */
#ifndef GATE_CONFIG_H
#define GATE_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "safety_gate.h"

/* Fill the compiled-in welfare limits for gate_init(). All values are
 * PLACEHOLDERS from Kconfig: set after welfare review. */
void gate_config_limits(gate_limits_t *out);

uint8_t feeder_hopper_count(void);
/* Name of hopper i, or "" when i is out of range. */
const char *feeder_hopper_name(uint8_t i);
/* Index of the hopper with this name, or -1. Only active hoppers match. */
int feeder_hopper_index(const char *name);
/* Compiled-in dose of a random scatter for hopper i (grams). */
uint32_t feeder_random_dose_g(uint8_t i);

/* Runtime check of ^[a-z0-9][a-z0-9-]{2,31}$. */
bool feeder_device_id_valid(const char *id);
/* Device id valid, hopper count 1..3, hopper names non-empty and unique. */
bool feeder_config_valid(void);

#endif /* GATE_CONFIG_H */
