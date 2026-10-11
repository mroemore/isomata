# Isomata Agent Framework — Design Spec

Status: draft for review
Date: 2026-10-11
Replaces: the ad-hoc customer brain (`src/sim/brain.c` BrainPhase, the
parallel T21 executor FSM, the hardcoded critter patrol loops).

## Purpose

Turn Isomata's entity layer into a general agent framework suited to a
business-tycoon game. Every agent runs on a want ladder: it adopts goals
(errands), pursues them as jobs, and — whenever blocked — does secondary
activities to keep its needs and happiness up. The behaviour emerges from
needs, urgency and pre-computed job bands; there is no scripted sequence and
no per-agent logic. The laundromat stays the reference consumer: every
general mechanism must be exercised by the running game, not just tests.

Success means: a new activity or errand is a **data row**, not new code; the
whole framework is pure and deterministic under a fixed dt; and the
laundromat visibly shows entities doing meaningful things while they wait.

## Vocabulary

Five distinct nouns. Keeping them distinct is the core of the design.

- **Need** — a named continuous quantity on an agent (0..1), decaying over
  time. Has a band (below), a decay rate, and an urgency weight.
- **Activity** — a *declaration* of something an agent can do. Says which
  needs it serves and how well, whether it requires a location, a duration,
  a cooldown, and required competencies. Registered once, permanent.
- **Job** — a *live instance* of an activity bound to a target, with state,
  progress, claimer, expiry and priority. Every activity can become a job.
- **Errand** — a multi-step commitment an agent owns and advances through
  jobs, with its own bookkeeping (e.g. garments). Hours-long lifetime.
- **Board** — where unclaimed jobs wait. A *personal* board (one agent's own
  errands; single consumer, no claiming) or a *shared* board (staff pull
  from it; multiple consumers, so claims are exclusive and time-limited).

An activity is never intrinsically "primary" or "secondary" — only the
*producer* differs. An arcade game is a job a customer's `PLAY_ARCADE`
errand enqueued, and equally a job a blocked wash customer picks for
itself from an empty board. One declaration, one executor, two producers.

## The want ladder

Needs carry a **tier** (Maslow-inspired, data-driven per game):

- 0 Survival — physical safety, panic.
- 1 Sustenance & safety — energy, hunger, hygiene, machine availability.
- 2 Belonging — social contact, company.
- 3 Esteem — competence, reputation, keeping promises.
- 4 Fulfilment — leisure, curiosity.

Urgency is **banded** (integers, ~0..15), not a raw float, so two activities
of nearly equal importance land in the same band and cannot displace each
other — this is what prevents oscillation.

**Soft falloff (not strict gating).** `wantUrgency = tierWeight × needDeficit`,
where tier weight falls off sharply with height. A *critical* survival need
always wins; a *mild* survival need can lose to a severe social one. We get
the ladder's structure without the dead corners of strict gating.

**Three regimes per need.** Each need is one value read three ways:
`content` → `deficit/urgency` → `panic` (above a threshold). Panic is a mode
change: while a need panics, higher-tier wants are suppressed, and the panic
level decays independently after its cause clears. This is how "a dryer is on
fire" reprioritises everyone without any special-case fire code — a machine in
FIRE state raises nearby agents' survival need, and the arithmetic handles the
rest.

**Influences act on needs only.** World facts (fire, flood, theft, injury)
move needs; agents then independently react. Nothing posts an "emergency job".
One mechanism, uniform, composable.

## Urgency, preemption, staying power

A job's priority is **computed from the needs it serves**, not assigned by
whoever queued it. Provenance (personal vs shared board) only decides *may
this agent claim it*, never ordering.

Selection happens at exactly two moments:

- **when idle** — take the best (highest band) job available;
- **on a challenger** — switch only if `challenger.band >= engaged.band +
  PREEMPT_MARGIN` (start at 2 bands) *and* the engaged job's dwell
  (`MIN_DWELL` seconds, or its first real yield, whichever is longer) has
  expired.

Between those moments nothing reconsiders — no per-tick re-ranking, so no
oscillation, no per-tick cost, no drift. Preemption is **full** (anywhere) but
governed by the band gap and dwell, so an agent cannot be short-circuited by
two near-equal activities.

Priority classes exist only as board provenance:

- EMERGENCY — survives as "a panicking agent suppresses its filler", not a class.
- ERRAND — an agent's own committed goal work.
- ASSIGNED — a job from the shared board.
- FILL — self-generated secondary activities.

An errand's continuation always outranks the agent's own filler, so the moment
a machine finishes and its `COLLECT` job becomes available, it preempts the
arcade game mid-animation.

## Errand adoption (two producers, one ladder)

A new errand enters an agent's list in one of two ways, both landing the same
way — as work on a board, arbitrated by the ladder:

- **Need-driven template matching** — needs are scanned against errand
  templates registered for the agent's role; the template whose requirement is
  most unmet is adopted. A dirty-garment customer adopts "get laundry done" on
  their own. Adding baseline behaviour = adding a template row.
- **World-seeded** — a world event (fire, complaint, machine fault) offers an
  errand at a **declared priority floor**. It is adopted at that tier and then
  wins or loses like everything else; it is never forced. A tier-0
  fire-suppression beats an arcade trip; a tier-3 complaint does not beat an
  urgent wash.

An errand never competes for attention directly — it only enqueues jobs. "Go
back to the wash or keep playing" is just two jobs on one board compared by
band. The errand is the only thing that remembers why the jobs exist.

## The agent

```c
typedef struct Agent {
    int   role;                       /* data, not code */
    float needs[NEED_COUNT];          /* 1.0 = content */
    float panic[NEED_COUNT];
    float needDecay[NEED_COUNT];      /* scaled by the role profile */
    uint16_t skills[SKILL_COUNT];     /* fixed competency levels */
    uint16_t flaws[SKILL_COUNT];      /* deficiencies, independent of skill */
    uint8_t  flags;
    EntityHandle entity;              /* the physical body */
    int      personalBoard;
} Agent;
```

Needs are a **fixed-size vector with named slots** (`NEED_COUNT` a compile-time
constant); adding a need is a table row plus an index, keeping needsUpdate a
tight loop and the state trivially inspectable for determinism.

**Per-role curves.** A `RoleProfile` gives each role a decay multiplier per
need and a panic threshold — a staffer at a desk gets hungry faster and bored
slower than a waiting customer, with no special code.

**Competencies modify, deficiencies can fail.** Skills shorten durations and
raise quality; a flaw (clumsy) can make a job fail outright. Low competency
never *disqualifies* an agent, so a bad hire can't deadlock the sim — staffing
is a soft optimisation puzzle.

**Satisfaction is a want, not a score.** A `NEED_SATISFACTION` need decays
whenever an agent's errand is blocked or failing, regardless of the errand's
tier. A queued wash customer decays satisfaction (tier 1), not survival — fine,
just annoyed. Crossing a leave-threshold abandons the errand and the agent
walks out the door with a reputation/money consequence at the exit. This is the
whole "takes their business elsewhere" loop, driven by a want rather than a
special-cased impatience counter.

## Boards and jobs

- **Personal board** — the agent's own errands' jobs. Single consumer, no
  claiming.
- **Shared board** — staff pull from here. Claims are **exclusive and
  time-limited**: a worker claims a job; if it isn't started within a
  reservation window it is released back, so nobody sits on a job forever.

A job either targets a **thing** (a machine — claimed exclusively, like the
existing `machineClaim`) or is **in-place** (no conflict) — one boolean in the
job record, mirroring the machines model. **Long-running** jobs (a wash) hold a
claim for their duration but are interruptible: a panic pulls the owner off and
the claim returns to the board. **Short** jobs (repair, tidy) are atomic.

Cancel **releases held resources** (a preempted `USE_MACHINE` releases its
claim); payments and machine-starts are **atomic commit points** (you can't be
halfway through paying).

## Player control

The model exposes the operations a tycoon UI needs (assign job→agent,
reprioritise, cancel errand, set skill) and the existing debug overlay
surfaces them. **No UI ships in this work** — the model is shaped so a UI can
be added later without touching the AI.

## The per-job lifecycle (where `fsm.c` goes)

One table-driven lifecycle machine drives every job:

```
UNCLAIMED → CLAIMED → WALKING → INTERACTING → WAITING → DONE
                              ↘ DENIED / INTERRUPTED / TIMEOUT → FAILED
```

This is nearly today's T21 FSM (`IDLE → MOVING_TO → ACTING → WAITING → DONE`
with `START/ARRIVED/DENIED/PHASE_DONE/INTERRUPTED/TIMEOUT`), retargeted from
"the agent" to "one job". A table is the right tool for a fixed 5-state
lifecycle and a wrong tool for an agent that also plans, targets and manages
needs — the current conflation happened because the FSM was asked to be the
brain. `fsm.c` survives, with its scope corrected.

## The tick

`agentUpdate(world, agent, dt)` — pure, no allocation, deterministic:

1. needsDecay — values fall by decay × role curve, accelerating as a need
   nears empty (so it panics rather than idles).
2. panicDecay — panic falls independently, faster when its need is healthy.
3. world influence — a machine on FIRE nudges survival for agents in sight.
4. band recompute — each relevant job → integer urgency band.
5. engage decision — idle ? best job : top challenger only if it clears
   `band + MARGIN` and the dwell has expired.
6. execute — the job lifecycle machine.
7. errand advance — completed/failed step → next step, or done/abandoned.

Steps 4–5 are the only place an agent changes its mind. Priority comes from
utility, not a table; ordering is emergent.

## Delivery — incremental, laundromat-driven

Each phase ends in something watchable in the running game; the laundromat is
the constant consumer and every general mechanism is exercised by it.

1. **Needs + ladder** — need vector, decay, panics, band utility; critters get
   `boredom` (visibly restless) with no jobs yet.
2. **Jobs + lifecycle** — job record, the retargeted lifecycle table, personal
   board; critters' hardcoded patrol loops become jobs on a board.
3. **Activity catalogue** — activities with location flags; critters get fill
   activities (first visible "they do things while idle").
4. **Errands** — templates, need-driven adoption, gates for waits; the
   customer's hardcoded flow becomes a template-driven errand.
5. **Shared board + staff** — claims, time limits, competencies, a repairer;
   world-seeded errand for a machine fault.
6. **Satisfaction → business** — the departure ledger: revenue, reputation,
   return-visit memory, defection. *(The satisfaction need itself and the
   abandon-on-threshold rule land with the customer migration in phase 4,
   because a customer errand without an abandon path can hang forever — the
   phase must be independently demonstrable. Phase 6 is the money and
   reputation half of the same loop.)*

Per your instruction, the current demo critters keep their patrol loops as a
"wander to a spot" *activity* (nothing thrown away), gaining needs and fill
activities so they become the testbed before customers move over.

**The observability work ships inside these phases** while the framework is
fresh — it is the cheap part and it pays for itself immediately:
phase 1 adds the per-agent needs overlay (bands + panic) and the perf counter;
phase 2 adds the why-this-job log and the board view; phase 3 adds the
band-tint overlay. Each phase reports its measured perf numbers (agent
decisions/s, A\* invocations/s) so the 60fps target is evidenced, not assumed.

## Migration

`src/sim/brain.c`'s `BrainPhase` switch and the parallel executor FSM both
dissolve into "jobs on a board". The customer's current hardcoded flow becomes
an errand template; the machines model is reused as-is (a claimed machine *is*
a claimed thing). The laundromat keeps working throughout — no phase may leave
the running game without a working demo.

## Performance

The per-agent math is tiny (needs decay over a handful of floats, one band
recompute, one integer comparison), so **the framework is not the perf risk —
the renderer is**. The design keeps the hot path cheap by construction:

- **Fixed capacity, flat arrays, no allocation in the tick** — the existing
  pattern (`Agent slots[AGENT_MAX]`) extends directly; allocation in the hot
  path is what we forbid.
- **Decision points, not per-tick rescoring** — an agent only *decides* when
  idle or when a challenger appears; otherwise it executes. Needs still decay
  per tick, but that's a tight contiguous float loop.
- **Dirty-flagged band recompute** — a job's band is recomputed only when a
  need it serves crosses a band boundary that tick, making the common case
  near-zero.
- **One shared board for staff**, so N staff scan one board in one claim pass,
  not N boards each.
- **A\* remains the only expensive call**, already bounded (fixed node store,
  TOO_LONG cap) and only invoked when an agent picks a new target — which the
  dwell + preemption margin reduce by design (fewer redundant re-paths).
- **Instrumentation, not guesswork** — a build-timed counter of agent decisions
  and A\* invocations per second, added in phase 1, so any future frame-budget
  concern has a number instead of a hunch.

Target: real-time (60fps) for several hundred agents on the low-end Android
target we already build for. This is **measured and reported, not a hard
ratchet gate** — if a measured run falls short, we say so with numbers rather
than blocking the build.

## Developer usability (extension experience)

The whole bet is that **adding behaviour is data**, so authoring is a feature:

- **One registry table per category** (activities, errand templates, needs,
  roles, skills), each a plain C table of named rows with a comment per row.
  Adding "PLAY_PINBALL" is one row plus (if it needs art) a texture line.
- **A uniform activity row** — id, name, needs-served + rate, location flag
  (or in-place), duration, cooldown, competency gate. A fixed shape means new
  activities are copy-and-edit and the compiler catches mistakes.
- **Intent-revealing names** so the tables read like the design:
  `ACT_PLAY_ARCADE`, `ERR_LAUNDRY_GET_CLOTHES_CLEAN`, `NEED_ENERGY`.
- **The `legend.txt` discipline carries over** — this codebase already proved a
  text/table manifest beats code for authoring.
- **Unknown activity/template id is a hard error with a diagnostic** (like the
  palette bug the review caught), never a silent no-op.

## Observability

The existing light-debug overlay and path overlay are the foundation; we grow
them so the emergent behaviour is *watchable*, which is what a tycoon game
needs:

- **Per-agent needs readout** — each need's bar with its band, panic
  highlighted; plus engaged job + band, errand + step, claim, competencies.
  Selectable later for the UI; exposed now via the overlay.
- **Why-this-job log** — when an agent picks or switches, log the two bands and
  the margin that decided it. The single highest-value artifact: it turns "why
  did that guy walk over there" from archaeology into a log line.
- **Board view** — jobs by band, claimers, expiries; contention visible at a
  glance.
- **Band/tier tint overlay** — optional colour tinting agents by top urgency, so
  a crowd going panicky is visible as colour.
- **Honest logging discipline** — logs and reports state what was *observed*,
  not what was intended; screenshot claims describe legibility honestly (a
  lesson from the T23 review).

## Testing

Pure and deterministic throughout. Needs, decay, panics, bands, the
lifecycle table, and each errand template are individually exact-tested. The
want ladder gets a focused table test (tier ordering, soft falloff, panic
suppression, band-gap preemption, dwell). An integration test steps a small
world with a fixed dt and asserts the emergent story (agents adopt errands,
block, fill time, pre-empt, and leave) is *replayable* — same input, same
output — rather than asserting a specific interleaving, which would be
brittle. Floors and the existing suite must stay green throughout.

**Property-style checks** where cheap — the invariants that catch emergent bugs
a fixed-assertion test misses: no agent oscillates (the engaged job's band is
monotone non-decreasing within a dwell), no job is double-claimed, every claim
is eventually released, no agent starves (every agent's needs stay above 0 for
bounded stretches).

## Open / deferred

- Per-agent needs only for now; places/environments influencing agents
  (crowded room, grimy lobby) is a natural later extension, deliberately not
  designed in yet.
- UI for tycoon control: model-ready, not built.
- Persistence/save-load of agents, needs and boards is implied by the pure
  data layout but is out of scope for these phases.