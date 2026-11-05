# Idea: reward-treat feeder

## Source

Item 06 of the Remény Farm idea sheet for corporate partners (September–October
2026, Hungarian). Original text, translated:

> **Reward-treat feeder.** At random times, after a short sound cue, it scatters
> a small amount of feed. The hens learn the cue and have something to look
> forward to: movement and activity, not just feeding. The app shows the
> scatter, and the patron can watch it live.

## Concept

1. A scheduler picks random times inside an allowed daily window.
2. At each chosen time the feeder plays a short, consistent sound cue.
3. After a short delay it scatters a small, fixed portion of treats or feed.
4. The device reports each event (cue, scatter, portion, time) to a backend.
5. The app shows the event. Patrons watch the scatter on a live camera stream.

## Why

- **Enrichment.** A learned cue followed by a reward gives the hens anticipation
  and a reason to move and forage.
- **Connection.** Remote patrons see a real moment on the farm when it happens,
  not only photos after the fact.
- **Low effort.** The device runs on its own; farm staff only refill and clean it.

## Constraints

- **Animal welfare first.** The cue volume, the portion size and the number of
  scatters per day must suit the hens. Their normal feed and water never depend
  on the feeder.
- **Small portions.** Treats stay a small share of the daily diet.
- **Outdoor hardware.** Dust, moisture, droppings, temperature swings and pecking.
- **Safe failure.** A jam, an empty hopper or a lost connection must not harm the
  birds. The device reports these faults.

## Open questions

Answered in [architecture.md](architecture.md):

- Mechanism: one auger per hopper, shared spinner.
- Feed type: three hoppers (grain, pellets, dried mealworms); dry, free-flowing feed only.
- Power: 230 V mains at the coop.
- Connectivity: Wi-Fi, outbound MQTT over mutual TLS; the random schedule runs offline.
- Integration: MQTT topics and JSON Schemas in `protocol/`.

Still open:

- Cue: which sound, how loud, how long before the scatter? (proposal in
  [welfare.md](welfare.md))
- Schedule: how many scatters per day, in which time window? (proposal in
  [welfare.md](welfare.md))
- Live view: does the feeder trigger or bookmark the camera stream?
- Measurement: how do we tell whether the hens learned the cue?
