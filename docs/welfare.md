# Welfare limits and sound cue (proposal)

Status: **proposal**, 2026-11-05. Nobody has approved these numbers yet.
The owner and a welfare reviewer must approve them before the firmware uses
them. The concept is in [idea.md](idea.md). The safety gate is in
[architecture.md](architecture.md).

Scope: a backyard or small-farm flock of **10 to 30 adult laying hens** with
normal layer feed and water always available. Pullets, chicks, broilers and
other species are out of scope.

Source tags like [S1] point to the list at the end. Each number says if it
comes from a source, from a calculation, or from design judgement.
Where sources disagree or none exists, the text says so and the value is
conservative.

## 1. Treat budget

### Basis

- Treats and scratch should stay a small share of daily intake. Extension
  guidance says about 10% for scratch grain [S1]. One extension guide says
  10 to 15% [S2]. One extension guide says to feed no crack corn or scratch at
  all [S3]. An industry source says 10% for all treats [S4].
  Extension sources do not agree, so the proposal uses the low end.
- Grain is high in energy and low in protein, minerals and vitamins. Too much
  grain next to complete feed can cause deficiencies [S5].
- Layer feed intake: 109 to 117 g per hen per day for a commercial brown layer
  hybrid [S6]. Extension tables give 0.18 to 0.27 lb per hen per day, which is
  82 to 122 g [S5]. Heritage and small hens often eat less.

### Formula

```
F      = reference feed intake        = 100 g per hen per day   (low end, conservative)
T_max  = 0.10 x F                     = 10 g per hen per day    (all treats, all sources)
s      = feeder share of T_max        = 0.5                      (design judgement)
B      = s x T_max                    = 5 g per hen per day     (feeder budget)
Budget for a flock of N hens          = B x N
```

Why `s = 0.5`: the 10% limit covers every treat. Keepers also give kitchen
scraps and greens. The feeder takes half and leaves half for the keeper.
The operator can lower the share. Nothing remote can raise it.

### Budget per hopper, per hen per day

| Hopper | Per hen | Basis |
|---|---|---|
| Grain / scratch | 4 g | Share of B. Grain dilutes protein and calcium [S5]. |
| Pellets | 4 g | Share of B. See the note below. |
| Dried mealworms | 1 g | Much lower. See the note below. |
| **Total, all hoppers** | **5 g** | `B`. The total is lower than the sum (9 g). The total always wins. |

Pellets: if the hopper holds the flock's own complete layer feed, scattering it
does not dilute the diet. It only moves feed from the trough to the ground.
Hens showed the most interest in scattered pelleted feed in one free-range
study [S7]. If the hopper holds treat pellets, the 4 g limit protects the diet.
The firmware cannot tell the two apart, so it uses the same limit.

Mealworms: a common retail dried mealworm lists 53% protein and 28% fat [S8].
Complete layer feed has about 16 to 18% crude protein [S6]. So 1 g of
mealworms carries about three times the protein of 1 g of layer feed.
We cap mealworms at 1 g per hen per day, which is 1% of intake.
That adds about 0.5 g protein per hen. A peak layer eats about 17 to 18 g of
protein per day [S6]. No extension or peer-reviewed source gives a daily
mealworm limit. Retail and hobby advice disagrees: 4 dried worms per day [S9],
10 to 15 per day [S10], 10 to 12 every second day [S8]. Retailers also say
mealworms should stay under 10% of the diet [S8, S10]. The 1 g value is our
own conservative calculation. A poultry nutritionist should check it.
We do not convert grams to worm counts, because no source gives the weight of
one dried worm. Weigh a sample at commissioning.

### Doses and counts

```
n      = scatters per day            = 4   (start value)
d_tot  = B / n                       = 1.25 g per hen per scatter (all hoppers together)
```

| Item | Per hen | Basis |
|---|---|---|
| Max dose per scatter, grain | 1.0 g | `d_tot` rounded down. 4 scatters x 1.0 g = 4 g. |
| Max dose per scatter, pellets | 1.0 g | Same. |
| Max dose per scatter, mealworms | 0.5 g | Two full doses use the whole 1 g budget. |
| Max total per scatter (mix) | 1.25 g | `d_tot`. |

Check that the flock clears the dose. Extension guidance says to feed no more
scratch than the birds finish in 1 to 3 hours [S5]. For range-fed pullets it
says 20 to 30 minutes [S5]. The proposal uses a check of **30 minutes**: if
grain is still on the ground 30 minutes after a scatter, lower the dose.
The camera can show this (see section 3).

### Minimum interval and count

- `min_interval_s` = 3600 (60 minutes). Hens need time to finish a scatter and
  go back to normal behaviour. No source gives a value. This is design
  judgement. It also keeps the random times from clustering.
- `scatters_per_day` = 4 at start, hard maximum 6. More than 4 does not raise
  the budget. The budget stops the extra scatters.
- Remote `scatter` commands pass the same gate. They count toward the same
  interval, count and budget.

### Daily window and quiet hours

Facts:

- Hens go to roost around dusk. They like to fill the crop before dark [S11].
- With free access, hens eat more complete feed in the afternoon than in the
  morning. In one trial intake peaked between 16:30 and 18:00 [S11]. The
  stated reason is that this is when a hen forms her next egg, and she needs
  calcium overnight [S11]. One breeder source also links more feed in the crop
  at lights-off to thicker shells [S12]. That source is about breeder hens.
- Some keepers scatter scratch in late afternoon so hens come back to the coop.
  One extension guide suggests a limited late-afternoon scratch for floor-reared
  birds [S5]. This is a keeper convention, not a welfare finding.

**Is "no scatter in the last 1 to 2 hours before dusk" a real recommendation?**
No. We found no source that says it. Some sources say the opposite (late
scratch is common). The reasoning for it is weaker but real: treats
eaten in the late-afternoon peak may replace complete feed when the hen needs
its calcium. The firmware cannot check how much layer feed the hens ate.
So the proposal uses a **2-hour buffer before sunset as a conservative design
choice, marked as judgement**. The welfare reviewer may shorten or drop it.

Window rule (local time, so the firmware needs the time zone and DST rule):

```
window_start = max(08:00, sunrise + 60 min)
window_end   = min(18:00, sunset  - 120 min)
quiet hours  = everything outside the window
hard limits  = never before 08:00 and never after 18:00 local time
```

Why these edges:

- Start 1 hour after sunrise and not before 08:00: hens are out of the coop and
  feeding. Neighbours are not woken (section 2).
- End at 18:00: later scatters add little in summer and fall in the evening
  noise period.
- Quiet hours also include the night, when hens roost.

Day length at Budapest (47.5 N). Times are local clock time. They come from a
standard solar formula (our own calculation) and are rounded to the minute.
Check them against any almanac before approval.

| Date | Sunrise | Sunset | Day length | Window (proposal) | Window length |
|---|---|---|---|---|---|
| 21 Dec | 07:28 | 15:54 | 8.4 h | 08:28 to 13:54 | 5.4 h |
| 20 Mar | 05:49 | 17:54 | 12.1 h | 08:00 to 15:54 | 7.9 h |
| 21 Jun | 04:45 | 20:44 | 16.0 h | 08:00 to 18:00 | 10.0 h |
| 22 Sep | 06:28 | 18:44 | 12.3 h | 08:00 to 16:44 | 8.7 h |

The shortest window (5.4 h) still fits 4 scatters with 60 minutes between them.

If the coop uses artificial light, hens may roost later. Commercial layer
programs run up to 16 hours of light [S6]. Then tie the window to the light
schedule and not to the sun. This is an open point.

The firmware needs a way to know sunrise and sunset: compute them on the device
from latitude, longitude and date, or load a table of 12 monthly values.
Both are small. This is an implementation choice for the firmware.

## 2. Sound cue

### What hens hear

- A measured audiogram of four hens: hearing range 9.1 Hz to 7.2 kHz at a 60 dB
  SPL criterion, best sensitivity at 2 kHz [S13]. Chickens also hear low
  frequencies better than humans do [S13].
- An earlier study found thresholds rise quickly below 400 Hz and above 4 kHz.
  The lowest thresholds were between 1 and 2 kHz [S14].
- Popular articles quote "60 to 12,000 Hz" with no peer-reviewed support.
  The proposal does not use that range.

So the cue carrier is **2 kHz**. It is the most sensitive frequency, and it is
inside the range of common piezo buzzers. Keep it between 1.5 and 2.5 kHz.
Do not go above 4 kHz.

### Loudness

- Laboratory conditioning studies played sound cues to hens at **65 dB SPL** and
  the hens learned them [S15].
- Hens kept at 65 dB (background fans and chicken sounds) were the control
  group in a noise study. Hens exposed to 90 dB for one hour were more stressed
  and more fearful. 75 dB music also raised fear in a second experiment [S16].
  That study used the tonic immobility test.
- Hens kept under continuous 80 dB(A) laid fewer eggs than hens at 60 dB(A).
  The study found no clear physiological stress [S17].
- A 1975 study found that tones of 90 dB ended tonic immobility in every
  chicken, and the authors read this as startle [S18].
- In 4-week-old chicks, 65 to 75 dB reduced fear. 85 to 95 dB reduced
  comfort behaviour and weakened learning [S19]. These are chicks, not adults.
- No source sets a validated welfare limit for adult hens. The values above are
  indicative. All are small studies.

Proposal: **65 dB(A) at 1 m** from the speaker, hard maximum **70 dB(A) at 1 m**.
This is 5 dB below the level where one study saw more fear (75 dB), and 20 dB
or more below the levels linked to stress (90 dB).
A piezo buzzer has no level setting in dB. Set the drive (duty, voltage) in
firmware to a value measured with a sound level meter at 1 m, and compile in
the maximum. A phone app is enough for a first check; a real meter is better.

### Pattern, duration and delay

- **Pattern (fixed, never changed):** 4 beeps of 2 kHz, each 600 ms on and
  400 ms off, with 10 ms fade-in and fade-out. Total 4 s. Fades avoid a click
  at each beep start. The fade is design judgement.
  Beeps are easy to tell from other farm noise. No source tests this pattern.
- **Use the cue for nothing else.** Not for errors, not for service, not for
  tests. A cue that is not followed by food loses its meaning.
  Conditioning theory is textbook material; we have no hen-specific source for
  how fast the loss happens.
- **Delay:** the spinner starts **2 s after the cue ends**. Feed reaches the
  ground a few seconds later (auger transit time). So the cue starts about
  6 s before spinner start, and the feed lands a few seconds after that.
  The studies we found used a 15 s cue with the reward at once after the end of
  the cue [S15, S20]. They did not test a trace gap. A short gap of a few
  seconds is design judgement. We chose a short cue because of neighbour noise.
  If the hens learn slowly, test a longer cue (up to 15 s) first.
- **Never play the cue if the gate will reject the scatter.** The cue starts
  only after the safety gate and the hopper-empty check pass. If the feeder
  jams after the cue, it reports a fault. It does not repeat the cue.

### How to start

1. Install the feeder with feed and water unchanged.
2. From day 1, every scatter has the cue. Always cue, then feed. Never feed
   without the cue and never cue without the feed.
3. For the first week, scatter near where the hens already are and near the
   feeder, so they see feed land when they hear the cue.
4. Use the same cue, level and delay for the whole trial.

Hens in lab studies learned a cue within days (all hens reached the first
training phase after 3 days [S15]). Those were individually trained hens.
We have no data on how long a free-ranging flock takes.

### Neighbour noise

- Level falls about 6 dB for each doubling of distance (free-field estimate):
  `L2 = L1 - 20 x log10(r2 / r1)`. From 65 dB at 1 m: about 45 dB at 10 m,
  39 dB at 20 m, 33 dB at 40 m. Walls and trees lower it further. A real
  buzzer is directional, so measure.
- The Hungarian decree 27/2008 (XII. 3.) KvVM-EüM sets limits for facility
  noise in residential areas, including village areas: **50 dB by day
  (06 to 22) and 40 dB at night (22 to 06)** [S21]. We read these values from
  the decree text, but the assessment method and zone type matter. This is a
  design reference, not legal advice. Check with the local authority.
- The window ends at 18:00 and starts at 08:00, so no cue is played at night.
- Place the speaker pointing at the scatter area and away from neighbours.

## 3. Measuring learning

Goal: see if hens come to the scatter area after the cue.

### Measures

1. **Arrival latency.** Seconds from cue start until the first hen is in the
   scatter zone. Laboratory studies measured latency to reach the reward area:
   7 to 17 s depending on reward [S20].
2. **Hens in the zone.** Count hens within **R metres** of the scatter point at
   cue start minus 10 s (baseline), and at cue start plus 10 s, 30 s and 60 s.
   `R` depends on the spinner radius, which is still open. Use the radius of the
   scatter area.
3. **Cue-only probes.** A cue with no scatter shows if the hens react to the cue
   itself. Use few. Proposal: none in the first 14 days, then at most
   **1 per week**, and not more than 1 in 10 cues. Each probe is an unrewarded
   cue and weakens the link a little. Count a remote `cue` command as a probe.
4. **Anticipation behaviour.** Studies saw more switching between behaviours
   after a rewarded cue than after a neutral sound [S20]. Hens showed more
   preening and wing flapping before a positive event, and more head
   movement and locomotion before a negative event [S15]. Hens give food calls
   when they expect a reward [S22]. These are research measures. A volunteer
   can note them from clips, but they are not needed for a first test.

Suggested "learned" rule (proposal, no source): on 4 of 5 consecutive days, the
number of hens in the zone at 30 s after the cue is at least **30% of the flock
above the baseline**, and the first hen arrives within 30 s.

### Camera

The device events carry timestamps (see [architecture.md](architecture.md)).
The camera stream can use them:

1. Mark the stream at each `cue` and `scatter` event.
2. Keep a short clip from cue minus 10 s to cue plus 120 s.
3. Count hens in the zone by hand at the four time points, or with a simple
   detector. A first study can use hand counts on a few clips per week.
4. Store the counts with the event id, so cue-only probes and normal scatters
   can be compared.

### Warning signs

| Sign | Action |
|---|---|
| Hens run from or freeze at the cue | Stop. Lower the cue level. Check the cue sound. |
| Crowding, pushing or fast pecking at one point | Scatter wider and use more points. Lower the dose. |
| Pecking or aggression at the scatter point | Same. Review again after one week. |
| One or more low-ranking hens never get treats | Scatter wider, more points, more space. Dominant hens eat more at a feeder in free-range flocks [S23]. |
| Feed still on the ground after 30 minutes | Lower the dose. |
| Hens stop coming to the cue (loss of interest) | Check for cue-only plays and for scatter faults. |
| Hens eat less layer feed (feed refusal, lower egg output) | Lower the budget; check that the layer feed is available. |
| Hot weather | Hot hens eat less. Poultry extension advises limiting treats in heat [S24]. The feeder has no temperature sensor in v1. |

No source gives how much scatter area per hen is enough, or how many scatter
points. The proposal is to start wide and watch the camera.
We found no direct test that scatter feeding reduces bullying. Spreading
feed is a plausible fix, not a proven one.
In one free-range study, scattered feed and other enrichments had no
consistent effect on feather cover [S7].

## 4. Failure safety (welfare side)

**The feeder dispenses nothing for days.** This is safe. Treats are extra.
The flock's complete feed and water never depend on the feeder
([architecture.md](architecture.md), safety gate). The hens lose a
pleasant event, and the cue may lose its meaning after many missed scatters.
Nothing worse happens. Fault events tell the keeper.

**The feeder dispenses too much.** The limits protect the flock:

1. **Compiled-in maximums.** The dose per scatter, the budget per hopper, the
   daily total and the interval are fixed in the firmware. No remote command
   raises them. The backend can only lower them.
2. **Count what is dispensed.** The gate counts grams from measured auger
   steps and the per-hopper calibration, not from what the command asked for.
3. **Keep the counters through a reboot.** Store the daily grams and the last
   scatter time in non-volatile memory. A reboot loop must not reset the budget.
4. **Day boundary in quiet hours.** The budget resets at 00:00 local time. No
   scatter runs then, so there is no double dose around midnight.
5. **No clock, no scatter.** Without a clock that was synced at least once since
   boot, quiet hours are unknown, so the random scheduler does not scatter.
6. **Second limit on the motor controller.** The Einsy has its own step and run
   time limits and a hardware kill (see the prototype section in
   [architecture.md](architecture.md)).
7. **Worst case, one day.** The compiled-in maximums are per-site values.
   Set them at build time from that site's flock size `N`: `daily_total_g =
   B x N = 5 g x N`, and every other maximum in section 5 is its per-hen value
   times `N`. Then the gate caps the day at 5 g per hen, which is 5% of the
   reference feed intake `F`. The feeder budget `B` is 5% of `F`. The
   other 5% of the 10% treat limit is left for the keeper (section 1).
   The caps are only right for the flock size they were built for. A build for
   30 hens (150 g per day) on a 10-hen flock allows 15 g per hen per day, which
   is 15% of intake. Never ship a flock-30 build to a smaller flock.
   If a fault bypasses the gate, a stuck auger can empty a hopper. The hopper
   capacity then sets the worst case, so keep hoppers small. This is an open
   hardware point.

## 5. Firmware values (proposal)

All values are proposals. "Flock 20" is the reference flock. "Flock 30" is the
largest supported flock. The flock size is a build-time value. It is not a
protocol field, not a firmware setting and not a remote setting. For each site
the builder takes the flock size `N` and computes the compiled-in maximums as
the per-hen value times `N`. The "Flock 30" column is the result for `N = 30`,
and it is wrong for a smaller flock. The backend can lower values at runtime,
never raise them above the compiled-in maximum.

```
daily_total_g     = B x N          = 5 x N        (g per day)
daily_budget_g    = per-hen share x N              (per hopper, see the table)
max_dose_g        = per-hen dose x N               (per hopper, per scatter)
```

**Current firmware defaults.** `firmware/main/Kconfig.projbuild` now matches the
10-hen column: max dose 10/10/5 g, daily budgets 40/40/10 g, and daily total 50 g
(`5 x N` with `N = 10`). The defaults are the proposal for a 10-hen flock and must
be derived per site from the flock size.

**Worst case with the defaults on a smaller flock.** The daily total of 50 g
is shared by `N` hens, so each hen can get up to `50 / N` g per day.

| Flock | Worst case per hen per day | Share of `F` (100 g) |
|---|---|---|
| 10 | 5 g | 5% (intended) |
| 8 | 6.3 g | 6% |
| 5 | 10 g | 10% (the whole treat limit) |
| 3 | 16.7 g | 17% |

A flock below 10 hens is outside the supported scope (section 1). Still, a
build must never use the defaults for such a flock. Set
`FEEDER_DAILY_TOTAL_G` to `5 x N` first.

| Name | Unit | Per hen | Flock 20 | Flock 30 (compiled-in max) | Basis |
|---|---|---|---|---|---|
| `max_dose_g`, hopper grain | g per scatter | 1.0 | 20 | 30 | Section 1 |
| `max_dose_g`, hopper pellets | g per scatter | 1.0 | 20 | 30 | Section 1 |
| `max_dose_g`, hopper mealworm | g per scatter | 0.5 | 10 | 15 | Section 1, own calculation |
| `max_scatter_total_g` (mix) | g per scatter | 1.25 | 25 | 37.5 | `B / n` |
| `daily_budget_g`, hopper grain | g per day | 4 | 80 | 120 | Section 1 |
| `daily_budget_g`, hopper pellets | g per day | 4 | 80 | 120 | Section 1 |
| `daily_budget_g`, hopper mealworm | g per day | 1 | 20 | 30 | Section 1, own calculation |
| `daily_total_g` | g per day | 5 | 100 | 150 | `s x 10% x F`, `F` = 100 g |
| `min_interval_s` | s | n/a | 3600 | 3600 | Design judgement |
| `scatters_per_day` | count per day | n/a | 4 | 6 (hard max) | Design judgement |
| `window_start` | local time | n/a | `max(08:00, sunrise + 60 min)` | not before 08:00 | Section 1 |
| `window_end` | local time | n/a | `min(18:00, sunset - 120 min)` | not after 18:00 | Section 1, judgement |
| Quiet hours | local time | n/a | outside the window | 18:00 to 08:00 at least | Section 1, 2 |
| `cue_freq_hz` | Hz | n/a | 2000 | 1500 to 2500 | [S13, S14] |
| `cue_pattern` | n/a | n/a | 4 beeps, 600 ms on, 400 ms off, 10 ms fades | fixed | Design judgement |
| `cue_duration_ms` | ms | n/a | 4000 | 4000 | Design judgement, [S15, S20] use 15 s |
| `cue_level_db` | dB(A) at 1 m | n/a | 65 | 70 (hard max) | [S15, S16] |
| `cue_to_scatter_delay_ms` | ms from cue end to spinner start | n/a | 2000 | 2000 | Design judgement |
| `cue_only_per_week` | count | n/a | 1 (none in first 14 days) | 1 | Design judgement |
| `flock_size` (`N`, input to the formulas above) | hens | n/a | 20 | 30 | Build-time value for the site. Not a runtime or remote setting |

Rejection reasons already in the protocol cover these limits: `quiet_hours`,
`min_interval`, `dose_limit`, `daily_budget_exceeded`, `no_clock`. A scatter
count limit and a cue-only limit need new reasons in `protocol/`. That is a
separate change.

## Open points for the owner and the welfare reviewer

1. **Feeder share of the treat limit.** 5% (`s = 0.5`) or the full 10%?
   Extension sources range from no scratch [S3] to 10 to 15% [S2].
2. **Reference intake `F`.** 100 g is the conservative choice. Commercial
   hybrids eat 109 to 117 g [S6].
3. **2-hour buffer before sunset.** No source supports it directly. Keep,
   shorten or drop it.
4. **Mealworm cap of 1 g per hen per day.** No source. A poultry nutritionist
   should check it.
5. **Cue length and delay.** 4 s cue with a 2 s gap is our choice. Lab studies
   used 15 s with no gap. Do we accept this, or test 15 s first?
6. **Cue level.** 65 dB(A) at 1 m, hard maximum 70. Confirm after a field
   measurement with a sound level meter.
7. **Flock size.** `N` is fixed at build time. When a keeper changes the
   flock, the site needs a new build (or a new signed config). A remote raise
   must stay impossible. Do we accept a rebuild, or do we want a local
   (not remote) install setting?
8. **Artificial light.** If the coop is lit, the window follows the light
   schedule and not the sun.
9. **Remote `cue` command.** It plays a cue without food. Limit it to
   `cue_only_per_week`, or remove it from the protocol?
10. **Heat.** Skip scatters on hot days? v1 has no temperature sensor.
11. **"Learned" rule.** The 30% of flock above baseline rule is our proposal.
12. **Hopper size.** Small hoppers cap the worst case if the gate fails.

## Sources

Access date for all links: 2026-11-05. Some pages were read through search
excerpts and not in full. Those are marked "excerpt".

- [S1] University of Maryland Extension, Feeding the Flock (scratch grain about
  10% of daily intake): <https://extension.umd.edu/resource/feeding-flock>
- [S2] Oregon State University Extension, How to feed your laying hens, PNW 477
  (scratch 10 to 15%; excerpt):
  <https://extension.oregonstate.edu/catalog/pnw-477-how-feed-your-laying-hens>
- [S3] Penn State Extension, Successfully Raising a Small Flock of Laying
  Chickens (advises no crack corn or scratch; excerpt):
  <https://extension.psu.edu/successfully-raising-a-small-flock-of-laying-chickens>
- [S4] Purina Mills, treats and snacks (treats 10% or less; industry source):
  <https://www.purinamills.com/chicken-feed/education/detail/treats-snacks-are-they-good-for-my-hens>
- [S5] Alabama Cooperative Extension System, Nutrition for Backyard Chicken
  Flocks (grain dilutes the diet; 0.18 to 0.27 lb per hen per day in Table 3;
  scratch finished in 1 to 3 hours; limited late-afternoon scratch):
  <https://www.aces.edu/blog/topics/farming/nutrition-for-backyard-chicken-flocks/>
- [S6] Hy-Line Brown Commercial Layers, Performance Standards Guide (feed
  109.1 to 116.9 g per bird per day, 19 to 100 weeks; protein 17.85 g per day
  at peak; up to 16 h light):
  <https://www.hyline.com/filesimages/Hy-Line-Products/Hy-Line-Product-PDFs/Brown/BRN%20STD%20ENG.pdf>
- [S7] Brocklehurst et al. 2022, Do Hens Use Enrichments Provided in Free-Range
  Systems? (hens most interested in scattered pelleted feed; no clear effect on
  feather cover): <https://www.ncbi.nlm.nih.gov/pmc/articles/PMC9031176/>
- [S8] Dine A Chook, dried mealworms (53% protein and 28% fat on the product
  page; 10 to 12 worms per hen every second day; under 10% of diet; retailer):
  <https://www.dineachook.com.au/dried-mealworms-1-7kg> and
  <https://dineachook.com.au/blog/dried-mealworms-for-chickens>
- [S9] Cluckin.net, mealworms for chickens (no more than 4 dried per day;
  hobby site): <https://cluckin.net/mealworms-for-chickens.html>
- [S10] Manna Pro, are mealworms good for chickens (10 to 15 dried per bird per
  day; industry blog): <https://mannapro.com/blogs/news/are-mealworms-good-for-chickens>
- [S11] Farmers Weekly (South Africa), Expert advice on feeding layer hens
  (hens fill the crop before dark; intake peaks 16:30 to 18:00; calcium for the
  next egg): <https://www.farmersweekly.co.za/animals/poultry/expert-advice-on-feeding-layer-hens/>
- [S12] Poultry Producer, late-afternoon feeding and shell thickness (breeder
  hens): <https://poultryproducer.com/?p=6184>
- [S13] Hill, Koay, Heffner and Heffner 2014, Audiogram of the chicken, J Comp
  Physiol A 200:863-870 (range 9.1 Hz to 7.2 kHz at 60 dB SPL, best at 2 kHz;
  the abstract was read through a search excerpt):
  <https://pubmed.ncbi.nlm.nih.gov/25092127/> and
  <https://doi.org/10.1007/s00359-014-0929-8>
- [S14] Psychoacoustics of normal adult chickens: thresholds and temporal
  integration (lowest thresholds 1 to 2 kHz; excerpt):
  <https://researchconnect.buffalo.edu/en/publications/psychoacoustics-of-normal-adult-chickens-thresholds-and-temporal-/>
- [S15] Zimmerman, Buijs, Bolhuis and Keeling 2011, Behaviour of domestic fowl
  in anticipation of positive and negative stimuli, Animal Behaviour 81:569-577
  (cues played at 65 dB, 15 s cue, mealworm reward; training reached after
  3 days for the first phase): <https://doi.org/10.1016/j.anbehav.2010.11.028>
- [S16] Campo, Gil and Davila 2005, Effects of specific noise and music stimuli
  on stress and fear levels of laying hens of several breeds, Appl Anim Behav
  Sci 91:75-84 (90 dB versus 65 dB; 75 dB music; excerpt):
  <https://agris.fao.org/search/fr/records/65119d3460f8dcc51c5fc228>
- [S17] Effect of low light and high noise on behavioural activity,
  physiological indicators of stress and production in laying hens (60 versus
  80 dB(A); excerpt):
  <https://pure.sruc.ac.uk/en/publications/effect-of-low-light-and-high-noise-on-behavioural-activity-physio/>
- [S18] Hatton and Thompson 1975, tone intensity and tonic immobility in
  chickens (90 dB ended immobility; known only through a search excerpt):
  <https://link.springer.com/article/10.3758/BF03336705>
- [S19] Effects of Different Auditory Environments on Behavior, Learning
  Ability, and Fearfulness in 4-Week-Old Laying Hen Chicks (chicks, not
  adults): <https://www.ncbi.nlm.nih.gov/pmc/articles/PMC10572013/>
- [S20] McGrath, Burman, Dwyer and Phillips 2016, Does the anticipatory
  behaviour of chickens communicate reward quality? Appl Anim Behav Sci
  184:80-90 (15 s anticipation period; latency to reward 7 to 17 s):
  <https://repository.lincoln.ac.uk/articles/journal_contribution/Does_the_anticipatory_behaviour_of_chickens_communicate_reward_quality_/24359269>
- [S21] Hungarian decree 27/2008 (XII. 3.) KvVM-EüM, Annex 1 (50 dB day, 40 dB
  night in residential areas; read through a fetch summary):
  <https://njt.jog.gov.hu/jogszabaly/2008-27-20-0N>
- [S22] McGrath et al. 2017, Hens vary their vocal repertoire and structure when
  anticipating different types of reward, Animal Behaviour 130 (excerpt):
  <https://pure.sruc.ac.uk/en/publications/hens-vary-their-vocal-repertoire-and-structure-when-anticipating-/>
- [S23] Hierarchical structure and feeding behaviors of free-range hens (more
  dominant hens ate at the feeder; excerpt):
  <https://www.researchbank.ac.nz/items/ab4a9f22-1186-4d29-8ef6-bef98516fda0>
- [S24] Poultry Extension, Managing backyard chickens during extreme summer
  temperatures (limit treats in heat; excerpt):
  <https://poultry.extension.org/articles/poultry-management/managing-backyard-chickens-during-extreme-summer-temperatures/>
