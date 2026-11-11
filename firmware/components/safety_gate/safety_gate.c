/* safety_gate: see include/safety_gate.h for the contract and the check order. */
#include "safety_gate.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

/* a + b without wrapping; counters saturate instead of going back to 0. */
static uint32_t sat_add(uint32_t a, uint32_t b)
{
    return (a > UINT32_MAX - b) ? UINT32_MAX : a + b;
}

/* Quiet window [s, e) on a 1440-minute circle; s > e crosses midnight. */
static bool in_window(uint16_t m, uint16_t s, uint16_t e)
{
    if (s == e) {
        return false; /* empty window */
    }
    if (s < e) {
        return m >= s && m < e;
    }
    return m >= s || m < e;
}

static bool window_ok(uint16_t s, uint16_t e)
{
    return s < GATE_MINUTES_PER_DAY && e < GATE_MINUTES_PER_DAY;
}

/* 1..GATE_ID_MAX_LEN printable, non-space ASCII characters. */
static bool id_ok(const char *id)
{
    size_t n = 0;
    if (id == NULL) {
        return false;
    }
    while (id[n] != '\0') {
        if (n >= GATE_ID_MAX_LEN) {
            return false; /* too long: never truncate, truncation could collide */
        }
        if ((unsigned char)id[n] < 0x21 || (unsigned char)id[n] > 0x7e) {
            return false;
        }
        n++;
    }
    return n > 0;
}

static bool clock_ok(const gate_time_t *tm)
{
    /* A local_minute out of range means the caller could not convert the
     * time: treat it like no clock (fail closed). */
    return tm != NULL && tm->clock_valid && tm->now >= GATE_MIN_VALID_EPOCH &&
           tm->local_minute < GATE_MINUTES_PER_DAY;
}

/* ---- bounded active cache and permanent legacy quarantine ---- */

static bool legacy_contains(const gate_t *g, const char *id)
{
    for (uint16_t i = 0; i < g->st.legacy_count; i++) {
        if (strncmp(g->st.legacy_ids[i], id, GATE_ID_BUF) == 0) {
            return true;
        }
    }
    return false;
}

static int seen_index(const gate_t *g, const char *id)
{
    for (uint16_t i = 0; i < g->st.seen_count; i++) {
        if (strncmp(g->st.seen_ids[i], id, GATE_ID_BUF) == 0) return i;
    }
    return -1;
}

static bool seen_record(gate_t *g, const char *id, int64_t expiry)
{
    if (g->st.seen_count == GATE_SEEN_IDS) {
        gate_set_paused(g, true);
        return false; /* truthful busy: this new id was not consumed */
    }
    const uint16_t i = g->st.seen_count++;
    const size_t n = strlen(id);
    memcpy(g->st.seen_ids[i], id, n);
    memset(g->st.seen_ids[i] + n, 0, GATE_ID_BUF - n);
    g->st.seen_expires_at[i] = expiry;
    g->st.seen_head = g->st.seen_count % GATE_SEEN_IDS;
    g->dirty = true;
    return true;
}

/* Quiet time is elapsed time; expiration belongs to each installed payload.
 * Raw wall samples never advance a global admission watermark. */
static void seen_touch(gate_t *g, const gate_time_t *tm)
{
    if (tm == NULL) {
        g->seen_timer_started = false;
        return;
    }
    const uint64_t horizon = GATE_MAX_FUTURE_S + FEEDER_CMD_MAX_TTL_S;
    if (clock_ok(tm) && g->seen_timer_started && tm->monotonic_s >= g->seen_last_mono &&
        tm->monotonic_s - g->seen_last_mono > horizon) {
        uint16_t keep = 0;
        for (uint16_t i = 0; i < g->st.seen_count; i++) {
            const int64_t expiry = g->st.seen_expires_at[i];
            if (tm->now > expiry) {
                if (expiry > g->st.retired_expiry_floor) g->st.retired_expiry_floor = expiry;
                g->dirty = true;
            } else {
                if (keep != i) {
                    memcpy(g->st.seen_ids[keep], g->st.seen_ids[i], GATE_ID_BUF);
                    g->st.seen_expires_at[keep] = expiry;
                }
                keep++;
            }
        }
        if (keep != g->st.seen_count) {
            memset(g->st.seen_ids + keep, 0, (GATE_SEEN_IDS - keep) * GATE_ID_BUF);
            memset(g->st.seen_expires_at + keep, 0, (GATE_SEEN_IDS - keep) * sizeof(int64_t));
            g->st.seen_count = keep;
            g->st.seen_head = keep % GATE_SEEN_IDS;
        }
    }
    g->seen_last_mono = tm->monotonic_s;
    g->seen_timer_started = true;
}

/* ---- effective limits ---- */

uint32_t gate_eff_max_dose(const gate_t *g, uint8_t h)
{
    if (h >= g->limits.hopper_count) {
        return 0;
    }
    return min_u32(g->limits.max_dose_g[h], g->st.runtime.max_dose_g[h]); /* UNSET = UINT32_MAX */
}

uint32_t gate_eff_daily_budget(const gate_t *g, uint8_t h)
{
    if (h >= g->limits.hopper_count) {
        return 0;
    }
    return min_u32(g->limits.daily_budget_g[h], g->st.runtime.daily_budget_g[h]);
}

uint32_t gate_eff_daily_total(const gate_t *g)
{
    return min_u32(g->limits.daily_total_g, g->st.runtime.daily_total_g);
}

uint32_t gate_eff_min_interval(const gate_t *g)
{
    /* The interval is a minimum, so "lowering" the limit means a LONGER
     * interval: effective = max(compiled, runtime). */
    uint32_t rt = g->st.runtime.min_interval_s;
    if (rt == GATE_UNSET || rt < g->limits.min_interval_s) {
        return g->limits.min_interval_s;
    }
    return rt;
}

bool gate_in_quiet_hours(const gate_t *g, uint16_t m)
{
    /* Union of the compiled window and the runtime window: membership in
     * either one. The runtime window can therefore only widen the quiet time. */
    if (in_window(m, g->limits.quiet_start_min, g->limits.quiet_end_min)) {
        return true;
    }
    return g->st.runtime.has_quiet &&
           in_window(m, g->st.runtime.quiet_start_min, g->st.runtime.quiet_end_min);
}

/* ------------------------------------------------------------------ */
/* init, persistence                                                   */
/* ------------------------------------------------------------------ */

const char *gate_reason_str(gate_result_t r)
{
    switch (r) {
    case GATE_OK: return NULL;
    case GATE_NO_CLOCK: return "no_clock";
    case GATE_EXPIRED: return "expired";
    case GATE_DUPLICATE: return "duplicate";
    case GATE_PAUSED: return "paused";
    case GATE_QUIET_HOURS: return "quiet_hours";
    case GATE_MIN_INTERVAL: return "min_interval";
    case GATE_DOSE_LIMIT: return "dose_limit";
    case GATE_BUDGET_EXCEEDED: return "daily_budget_exceeded";
    case GATE_HOPPER_EMPTY: return "hopper_empty";
    case GATE_FAULT: return "fault";
    case GATE_INVALID: return "invalid";
    case GATE_BUSY: return "busy";
    }
    return "invalid";
}

void gate_runtime_limits_clear(gate_runtime_limits_t *rt)
{
    memset(rt, 0, sizeof *rt);
    for (int i = 0; i < GATE_MAX_HOPPERS; i++) {
        rt->max_dose_g[i] = GATE_UNSET;
        rt->daily_budget_g[i] = GATE_UNSET;
    }
    rt->daily_total_g = GATE_UNSET;
    rt->min_interval_s = GATE_UNSET;
    rt->has_quiet = false;
}

static void state_reset(gate_state_t *st)
{
    memset(st, 0, sizeof *st);
    st->magic = GATE_STATE_MAGIC;
    st->version = GATE_STATE_VERSION;
    gate_runtime_limits_clear(&st->runtime);
}

bool gate_init(gate_t *g, const gate_limits_t *compiled)
{
    memset(g, 0, sizeof *g);
    state_reset(&g->st);
    if (compiled == NULL || compiled->hopper_count < 1 || compiled->hopper_count > GATE_MAX_HOPPERS ||
        !window_ok(compiled->quiet_start_min, compiled->quiet_end_min)) {
        return false; /* g->ready stays false: every check then fails with GATE_FAULT */
    }
    g->limits = *compiled;
    for (int i = 0; i < GATE_MAX_HOPPERS; i++) {
        g->level[i] = GATE_LEVEL_UNKNOWN;
    }
    g->ready = true;
    return true;
}

/* Clamp a runtime value to the compiled maximum. Returns true when it clamped. */
static bool clamp_max(uint32_t *rt, uint32_t compiled)
{
    if (*rt != GATE_UNSET && *rt > compiled) {
        *rt = compiled;
        return true;
    }
    return false;
}

/* Validate and clamp `rt` against the compiled limits. Returns false (and
 * changes nothing in `out`) when `rt` is malformed. */
static bool runtime_normalise(const gate_t *g, const gate_runtime_limits_t *rt,
                              gate_runtime_limits_t *out, int *clamped)
{
    int n = 0;
    gate_runtime_limits_t tmp = *rt;
    for (uint8_t h = 0; h < GATE_MAX_HOPPERS; h++) {
        if (h >= g->limits.hopper_count) {
            if (tmp.max_dose_g[h] != GATE_UNSET || tmp.daily_budget_g[h] != GATE_UNSET) {
                return false; /* override for a hopper that does not exist */
            }
            continue;
        }
        n += clamp_max(&tmp.max_dose_g[h], g->limits.max_dose_g[h]);
        n += clamp_max(&tmp.daily_budget_g[h], g->limits.daily_budget_g[h]);
    }
    n += clamp_max(&tmp.daily_total_g, g->limits.daily_total_g);
    /* min_interval: a runtime value below the compiled one is clamped UP. */
    if (tmp.min_interval_s != GATE_UNSET && tmp.min_interval_s < g->limits.min_interval_s) {
        tmp.min_interval_s = g->limits.min_interval_s;
        n++;
    }
    if (tmp.has_quiet) {
        /* start == end is ambiguous (empty or 24 h?), so it is malformed. */
        if (!window_ok(tmp.quiet_start_min, tmp.quiet_end_min) ||
            tmp.quiet_start_min == tmp.quiet_end_min) {
            return false;
        }
    } else {
        tmp.quiet_start_min = 0;
        tmp.quiet_end_min = 0;
    }
    *out = tmp;
    if (clamped != NULL) {
        *clamped = n;
    }
    return true;
}

gate_result_t gate_set_runtime_limits(gate_t *g, const gate_runtime_limits_t *rt, int *clamped)
{
    gate_runtime_limits_t norm;
    if (clamped != NULL) {
        *clamped = 0;
    }
    if (!g->ready) {
        return GATE_FAULT;
    }
    if (rt == NULL || !runtime_normalise(g, rt, &norm, clamped)) {
        return GATE_INVALID;
    }
    g->st.runtime = norm;
    g->dirty = true;
    return GATE_OK;
}

bool gate_restore(gate_t *g, const gate_state_t *saved)
{
    const gate_state_t *st = saved;
    gate_runtime_limits_t norm;

    if (!g->ready || saved == NULL) {
        return false;
    }
    if (st->magic != GATE_STATE_MAGIC || st->version != GATE_STATE_VERSION) {
        return false;
    }
    if (st->day < 0 || st->last_scatter_at < 0 || st->last_cue_at < 0 ||
        st->seen_head >= GATE_SEEN_IDS || st->seen_count > GATE_SEEN_IDS ||
        st->retired_expiry_floor < 0 || st->legacy_count > GATE_LEGACY_IDS) {
        return false;
    }
    if ((st->fault_mask & ~(GATE_FAULT_STALL | GATE_FAULT_LID_OPEN | GATE_FAULT_MOTOR_CONTROLLER |
                           GATE_FAULT_WATCHDOG)) != 0) {
        return false;
    }
    for (uint8_t h = g->limits.hopper_count; h < GATE_MAX_HOPPERS; h++) {
        if (st->used_g[h] != 0) {
            return false;
        }
    }
    /* Compiled limits may have changed with a new firmware: re-clamp. */
    if (!runtime_normalise(g, &st->runtime, &norm, NULL)) {
        return false;
    }
    g->st = *st;
    g->st.runtime = norm;
    for (uint16_t i = 0; i < GATE_SEEN_IDS; i++) {
        g->st.seen_ids[i][GATE_ID_BUF - 1] = '\0';
    }
    for (uint16_t i = 0; i < GATE_LEGACY_IDS; i++) {
        g->st.legacy_ids[i][GATE_ID_BUF - 1] = '\0';
    }
    if (g->st.replay_untrusted) {
        g->ready = false;
    }
    g->seen_timer_started = false; /* restart elapsed quiet interval after reboot */
    g->reserved = false; /* a reservation does not survive a restore: its grams stay counted */
    memset(g->reserved_g, 0, sizeof g->reserved_g);
    g->dirty = false;
    return true;
}

const gate_state_t *gate_state(const gate_t *g) { return &g->st; }

bool gate_take_dirty(gate_t *g)
{
    bool d = g->dirty;
    g->dirty = false;
    return d;
}

/* ------------------------------------------------------------------ */
/* clock-dependent state                                               */
/* ------------------------------------------------------------------ */

/* Daily counters reset at local midnight. Needs a valid clock.
 * st.day is MONOTONIC: it is the highest local day seen. If the local day goes
 * backwards (clock corrected, time zone change) the counters are kept and
 * st.day stays. Only a day above st.day resets, so stepping the clock back and
 * forward again cannot hand out the day's budget twice (fail closed). */
static void roll_day(gate_t *g, const gate_time_t *tm)
{
    if (tm->local_day > g->st.day) {
        memset(g->st.used_g, 0, sizeof g->st.used_g);
        g->st.used_total_g = 0;
        g->st.day = tm->local_day;
        g->dirty = true;
    }
}

/* A last-action time in the future means the clock moved back. Pretend the
 * action happened now, so the full interval is waited again (fail closed). */
static void clamp_last(gate_t *g, const gate_time_t *tm)
{
    if (g->st.last_scatter_at > tm->now) {
        g->st.last_scatter_at = tm->now;
        g->dirty = true;
    }
    if (g->st.last_cue_at > tm->now) {
        g->st.last_cue_at = tm->now;
        g->dirty = true;
    }
}

/* ------------------------------------------------------------------ */
/* checks                                                              */
/* ------------------------------------------------------------------ */

/* Steps 0-3 of the check order. */
static gate_result_t precheck(gate_t *g, const gate_time_t *tm, gate_source_t src, const gate_cmd_meta_t *meta)
{
    if (src == GATE_SRC_REMOTE && (meta == NULL || !id_ok(meta->id))) {
        return GATE_INVALID;
    }
    if (src == GATE_SRC_REMOTE) seen_touch(g, tm);
    bool seen = false;
    if (src == GATE_SRC_REMOTE) {
        const int index = seen_index(g, meta->id);
        const bool legacy = legacy_contains(g, meta->id);
        seen = index >= 0 || legacy;
        const bool lifetime = meta->expires_at >= meta->issued_at &&
            (uint64_t)meta->expires_at - (uint64_t)meta->issued_at <= (uint64_t)FEEDER_CMD_MAX_TTL_S;
        const int64_t expiry = lifetime ? meta->expires_at : (clock_ok(tm) ? tm->now : 0);
        if (index >= 0 && lifetime && expiry > g->st.seen_expires_at[index]) {
            g->st.seen_expires_at[index] = expiry; /* retries never shorten a protected id */
            g->dirty = true;
        } else if (!seen && !seen_record(g, meta->id, expiry)) {
            return GATE_BUSY;
        }
    }
    if (!clock_ok(tm)) return GATE_NO_CLOCK;
    if (src == GATE_SRC_RANDOM) return GATE_OK;
    if (tm->now > meta->expires_at || meta->issued_at > tm->now + GATE_MAX_FUTURE_S ||
        meta->expires_at < meta->issued_at ||
        (uint64_t)meta->expires_at - (uint64_t)meta->issued_at > (uint64_t)FEEDER_CMD_MAX_TTL_S ||
        meta->expires_at <= g->st.retired_expiry_floor) {
        return GATE_EXPIRED;
    }
    return seen ? GATE_DUPLICATE : GATE_OK;
}

/* Dose list structure: step 4. */
static gate_result_t check_doses_valid(const gate_t *g, const gate_dose_t *doses, size_t n)
{
    unsigned seen_mask = 0;
    if (doses == NULL || n < 1 || n > g->limits.hopper_count) {
        return GATE_INVALID;
    }
    for (size_t i = 0; i < n; i++) {
        if (doses[i].hopper >= g->limits.hopper_count || doses[i].grams < 1 ||
            (seen_mask & (1u << doses[i].hopper)) != 0) {
            return GATE_INVALID;
        }
        seen_mask |= 1u << doses[i].hopper;
    }
    return GATE_OK;
}

/* Steps 5-8, shared by scatter and cue. */
static gate_result_t check_state(gate_t *g, const gate_time_t *tm, bool cue_only)
{
    roll_day(g, tm);
    clamp_last(g, tm);
    if (g->st.paused) {
        return GATE_PAUSED;
    }
    if (g->st.fault_mask != 0) {
        return GATE_FAULT;
    }
    if (gate_in_quiet_hours(g, tm->local_minute)) {
        return GATE_QUIET_HOURS;
    }
    int64_t interval = (int64_t)gate_eff_min_interval(g);
    if (g->st.last_scatter_at != 0 && tm->now - g->st.last_scatter_at < interval) {
        return GATE_MIN_INTERVAL;
    }
    if (cue_only && g->st.last_cue_at != 0 && tm->now - g->st.last_cue_at < interval) {
        return GATE_MIN_INTERVAL;
    }
    return GATE_OK;
}

gate_result_t gate_check_command(gate_t *g, const gate_time_t *tm, const gate_cmd_meta_t *meta)
{
    if (!g->ready) {
        return GATE_FAULT;
    }
    return precheck(g, tm, GATE_SRC_REMOTE, meta);
}

bool gate_note_id(gate_t *g, const char *id)
{
    if (!g->ready || !id_ok(id)) {
        return false;
    }
    seen_touch(g, NULL);
    if (legacy_contains(g, id)) return true;
    const int index = seen_index(g, id);
    if (index >= 0) {
        g->st.seen_expires_at[index] = INT64_MAX; /* no expiry was supplied */
        g->dirty = true;
        return true;
    }
    return seen_record(g, id, INT64_MAX);
}

gate_result_t gate_check_cue(gate_t *g, const gate_time_t *tm, gate_source_t src, const gate_cmd_meta_t *meta)
{
    gate_result_t r;
    if (!g->ready) {
        return GATE_FAULT;
    }
    r = precheck(g, tm, src, meta);
    if (r != GATE_OK) {
        return r;
    }
    return check_state(g, tm, true);
}

gate_result_t gate_check_scatter(gate_t *g, const gate_time_t *tm, gate_source_t src,
                                 const gate_cmd_meta_t *meta, const gate_dose_t *doses, size_t n)
{
    gate_result_t r;
    int64_t total = 0;

    if (!g->ready) {
        return GATE_FAULT;
    }
    r = precheck(g, tm, src, meta);
    if (r != GATE_OK) {
        return r;
    }
    r = check_doses_valid(g, doses, n);
    if (r != GATE_OK) {
        return r;
    }
    r = check_state(g, tm, false);
    if (r != GATE_OK) {
        return r;
    }
    for (size_t i = 0; i < n; i++) {
        if (doses[i].grams > (int64_t)gate_eff_max_dose(g, doses[i].hopper)) {
            return GATE_DOSE_LIMIT;
        }
    }
    for (size_t i = 0; i < n; i++) {
        uint8_t h = doses[i].hopper;
        /* each dose <= max dose here, so the sums cannot overflow int64 */
        if ((int64_t)g->st.used_g[h] + doses[i].grams > (int64_t)gate_eff_daily_budget(g, h)) {
            return GATE_BUDGET_EXCEEDED;
        }
        total += doses[i].grams;
    }
    if ((int64_t)g->st.used_total_g + total > (int64_t)gate_eff_daily_total(g)) {
        return GATE_BUDGET_EXCEEDED;
    }
    for (size_t i = 0; i < n; i++) {
        if (g->level[doses[i].hopper] == GATE_LEVEL_EMPTY) {
            return GATE_HOPPER_EMPTY;
        }
    }
    return GATE_OK;
}

/* ------------------------------------------------------------------ */
/* commits                                                             */
/* ------------------------------------------------------------------ */

void gate_commit_scatter(gate_t *g, const gate_time_t *tm, const uint32_t grams[GATE_MAX_HOPPERS])
{
    if (!g->ready || tm == NULL || grams == NULL) {
        return;
    }
    if (clock_ok(tm)) {
        roll_day(g, tm);
    }
    for (uint8_t h = 0; h < g->limits.hopper_count; h++) {
        g->st.used_g[h] = sat_add(g->st.used_g[h], grams[h]);
        g->st.used_total_g = sat_add(g->st.used_total_g, grams[h]);
    }
    if (clock_ok(tm)) {
        g->st.last_scatter_at = tm->now;
    }
    g->dirty = true;
}

/* Grams to take back when `actual` came out of `reserved`: never negative,
 * never more than was reserved. */
static uint32_t refund_of(uint32_t reserved, uint32_t actual)
{
    return actual < reserved ? reserved - actual : 0;
}

bool gate_reserve_scatter(gate_t *g, const gate_time_t *tm, const gate_dose_t *doses, size_t n)
{
    if (!g->ready || !clock_ok(tm) || check_doses_valid(g, doses, n) != GATE_OK) {
        return false;
    }
    roll_day(g, tm);
    for (size_t i = 0; i < n; i++) {
        uint8_t h = doses[i].hopper;
        uint32_t grams = (doses[i].grams > (int64_t)UINT32_MAX) ? UINT32_MAX : (uint32_t)doses[i].grams;
        g->st.used_g[h] = sat_add(g->st.used_g[h], grams);
        g->st.used_total_g = sat_add(g->st.used_total_g, grams);
        g->reserved_g[h] = sat_add(g->reserved_g[h], grams);
    }
    g->reserved_day = g->st.day;
    g->reserved = true;
    g->st.last_scatter_at = tm->now;
    g->dirty = true;
    return true;
}

void gate_settle_scatter(gate_t *g, const gate_time_t *tm, const uint32_t actual[GATE_MAX_HOPPERS])
{
    (void)tm;
    if (!g->ready || actual == NULL || !g->reserved) {
        return; /* nothing reserved: nothing to give back (fail closed) */
    }
    /* If the local day rolled while the motors ran, the reserved grams belong to
     * the old day: the counters were already reset, so there is nothing to refund. */
    if (g->reserved_day == g->st.day) {
        for (uint8_t h = 0; h < g->limits.hopper_count; h++) {
            uint32_t refund = refund_of(g->reserved_g[h], actual[h]);
            refund = min_u32(refund, g->st.used_g[h]);
            g->st.used_g[h] -= refund;
            g->st.used_total_g -= min_u32(refund, g->st.used_total_g);
        }
    }
    memset(g->reserved_g, 0, sizeof g->reserved_g);
    g->reserved = false;
    g->dirty = true;
}

void gate_commit_cue(gate_t *g, const gate_time_t *tm)
{
    if (!g->ready || !clock_ok(tm)) {
        return;
    }
    g->st.last_cue_at = tm->now;
    g->dirty = true;
}

/* ------------------------------------------------------------------ */
/* pause, levels, faults, read-outs                                    */
/* ------------------------------------------------------------------ */

void gate_set_paused(gate_t *g, bool paused)
{
    if (g->st.paused != paused) {
        g->st.paused = paused;
        g->dirty = true;
    }
}

bool gate_paused(const gate_t *g) { return g->st.paused; }

void gate_set_level(gate_t *g, uint8_t hopper, gate_level_t level)
{
    if (hopper < GATE_MAX_HOPPERS) {
        g->level[hopper] = level;
    }
}

gate_level_t gate_level(const gate_t *g, uint8_t hopper)
{
    return hopper < GATE_MAX_HOPPERS ? g->level[hopper] : GATE_LEVEL_UNKNOWN;
}

void gate_fault_raise(gate_t *g, gate_fault_t f)
{
    if ((g->st.fault_mask & (uint32_t)f) != (uint32_t)f) {
        g->st.fault_mask |= (uint32_t)f;
        g->dirty = true;
    }
}

void gate_fault_clear(gate_t *g, gate_fault_t f)
{
    if ((g->st.fault_mask & (uint32_t)f) != 0) {
        g->st.fault_mask &= ~(uint32_t)f;
        g->dirty = true;
    }
}

void gate_fault_clear_all(gate_t *g)
{
    if (g->st.fault_mask != 0) {
        g->st.fault_mask = 0;
        g->dirty = true;
    }
}

uint32_t gate_faults(const gate_t *g) { return g->st.fault_mask; }

uint32_t gate_used_today(gate_t *g, const gate_time_t *tm, uint8_t hopper)
{
    if (hopper >= GATE_MAX_HOPPERS) {
        return 0;
    }
    if (clock_ok(tm)) {
        roll_day(g, tm);
    }
    return g->st.used_g[hopper];
}

uint32_t gate_used_total_today(gate_t *g, const gate_time_t *tm)
{
    if (clock_ok(tm)) {
        roll_day(g, tm);
    }
    return g->st.used_total_g;
}
