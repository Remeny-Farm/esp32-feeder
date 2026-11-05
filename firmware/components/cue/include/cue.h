/*
 * cue: the sound that tells the hens food is coming.
 *
 * The pattern is identical every time on purpose: the hens learn it. Do not
 * vary it per call and do not change it casually after deployment.
 */
#ifndef CUE_H
#define CUE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Set up the LEDC timer and channel for the buzzer on GPIO20 (silent). */
esp_err_t cue_init(void);

/* Play the 3-note pattern, then silence. Blocking (about 0.5 s). Feeder task only. */
void cue_play(void);

#ifdef __cplusplus
}
#endif

#endif /* CUE_H */
