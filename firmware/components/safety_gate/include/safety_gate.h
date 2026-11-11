/*
 * safety_gate: welfare limits for the treat feeder.
 *
 * Pure C99. No ESP-IDF, no libc time(), no malloc, no I/O. It compiles on the
 * host, where the unit tests in firmware/test/host/ exercise it.
 *
 * Every scatter and cue, from the random scheduler or from a remote command,
 * must pass gate_check_scatter() / gate_check_cue() before any motor or
 * buzzer moves. The gate never raises a limit above the compiled-in maximum.
 *
 * THREADING: a gate_t has no lock. One task (the feeder task) must own it and
 * serialise check -> run -> commit. This also closes the "two scatters pass
 * the budget before either commits" race.
 *
 * WELFARE REVIEW: all compiled-in numbers come from Kconfig and are
 * PLACEHOLDERS: set after welfare review.
 *
 * ---------------------------------------------------------------------
 * Check order. The first failing step wins. R = remote path only.
 *
 *   0. invalid        R: missing or malformed command id (1..36 printable chars)
 *   1. no_clock       R and random: no valid clock (never synced since boot,
 *                     or the time is before GATE_MIN_VALID_EPOCH)
 *   2. expired    R   now > expires_at, or issued_at > now + 5 min, or
 *                     expires_at < issued_at, or the lifetime
 *                     expires_at - issued_at > FEEDER_CMD_MAX_TTL_S. The TTL
 *                     cap bounds replay retention together with clock skew.
 *   3. duplicate  R   id is in the active cache or permanent legacy quarantine.
 *                     Installed ids retain their own MAX expiry. After 600 s
 *                     monotonic silence, only expired slots can be retired.
 *                     A persisted payload-expiry cutoff prevents old retired
 *                     payloads becoming eligible after a clock correction.
 *      busy       R   new id cannot fit: pause, reject, do NOT consume that id.
 *                     A later retry may be evaluated after capacity recovers.
 *   4. invalid        scatter only: dose list malformed (1..hopper_count
 *                     doses, unique hopper index < hopper_count, grams >= 1)
 *   5. paused         BOTH paths: a paused feeder neither cues nor scatters,
 *                     random included. (pause/resume/set_schedule skip it.)
 *   6. fault          any latched fault
 *   7. quiet_hours    compiled window UNION runtime window, windows may cross
 *                     midnight
 *   8. min_interval   since the last scatter (either source). A cue-only
 *                     command is also limited against the last cue-only
 *                     command, so a flood of `cue` commands cannot make noise.
 *   9. dose_limit     scatter: any dose > effective max dose of its hopper
 *  10. daily_budget_exceeded  scatter: used-today + requested > effective
 *                     budget, per hopper and in total
 *  11. hopper_empty   scatter: a requested hopper reports level `empty`
 *
 * pause / resume / set_schedule only run steps 0-3 (gate_check_command()).
 *
 * Runtime limits (set_schedule) can only make the feeder stricter:
 *   max dose, daily budget, daily total: effective = min(compiled, runtime)
 *   min_interval_s: effective = MAX(compiled, runtime). The contract says
 *       min() for every value, but for an interval "lower" means longer.
 *       min() would let a command SHORTEN the interval below the compiled
 *       one. The gate therefore clamps a shorter runtime interval up.
 *   quiet hours: effective quiet set = union of the compiled and the runtime
 *       window (membership in either). A runtime window can only add quiet
 *       time. start == end is rejected as invalid for a runtime window (it
 *       could mean "empty" or "24 h"); a compiled start == end means "no
 *       compiled quiet hours" (not recommended).
 *
 * Time: the gate never reads a clock. If the local day number goes backwards
 * the counters are kept and st.day stays at the highest day seen, so stepping
 * the clock back and forward again never resets the counters twice (fail
 * closed). A last-scatter time in the future is clamped to now. A jump to a
 * day above st.day resets the counters, so time is a trust root: the SNTP
 * source must be trusted.
 * ---------------------------------------------------------------------
 */
#ifndef SAFETY_GATE_H
#define SAFETY_GATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GATE_MAX_HOPPERS 3
/* Number of remembered command ids. The contract asks for >= 32. */
#define GATE_SEEN_IDS 64
#define GATE_LEGACY_IDS GATE_SEEN_IDS
/* UUID text is 36 characters; the buffer includes the terminating NUL. */
#define GATE_ID_MAX_LEN 36
#define GATE_ID_BUF (GATE_ID_MAX_LEN + 1)
/* issued_at may be at most this far in the future (clock skew allowance). */
#define GATE_MAX_FUTURE_S (5 * 60)
/* Longest accepted lifetime of a remote command (expires_at - issued_at).
 * The replay cache retains ids for this lifetime plus clock skew after the
 * last remote attempt, and each installed id until its own expiry. The app
 * may override the value with a compiler definition. */
#ifndef FEEDER_CMD_MAX_TTL_S
#define FEEDER_CMD_MAX_TTL_S 300
#endif
/* A clock earlier than this is not a valid clock (2026-11-01T00:00:00Z). */
#define GATE_MIN_VALID_EPOCH INT64_C(1793491200)
/* "not set" marker for runtime limits. */
#define GATE_UNSET UINT32_MAX
#define GATE_MINUTES_PER_DAY 1440

typedef enum {
    GATE_OK = 0,
    GATE_NO_CLOCK,
    GATE_EXPIRED,
    GATE_DUPLICATE,
    GATE_PAUSED,
    GATE_QUIET_HOURS,
    GATE_MIN_INTERVAL,
    GATE_DOSE_LIMIT,
    GATE_BUDGET_EXCEEDED,
    GATE_HOPPER_EMPTY,
    GATE_FAULT,
    GATE_INVALID,
    GATE_BUSY /* new id not consumed: active replay capacity exhausted */
} gate_result_t;

/* Protocol reason string of a result ("no_clock", ...). NULL for GATE_OK. */
const char *gate_reason_str(gate_result_t r);

typedef enum { GATE_SRC_RANDOM = 0, GATE_SRC_REMOTE = 1 } gate_source_t;

typedef enum {
    GATE_LEVEL_UNKNOWN = 0, /* no sensor or sensor failed: scatter allowed */
    GATE_LEVEL_OK,
    GATE_LEVEL_LOW,
    GATE_LEVEL_EMPTY        /* scatter from this hopper rejected */
} gate_level_t;

/* Latched fault bits. hopper_empty is a level, not a latched fault. */
typedef enum {
    GATE_FAULT_STALL = 1u << 0,
    GATE_FAULT_LID_OPEN = 1u << 1,
    GATE_FAULT_MOTOR_CONTROLLER = 1u << 2,
    GATE_FAULT_WATCHDOG = 1u << 3
} gate_fault_t;

/* ---- compiled-in limits (from Kconfig, passed in by the app) ---- */
typedef struct {
    uint8_t hopper_count;                        /* 1..GATE_MAX_HOPPERS */
    uint32_t max_dose_g[GATE_MAX_HOPPERS];       /* per scatter, per hopper */
    uint32_t daily_budget_g[GATE_MAX_HOPPERS];   /* per local day, per hopper */
    uint32_t daily_total_g;                      /* per local day, all hoppers */
    uint32_t min_interval_s;
    /* Quiet window [start, end) in local minutes of day, 0..1439. It may
     * cross midnight (start > end). start == end means "no quiet hours". */
    uint16_t quiet_start_min;
    uint16_t quiet_end_min;
} gate_limits_t;

/* ---- runtime limits (from set_schedule): can only LOWER ---- */
typedef struct {
    uint32_t max_dose_g[GATE_MAX_HOPPERS];       /* GATE_UNSET = no override */
    uint32_t daily_budget_g[GATE_MAX_HOPPERS];
    uint32_t daily_total_g;
    uint32_t min_interval_s;
    bool has_quiet;
    uint16_t quiet_start_min;                    /* start != end, both < 1440 */
    uint16_t quiet_end_min;
} gate_runtime_limits_t;

/* All fields unset. */
void gate_runtime_limits_clear(gate_runtime_limits_t *rt);

/* ---- time, passed in by the caller (the gate never reads a clock) ---- */
typedef struct {
    int64_t now;            /* UTC epoch seconds */
    uint64_t monotonic_s;   /* elapsed seconds since boot; never wall-clock time */
    bool clock_valid;       /* trusted clock established since boot */
    uint16_t local_minute;  /* local minutes since midnight, 0..1439 */
    int64_t local_day;      /* local calendar day number; +1 per local midnight */
} gate_time_t;

/* ---- remote command metadata ---- */
typedef struct {
    const char *id;         /* NUL-terminated, 1..GATE_ID_MAX_LEN chars */
    int64_t issued_at;      /* UTC epoch seconds */
    int64_t expires_at;     /* UTC epoch seconds */
} gate_cmd_meta_t;

typedef struct {
    uint8_t hopper;         /* index 0..hopper_count-1 */
    int64_t grams;          /* requested grams; must be >= 1 */
} gate_dose_t;

/* ---- persisted state (save it to NVS when gate_take_dirty() is true) ---- */
#define GATE_STATE_MAGIC 0x47415445u /* "GATE" */
#define GATE_STATE_VERSION 3u
#define GATE_PER_ID_EXPIRY 1

typedef struct {
    uint32_t magic;
    uint32_t version;
    int64_t day;                                 /* highest local day seen (monotonic) */
    /* Grams today. A scatter is counted at its REQUESTED size from
     * gate_reserve_scatter() on, and lowered to the actual grams by
     * gate_settle_scatter(). A reset in between keeps the requested grams. */
    uint32_t used_g[GATE_MAX_HOPPERS];
    uint32_t used_total_g;
    int64_t last_scatter_at;                     /* 0 = never */
    int64_t last_cue_at;                         /* cue-only commands, 0 = never */
    uint32_t fault_mask;                         /* gate_fault_t bits */
    bool paused;
    gate_runtime_limits_t runtime;
    char seen_ids[GATE_SEEN_IDS][GATE_ID_BUF];
    uint16_t seen_head;                          /* next free slot, wraps to zero when full */
    uint16_t seen_count;
    int64_t seen_expires_at[GATE_SEEN_IDS];      /* MAX expiry of each installed id */
    int64_t retired_expiry_floor;                /* expiry cutoff of discarded remote payloads */
    char legacy_ids[GATE_LEGACY_IDS][GATE_ID_BUF]; /* unknown v1 expiry: never recycled */
    uint16_t legacy_count;
    bool replay_untrusted;                       /* mismatched persistent snapshots: disable admission */
} gate_state_t;

/* v1 is the unchanged prefix; legacy expiry metadata is unknowable. */
#define GATE_STATE_V1_SIZE offsetof(gate_state_t, seen_expires_at)

typedef struct {
    gate_limits_t limits;                        /* compiled-in */
    gate_state_t st;
    gate_level_t level[GATE_MAX_HOPPERS];        /* not persisted */
    bool dirty;
    bool ready;                                  /* gate_init() succeeded */
    uint64_t seen_last_mono;                      /* last remote attempt, elapsed seconds */
    bool seen_timer_started;                     /* restart conservative quiet interval after restore */
    /* In-flight reservation (gate_reserve_scatter), NOT persisted: after a
     * reset the reserved grams simply stay counted in st.used_g. */
    bool reserved;
    int64_t reserved_day;
    uint32_t reserved_g[GATE_MAX_HOPPERS];
} gate_t;

/* Validate and install the compiled-in limits; state starts empty (running,
 * no faults, counters zero, all levels unknown). Returns false (gate unusable
 * and every check then returns GATE_FAULT) when the limits are inconsistent. */
bool gate_init(gate_t *g, const gate_limits_t *compiled);

/* Restore persisted state. Returns false and leaves the gate unchanged when
 * the blob fails validation (magic, version, ranges). */
bool gate_restore(gate_t *g, const gate_state_t *saved);
const gate_state_t *gate_state(const gate_t *g);
/* True once after any change to persisted state. */
bool gate_take_dirty(gate_t *g);

/* ---- remote control: steps 1-3 only ---- */
gate_result_t gate_check_command(gate_t *g, const gate_time_t *tm, const gate_cmd_meta_t *meta);
/* Record an id of a command that failed to parse after its id was read, so a
 * replay is `duplicate`. False if id unusable or cache full (then pauses). */
bool gate_note_id(gate_t *g, const char *id);

void gate_set_paused(gate_t *g, bool paused);
bool gate_paused(const gate_t *g);

/* Apply runtime limits from set_schedule. Each call REPLACES the previous
 * runtime limits (an omitted `limits` object = gate_runtime_limits_clear()).
 * Values above the compiled maximum are clamped silently; *clamped (optional)
 * receives how many fields were clamped. Returns GATE_INVALID, changing
 * nothing, when the quiet window is malformed. */
gate_result_t gate_set_runtime_limits(gate_t *g, const gate_runtime_limits_t *rt, int *clamped);

/* ---- scatter / cue admission ----
 * `meta` is required for GATE_SRC_REMOTE and ignored for GATE_SRC_RANDOM. */
gate_result_t gate_check_scatter(gate_t *g, const gate_time_t *tm, gate_source_t src,
                                 const gate_cmd_meta_t *meta, const gate_dose_t *doses, size_t n_doses);
gate_result_t gate_check_cue(gate_t *g, const gate_time_t *tm, gate_source_t src,
                             const gate_cmd_meta_t *meta);

/* Fail-closed scatter accounting, in two steps:
 *   1. After gate_check_scatter() returned GATE_OK and BEFORE any motor runs,
 *      call gate_reserve_scatter(). It adds the REQUESTED grams to used-today
 *      (per hopper and total) and sets last_scatter_at = now. Save the gate
 *      state (gate_take_dirty) before the motors start. A brownout, panic or
 *      watchdog reset during dispensing then still counts the requested grams
 *      and applies min_interval.
 *   2. After the motors stopped, call gate_settle_scatter() with the ACTUAL
 *      grams. It only lowers the counters toward the actual amount: never
 *      below 0, never above what was reserved (an actual above the request
 *      changes nothing). last_scatter_at stays: the motors ran, even with 0 g.
 * Returns false (nothing changed, do NOT run the motors) when the gate is not
 * ready, the clock is invalid or the dose list is malformed. Reservations add
 * up if a second one comes before the settle. */
bool gate_reserve_scatter(gate_t *g, const gate_time_t *tm, const gate_dose_t *doses, size_t n_doses);
/* grams[i] is the actual amount of hopper i (0 if nothing came out). A settle
 * without a pending reservation does nothing. If the local day rolled since the
 * reservation, the old day's grams are not refunded from the new day. */
void gate_settle_scatter(gate_t *g, const gate_time_t *tm, const uint32_t grams[GATE_MAX_HOPPERS]);

/* Simple path without a reservation: record what a scatter dispensed. grams[i]
 * is the actual amount of hopper i. Counts as a scatter for min_interval even
 * when everything is 0. NOT crash safe (see above): the app uses
 * reserve + settle. Do not call it between a reserve and its settle. */
void gate_commit_scatter(gate_t *g, const gate_time_t *tm, const uint32_t grams[GATE_MAX_HOPPERS]);
/* Record a cue-only command that actually played. */
void gate_commit_cue(gate_t *g, const gate_time_t *tm);

/* ---- hopper levels, faults ---- */
void gate_set_level(gate_t *g, uint8_t hopper, gate_level_t level);
gate_level_t gate_level(const gate_t *g, uint8_t hopper);
void gate_fault_raise(gate_t *g, gate_fault_t f);
/* Clear one bit (e.g. lid closed again). Latched faults stay until a service
 * action calls gate_fault_clear_all(). */
void gate_fault_clear(gate_t *g, gate_fault_t f);
void gate_fault_clear_all(gate_t *g);
uint32_t gate_faults(const gate_t *g);

/* ---- read-outs for state publishing and the scheduler ---- */
/* True when `local_minute` lies in the compiled or the runtime quiet window. */
bool gate_in_quiet_hours(const gate_t *g, uint16_t local_minute);
uint32_t gate_eff_max_dose(const gate_t *g, uint8_t hopper);
uint32_t gate_eff_daily_budget(const gate_t *g, uint8_t hopper);
uint32_t gate_eff_daily_total(const gate_t *g);
uint32_t gate_eff_min_interval(const gate_t *g);
/* Grams used today. Rolls the day first, so it is correct after midnight. */
uint32_t gate_used_today(gate_t *g, const gate_time_t *tm, uint8_t hopper);
uint32_t gate_used_total_today(gate_t *g, const gate_time_t *tm);

#ifdef __cplusplus
}
#endif

#endif /* SAFETY_GATE_H */
