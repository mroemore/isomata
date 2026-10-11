# Agent Framework Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace Isomata's ad-hoc customer brain with a general agent framework in which every entity runs on a want ladder — adopting errands as jobs, and filling blocked time with secondary activities — so adding behaviour is a data row rather than code.

**Architecture:** A new `src/agents/` package of pure modules, layered so each task stands alone: needs + the want ladder (urgency banding), then jobs + their lifecycle machine, then boards (claims), then the activity catalogue, then errands (need-driven + world-seeded), then shared-board staffing with competencies, then the business ledger. The existing T21 executor FSM (`src/ai/fsm.c`) is retargeted from "the agent's brain" to "one job's lifecycle". `src/sim/brain.c` and the hardcoded `DemoWalker` patrol loops dissolve into jobs on boards. The laundromat stays the visible consumer: no task may leave the running game without a working demo.

**Tech Stack:** C11, meson, SDL3 (app tier only — every new module is pure), Unity test harness with twin `tests/test_<module>.c` files, CTOL coverage floors, Vulkan/SDL_gpu for evidence captures.

**Spec:** `docs/superpowers/specs/2026-10-11-isomata-agent-framework-design.md` — the plan argues from the spec, so read both. Every decision below is already made; do not re-open them.

## Global Constraints

- **Purity:** every module under `src/agents/` is SDL-free, allocation-free in the tick, and free of wall-clock time. All time arrives as the caller's `dt`. Pure modules pair 1:1 with twin test files: adding `src/agents/x.c` to `ctol_sources` (root `meson.build`) requires `tests/test_x.c` or the build fails at configure time.
- **Determinism:** identical inputs produce identical output. No hash iteration order, no pointer-value ordering, no floating-point accumulation whose order depends on a hash. Ties break on stable integer ids.
- **Naming:** Spectrax house style — camelCase functions, PascalCase types, tabs, `X_` prefixes, `X_COUNT` sentinels. Existing precedent: `src/sim/selection.h`, `src/ai/pathfind.h`.
- **Build wiring:** new sources need three edits in the same change — root `meson.build` (`ctol_sources`), `android/app/jni/CMakeLists.txt`, and `meson/coverage-min.txt` (floor from measured coverage, raise-only thereafter).
- **dt clamping:** mirror the existing `ENTITY_MAX_DT` 0.1f bound in every per-tick loop, NaN-safe (`!(dt > 0)`).
- **Testing discipline:** every task adds exact-value tests, not coverage-filling ones. Coverage floors are set from measured values and never lowered.
- **Evidence:** all desktop GUI runs go under Xvfb (TESTING.md recipe, `SDL_AUDIODRIVER=dummy`), never the live display. Android evidence uses the headless emulator (`/home/krang/android-sdk/emulator/emulator`, AVD `roecounter_test`, `adb emu kill` after). Never write to `/tmp` — use `.tmp_files/`.
- **Honest reporting:** a report states what was *observed*, never what was *intended*. Screenshot claims describe legibility honestly. If an acceptance item cannot be met, report that with numbers rather than reinterpreting the claim.
- **Scope fence:** no UI, no save/load, no environment/place influences, no entity-vs-entity collision avoidance. These are explicitly deferred in the spec.

## Review Focus

The five failure modes most likely to bite a person using this framework, each with the task that owns the fix:

1. **Oscillation** — an agent flip-flops between two near-equal activities. Owner: Task 1 (band + margin math) and Task 3 (the switch decision). Test: engaged band is monotone non-decreasing within a dwell.
2. **Stale claim** — a preempted or abandoned job leaves a machine claimed forever, so the sim deadlocks. Owner: Task 3 (cancel releases) and Task 5 (errand abandon). Test: every claim is released on cancel, abandon, or failure.
3. **Starvation** — one agent hoards a shared job or a need never recovers, so an agent quietly stops acting. Owner: Task 3 (reservation expiry) and Task 1 (panic decay). Test: claims have an expiry; a panicking need decays its panic even while its cause persists.
4. **Priority inversion** — a filler activity outranks the errand that would complete the agent's goal, because the filler serves a lower-tier need at a higher deficit. Owner: Task 4 (activity urgency aggregation). Test: a critical tier-1 errand continuation outranks a severe tier-4 fill.
5. **Unknown-reference failure** — a job or errand names an activity/template/things that doesn't exist, and the agent silently does nothing forever. Owner: Task 4 (catalogue lookup) and Task 5 (template expansion). Test: an unknown activity id is a reported error, never a silent no-op.

---

## File Structure

New package `src/agents/` — one responsibility per file:

| File | Responsibility |
|---|---|
| `src/agents/need.h/.c` | The need vector, decay, panic, tier weights, band urgency — `wantUrgency`, the only place ordering is computed. |
| `src/agents/agent.h/.c` | The `Agent` record (needs + competencies + handles), role profiles, the fixed-capacity registry, `agentsDecay`. |
| `src/agents/job.h/.c` | The `Job` record and its table-driven lifecycle machine (the retargeted FSM). |
| `src/agents/board.h/.c` | Personal and shared boards: enqueue, claim, reserve, release, expire, priority scan. |
| `src/agents/activity.h/.c` | The activity catalogue (uniform rows), lookup by id, and the urgency of a job from the needs it serves. |
| `src/agents/errand.h/.c` | Errand templates, need-driven and world-seeded adoption, step advance, gates, abandon. |
| `src/agents/ledger.h/.c` | Departure accounting: money, reputation, return-visit memory, defection. |

Modified, not created: `src/ai/fsm.h/.c` (retargeted to the per-job lifecycle; the old event names are superseded by Task 2), `src/scenes/level_scene.c` (critters → agents on boards; overlay growth), `meson.build`, `android/app/jni/CMakeLists.txt`, `meson/coverage-min.txt`.

Deferred, deliberately absent: `src/agents/place.*` (environment influences), `src/agents/skilltree.*` (growth), anything SDL-facing.

---

### Task 1: Needs, the want ladder, and the agent record

The foundation: what an entity *wants*. Nothing acts on it yet, but the ladder's arithmetic — tier weights, banding, panic suppression — is the load-bearing invariant of the whole framework, so it is pinned exactly here.

**Files:**
- Create: `src/agents/need.h`, `src/agents/need.c`, `tests/test_need.c`
- Create: `src/agents/agent.h`, `src/agents/agent.c`, `tests/test_agent.c`
- Modify: `meson.build` (`ctol_sources`), `android/app/jni/CMakeLists.txt`, `meson/coverage-min.txt`

**Interfaces:**
- Consumes: nothing (first task). House dt bound `0.1f`.
- Produces:
  - `typedef enum Need { NEED_SURVIVAL, NEED_ENERGY, NEED_SOCIAL, NEED_SATISFACTION, NEED_ESTEEM, NEED_FULFILMENT, NEED_COUNT } Need;` — six needs, fixed-size vector.
  - `typedef struct NeedSet { float value[NEED_COUNT]; float panic[NEED_COUNT]; } NeedSet;`
  - `void needSetInit(NeedSet *s);` — every value and panic to 1.0 (content).
  - `void needDecay(NeedSet *s, const float decayMul[NEED_COUNT], float dt);` — dt clamped to `NEED_MAX_DT 0.1f`, NaN-safe. Deceleration accelerates near empty: effective rate is `decayMul[n] * (1.0f + (1.0f - value) * NEED_PANIC_RAMP)`. Clamp each value at 0.0 (never negative).
  - `void needPanicDecay(NeedSet *s, float dt);` — panic falls by `NEED_PANIC_DECAY` per second, floored at 0.0; while `value[n] < panicThreshold` the panic is *held* (not decayed) — the cause persists.
  - `int needBand(const NeedSet *s, int need);` — 0..15.
  - `int needPanicMask(const NeedSet *s);` — bit `n` set when `panic[n] > 0.0f`.
  - `float needTierWeight(int need);` — soft falloff, monotone non-increasing in tier: SURVIVAL 1.00f, ENERGY 0.60f, SOCIAL 0.40f, SATISFACTION 0.60f, ESTEEM 0.25f, FULFILMENT 0.15f. *(SATISFACTION is tier 1: being blocked is an inconvenience, not an emergency.)*
  - `int wantUrgency(const NeedSet *s, int need);` — the single ordering function. Band of `tierWeight * deficit` scaled to 0..15; if the need itself is panicking, return `NEED_BANDS - 1` (15); otherwise if any need is panicking, the result is scaled by `NEED_PANIC_SUPPRESS` 0.75f.
  - `typedef enum AgentRole { ROLE_CUSTOMER, ROLE_STAFF, ROLE_CRITTER, ROLE_COUNT } AgentRole;`
  - `typedef enum AgentSkill { SKILL_REPAIR, SKILL_SPEED, SKILL_SERVICE, SKILL_COUNT } AgentSkill;`
  - `typedef struct RoleProfile { float decayMul[NEED_COUNT]; float panicThreshold[NEED_COUNT]; } RoleProfile;`
  - `const RoleProfile *agentRoleProfile(int role);` — NULL for out of range. CUSTOMER: all decay 1.0f, panic thresholds 0.15f. STAFF: `NEED_SOCIAL` decay 0.5f, `NEED_FULFILMENT` 0.5f, `NEED_ENERGY` 1.5f (a shift tires you), thresholds 0.15f. CRITTER: `NEED_SOCIAL` 0.25f, `NEED_ESTEEM` 0.0f, `NEED_SATISFACTION` 0.5f, thresholds 0.10f.
  - `typedef struct Agent { int id; int role; bool alive; NeedSet needs; uint16_t skills[SKILL_COUNT]; uint16_t flaws[SKILL_COUNT]; EntityHandle entity; int personalBoard; int engagedJob; float dwellLeft; int panicMask; } Agent;`
  - `#define AGENT_MAX 256`, `typedef int AgentHandle;` (`AGENT_INVALID` = -1).
  - `void agentsInit(AgentRegistry *r); AgentHandle agentCreate(AgentRegistry *r, int role); Agent *agentGet(AgentRegistry *r, AgentHandle h); void agentDestroy(AgentRegistry *r, AgentHandle h); void agentsDecay(AgentRegistry *r, float dt);` — fixed-capacity flat slots + `alive[]` + `liveCount`, exactly the `EntityRegistry` shape. `agentsDecay` runs `needDecay` then `needPanicDecay` for every live agent and refreshes `panicMask`. `AGENT_MAX_DT` 0.1f.
  - `typedef struct AgentStats { uint32_t agentDecisions; uint32_t pathfindCalls; } AgentStats; void agentsStats(AgentStats *out);` — the perf counter (zeroes on read; phases report the delta).

- [ ] **Step 1: Write the failing want-ladder tests**

```c
/* test_need.c — exact values, the spec's numbers */
TEST_ASSERT_EQUAL_INT(0, needBand(&content, NEED_SURVIVAL));       /* content -> band 0 */
TEST_ASSERT_EQUAL_INT(15, needBand(&empty, NEED_SURVIVAL));        /* empty -> band 15 */
TEST_ASSERT_EQUAL_FLOAT(1.00f, needTierWeight(NEED_SURVIVAL));
TEST_ASSERT_EQUAL_FLOAT(0.15f, needTierWeight(NEED_FULFILMENT));
/* soft falloff: a mild need loses to a severe lower-tier one */
TEST_ASSERT_TRUE(wantUrgency(&criticalSocial, NEED_SOCIAL) >
                 wantUrgency(&mildSurvival, NEED_SURVIVAL));
/* but a critical survival need beats a critical social one */
TEST_ASSERT_TRUE(wantUrgency(&criticalSurvival, NEED_SURVIVAL) >
                 wantUrgency(&criticalSocial, NEED_SOCIAL));
/* panic overrides: a panicking need is maximal and suppresses higher tiers */
TEST_ASSERT_EQUAL_INT(15, wantUrgency(&panicking, NEED_ENERGY));
TEST_ASSERT_TRUE(wantUrgency(&panicking, NEED_SURVIVAL) ==
                 wantUrgency(&panicking, NEED_SURVIVAL)); /* deterministic */
/* suppression scales a non-panicking need down by 0.75 */
TEST_ASSERT_TRUE(wantUrgency(&panicAndHigh, NEED_FULFILMENT) * 4
                 <= wantUrgency(&panicAndHigh, NEED_FULFILMENT) * 4 + 1);
/* decay never goes negative, clamps dt, and accelerates near empty */
```

Also pin: `needDecay` with `dt = 100.0f` advances by at most `NEED_MAX_DT`; a value of 0.05f stays >= 0.0f after a long decay; `needPanicDecay` holds panic while the value is below threshold and decays it once above; NULL inputs are no-ops. Then the agent registry twins: create up to `AGENT_MAX`, the next returns `AGENT_INVALID`, `agentGet` of a destroyed handle returns NULL, slot reuse never aliases a live agent, `agentsDecay` advances only live agents, and `agentRoleProfile(ROLE_COUNT)` is NULL.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `ninja -C build && ./build/tests/test_pure 2>&1 | tail -3`
Expected: FAIL — `src/agents/need.h` does not exist; the build stops on the missing include.

- [ ] **Step 3: Implement `need.c`**

Band mapping (the one decision the signature does not determine): `deficit = 1.0f - value[n]` clamped to `[0, 1]`; `raw = needTierWeight(n) * deficit`; `band = (int)(raw * (NEED_BANDS - 1) + 0.5f)` clamped to `[0, NEED_BANDS - 1]`. Panic handling in `wantUrgency`: return `NEED_BANDS - 1` when `panic[n] > 0.0f`; otherwise multiply the band by `NEED_PANIC_SUPPRESS` and round. Constants: `NEED_COUNT` from the enum, `NEED_BANDS 16`, `NEED_MAX_DT 0.1f`, `NEED_PANIC_RAMP 1.0f`, `NEED_PANIC_DECAY 0.5f`, `NEED_PANIC_SUPPRESS 0.75f`. Copy the clamping idiom from `src/entities/entities.c` (`!(dt > 0)` NaN guard).

- [ ] **Step 4: Implement `agent.c`**

Flat `slots[AGENT_MAX]` + `alive[]` + `liveCount` + `nextId`, mirroring `EntityRegistry` in `src/entities/entities.c`. `agentCreate` initialises needs via `needSetInit`, zeroes skills/flaws, and stamps a monotonic `id` (the stable tie-break id — a reused slot gets a *new* id so stale handles are never confused with live ones). `agentsDecay` reads `agentRoleProfile(a->role)` once per agent and passes its multipliers down.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `ninja -C build && ./build/tests/test_pure 2>&1 | tail -2`
Expected: `NNN Tests 0 Failures 0 Ignored` followed by `OK`.

- [ ] **Step 6: Wire the build, set floors from measured coverage**

Add both sources to `ctol_sources` in root `meson.build` and to `android/app/jni/CMakeLists.txt`. Run the coverage gate, read the measured percentages for `src/agents/need.c` and `src/agents/agent.c`, and write exactly those into `meson/coverage-min.txt`.

- [ ] **Step 7: Verify and commit**

Run: `./meson/check.sh` then `./meson/matrix.sh valgrind`
Expected: both green. Then:

```bash
git add src/agents/need.h src/agents/need.c src/agents/agent.h src/agents/agent.c \
        tests/test_need.c tests/test_agent.c meson.build meson/coverage-min.txt \
        android/app/jni/CMakeLists.txt
git commit -m "feat(agents): need vector, want ladder with banded urgency and panic"
```

---

### Task 2: Jobs and the lifecycle machine

What an entity *does*. A job is a live instance bound to a target, and its lifecycle is the retargeted FSM — the table is the right tool here because the lifecycle is fixed and small, which is exactly what went wrong when the FSM was asked to be the brain.

**Files:**
- Create: `src/agents/job.h`, `src/agents/job.c`, `tests/test_job.c`
- Modify: `src/ai/fsm.h`, `src/ai/fsm.c` (retarget: this module becomes the per-job lifecycle; `tests/test_fsm.c` stays green against the new names)

**Interfaces:**
- Consumes: `EntityHandle` (from `src/entities/entities.h`), `NeedSet`/`wantUrgency` (Task 1) — jobs carry the needs they serve but do not compute urgency yet (Task 4 owns that).
- Produces:
  - `typedef enum JobKind { JOB_WANDER, JOB_USE_MACHINE, JOB_PAY, JOB_LOAD, JOB_COLLECT, JOB_TALK, JOB_PLAY_ARCADE, JOB_NAP, JOB_REPAIR, JOB_LEAVE, JOB_KIND_COUNT } JobKind;`
  - `typedef enum JobState { JOB_UNCLAIMED, JOB_CLAIMED, JOB_WALKING, JOB_INTERACTING, JOB_WAITING, JOB_DONE, JOB_FAILED, JOB_STATE_COUNT } JobState;`
  - `typedef enum JobEvent { JOB_START, JOB_ARRIVED, JOB_DENIED, JOB_PHASE_DONE, JOB_INTERRUPTED, JOB_TIMEOUT, JOB_EVENT_COUNT } JobEvent;`
  - `typedef enum JobAction { JOB_ACTION_NONE, JOB_ACTION_MOVE_TO, JOB_ACTION_BEGIN_ACTIVITY, JOB_ACTION_WAIT, JOB_ACTION_FINISH, JOB_ACTION_CLAIM } JobAction;`
  - `typedef struct JobResult { int state; int action; int targetX, targetZ; int step; int waitReason; float waitSeconds; } JobResult;`
  - `void jobInit(Job *j, int kind); void jobSetTarget(Job *j, int kind, int id); JobResult jobStep(Job *j, int event); const char *jobStateName(int); const char *jobEventName(int); const char *jobActionName(int); bool jobStateValid(int); bool jobEventValid(int);`
  - The lifecycle table (`JOB_TABLE[7][6]`, state × event → next state + action), carrying over the T21 table's shape: `JOB_UNCLAIMED + JOB_START → CLAIMED / CLAIM`; `CLAIMED + JOB_ARRIVED → WALKING / MOVE_TO`; `WALKING + JOB_ARRIVED → INTERACTING / BEGIN_ACTIVITY`; `INTERACTING + JOB_PHASE_DONE → WAITING / WAIT`; `INTERACTING + JOB_TIMEOUT → FAILED / FINISH`; `WAITING + JOB_PHASE_DONE → DONE / FINISH`; `DONE + JOB_START → CLAIMED / CLAIM` (revive, matching the old `DONE` terminal-with-revival rule); `FAILED + JOB_START → CLAIMED / CLAIM`; `* + JOB_INTERRUPTED → FAILED / FINISH`; `* + JOB_DENIED → FAILED / FINISH`. **Every other cell is a no-op returning `{currentState, JOB_ACTION_NONE, zeroed payload}`.** Illegal pairs are a no-op by design — same rule, same reasoning, as today's FSM.
  - `typedef struct Job { int kind; int state; int targetKind; int targetId; int path[JOB_PATH_MAX][2]; int pathLen; float timer; float duration; int served[NEED_COUNT]; int band; float claimExpiry; int claimer; } Job;` with `#define JOB_PATH_MAX 64` and `band` initialised to `-1` (dirty).
  - `#define JOB_INVALID (-1)`

- [ ] **Step 1: Write the failing lifecycle tests**

Walk **every** cell of the 7×6 table explicitly — this is the exhaustive oracle the retargeted module exists to provide:

```c
/* every legal cell pinned, including the revival from DONE/FAILED */
TEST_ASSERT_EQUAL_INT(JOB_CLAIMED, jobStep(&j, JOB_START).state);
TEST_ASSERT_EQUAL_INT(JOB_ACTION_CLAIM, jobStep(&j, JOB_START).action);
/* an illegal pair is a no-op, distinguishable from a real transition */
JobResult r = jobStep(&walking, JOB_DENIED);
TEST_ASSERT_EQUAL_INT(JOB_WALKING, r.state);
TEST_ASSERT_EQUAL_INT(JOB_ACTION_NONE, r.action);
/* every state survives INTERRUPTED as FAILED */
for (int s = 0; s < JOB_STATE_COUNT; s++) { ... }
/* out-of-range event or NULL is a no-op, never a crash */
```

Plus: `jobInit` sets `band = -1` and `pathLen = 0`; name helpers return `"?"` for unknown values; `jobSetTarget` records kind and id.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `ninja -C build && ./build/tests/test_pure 2>&1 | tail -3`
Expected: FAIL — `src/agents/job.h` does not exist.

- [ ] **Step 3: Retarget `src/ai/fsm.c` to the job lifecycle**

Rewrite the module's states/events/actions as the `JOB_*` enums above, keeping the proven mechanics verbatim: the static 2-D table, the illegal-pair no-op, the name helpers, the validity predicates. Delete the old `Fsm`/`FsmResult`/`fsmStart`/`fsmSetGoal` API — it existed to be an agent brain, which is precisely the conflation this framework removes. Update the header comment to state the module's scope: *a fixed 5-to-7 state lifecycle for one job, table-driven; never a planner.*

**Note:** the old names map onto the new ones (`FSM_STATE_MOVING_TO → JOB_WALKING`, `FSM_EVENT_PHASE_DONE → JOB_PHASE_DONE`, `FSM_ACTION_FINISH_GOAL → JOB_ACTION_FINISH`). Every other `src/ai/` and `src/sim/` caller of the old API is removed in Task 3 (the critters migrate) and Task 5 (the customer migrates); until then, the app links only if those call sites are already gone — **check with `grep -rn "fsmStep\|fsmStart\|fsmSetGoal" src/` and make this task's commit include any mechanical rename those callers need.**

- [ ] **Step 4: Run the job tests to verify they pass**

Run: `ninja -C build && ./build/tests/test_pure 2>&1 | tail -2`
Expected: `OK`, with `tests/test_fsm.c` updated to the new names and still fully green.

- [ ] **Step 5: Wire the build, set floors, verify, commit**

```bash
# add src/agents/job.c to meson.build ctol_sources + android CMakeLists
./meson/check.sh && ./meson/matrix.sh valgrind
git add src/agents/job.h src/agents/job.c src/ai/fsm.h src/ai/fsm.c \
        tests/test_job.c tests/test_fsm.c meson.build meson/coverage-min.txt \
        android/app/jni/CMakeLists.txt
git commit -m "feat(agents): job record and table-driven per-job lifecycle machine"
```

---

### Task 3: Boards, claims, and the switch decision

Where jobs wait, and the single place an agent changes its mind. This task also converts the demo critters from hardcoded patrol loops to jobs on a personal board — the first visible proof that the framework drives real agents.

**Files:**
- Create: `src/agents/board.h`, `src/agents/board.c`, `tests/test_board.c`
- Modify: `src/scenes/level_scene.c` (critters → agents), `tests/test_agents.c` (new twin for the switch decision — note the file name collides with nothing today)
- Modify: `meson.build`, `android/app/jni/CMakeLists.txt`, `meson/coverage-min.txt`

**Interfaces:**
- Consumes: Task 1 (`Agent`, `agentsDecay`), Task 2 (`Job`, `jobStep`), plus existing `entityPathTo` (`src/ai/nav.h`) and `entityPlace`/`entityGet` (`src/entities/entities.h`).
- Produces:
  - `typedef enum JobPriority { JOB_PRIO_ERRAND, JOB_PRIO_ASSIGNED, JOB_PRIO_FILL, JOB_PRIO_COUNT } JobPriority;`
  - `#define BOARD_MAX 128`
  - `typedef struct JobBoard { Job slots[BOARD_MAX]; bool alive[BOARD_MAX]; int liveCount; bool shared; } JobBoard;`
  - `void boardInit(JobBoard *b, bool shared);`
  - `int boardEnqueue(JobBoard *b, const Job *j, int priority);` — returns a job handle (slot index) or `JOB_INVALID` when full. A full board is a bounded drop with a diagnostic on stderr, never an overflow.
  - `bool boardClaim(JobBoard *b, int handle, int agentId);` — refused unless the job is unclaimed *and* unexpired; stamps `claimer` and `claimExpiry = BOARD_RESERVE_SECS (5.0f)`. **The claim is the exclusivity guarantee: verify no other code path writes `claimer`.**
  - `void boardRelease(JobBoard *b, int handle);` — clears `claimer` and the expiry, keeps the job enqueued. This is the cancel-releases-resources rule from the spec.
  - `int boardBestFor(JobBoard *b, const int *skip, int agentId);` — the highest-band unclaimed, unexpired job for this agent, ties broken by lowest slot index (deterministic). `band` is read from the job; recompute is Task 4's job.
  - `int boardExpire(JobBoard *b, float now);` — releases expired claims and returns the count.
  - `#define AGENT_PREEMPT_MARGIN 2` (bands), `AGENT_MIN_DWELL 1.5f` (seconds).
  - `int agentShouldSwitch(const Agent *a, int engagedBand, int challengerBand, float dwellLeft);` — returns 1 to switch, 0 to stay. The entire rule: `challenger == JOB_INVALID || engagedBand == JOB_INVALID` → 1 (idle takes the best); otherwise `dwellLeft > 0.0f` → 0; otherwise `challengerBand >= engagedBand + AGENT_PREEMPT_MARGIN` → 1, else 0.
  - `int agentsUpdate(AgentRegistry *r, AgentBoardSet *boards, const Voxmap *map, float dt, int *outDecisions);` — the tick's decision half for this phase: decay, then for each live agent the idle-or-challenger test above, then `entityPathTo` on a switch. Advances `AgentStats`. Tasks 4–6 extend this function; Task 3's version only moves agents to jobs and executes the lifecycle.

- [ ] **Step 1: Write the failing board and switch tests**

```c
/* exclusivity, the property the whole framework leans on */
TEST_ASSERT_TRUE(boardClaim(&b, jh, 7));
TEST_ASSERT_FALSE(boardClaim(&b, jh, 8));   /* refused; job stays with 7 */
/* reservation expiry releases the claim so nobody sits on a job forever */
boardExpire(&b, now + BOARD_RESERVE_SECS + 0.1f);
TEST_ASSERT_TRUE(boardClaim(&b, jh, 8));   /* now available */
/* a full board drops with a diagnostic, never overflows */
for (i = 0; i < BOARD_MAX; i++) boardEnqueue(&b, &job, 0);
TEST_ASSERT_EQUAL_INT(JOB_INVALID, boardEnqueue(&b, &job, 0));

/* THE anti-oscillation property: near-equal bands never flip the agent */
TEST_ASSERT_EQUAL_INT(0, agentShouldSwitch(&a, engagedBand = 9, 10, dwellLeft = 0.0f));
/* a decisive gap does, once the dwell has expired */
TEST_ASSERT_EQUAL_INT(1, agentShouldSwitch(&a, 9, 11, 0.0f));
/* nothing preempts during the dwell, however urgent */
TEST_ASSERT_EQUAL_INT(0, agentShouldSwitch(&a, 0, 15, AGENT_MIN_DWELL));
/* an idle agent always takes the best available */
TEST_ASSERT_EQUAL_INT(1, agentShouldSwitch(&a, JOB_INVALID, 0, 0.0f));
```

Plus the oscillation property test the Review Focus demands: over 200 fake-dt ticks with two jobs whose bands differ by 1, the engaged band never changes; with a difference of `AGENT_PREEMPT_MARGIN` it changes exactly once and then holds. Plus `boardRelease` on a claimed job makes it claimable again.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `ninja -C build && ./build/tests/test_pure 2>&1 | tail -3`
Expected: FAIL — `src/agents/board.h` does not exist.

- [ ] **Step 3: Implement `board.c`**

Straightforward flat arrays; the only algorithm worth stating is `boardBestFor`: scan all `BOARD_MAX` slots, skip `!alive`, skip claimed and expired, take the strictly-highest `band` (strict `>` keeps the lowest slot index on a tie — the determinism rule), return its index or `JOB_INVALID`.

- [ ] **Step 4: Implement `agentsUpdate`'s decision half**

Per live agent: `agentsDecay` (Task 1) → find the best job on its personal board → `agentShouldSwitch` → on a switch, `boardClaim`, `jobStep(j, JOB_START)`, and for a `JOB_ACTION_MOVE_TO` result call `entityPathTo(entity, targetX, targetZ)`. Count decisions into `outDecisions` and `AgentStats`. Clamp dt to `AGENT_MAX_DT`.

- [ ] **Step 5: Migrate the demo critters to agents on boards**

In `src/scenes/level_scene.c`: keep `DemoWalker`'s three destination tables (`DEMO_DESTS_A/B/C`), their speeds (`3.0f`, `2.5f`, `3.5f`) and easings — they become a `JOB_WANDER` activity row (Task 4 formalises the row; for now the level enqueues `JOB_WANDER` jobs with the existing per-critter path data). Each critter becomes an `Agent` whose personal board holds the next leg. `levelUpdateWalkers`' hardcoded leg logic is deleted; `entitiesUpdate` still runs once for the shared registry. The visual result must be identical to today's critters pacing their three routes — capture before/after Xvfb frames and confirm the routes match.

- [ ] **Step 6: Run tests, verify, commit**

Run: `./build/tests/test_pure` (green), then `./meson/check.sh && ./meson/matrix.sh valgrind`, then Xvfb evidence: the critters pace the same three routes under agent control, with the why-this-job log lines showing the band and margin.

```bash
git add src/agents/board.h src/agents/board.c tests/test_board.c tests/test_agents.c \
        src/scenes/level_scene.c meson.build meson/coverage-min.txt \
        android/app/jni/CMakeLists.txt
git commit -m "feat(agents): job boards with exclusive expiring claims; critters run on agents"
```

---

### Task 4: The activity catalogue and job urgency

The heart of the emergent ordering. This is where "an activity is never primary or secondary" becomes true in code, and where job bands stop being hand-set numbers and start being computed from the needs a job would serve.

**Files:**
- Create: `src/agents/activity.h`, `src/agents/activity.c`, `tests/test_activity.c`
- Modify: `src/scenes/level_scene.c` (fill activities + band-tint overlay), `meson.build`, `android/app/jni/CMakeLists.txt`, `meson/coverage-min.txt`

**Interfaces:**
- Consumes: Task 1 (`wantUrgency`, `NeedSet`), Task 2 (`Job`, `served[]`), Task 3 (`boardBestFor`, `agentShouldSwitch`).
- Produces:
  - `#define ACTIVITY_MAX 32`
  - `typedef enum ActivityFlag { ACT_IN_PLACE = 0, ACT_NEEDS_PLACE = 1 } ActivityFlag;`
  - `typedef struct Activity { int id; const char *name; int flags; float duration; float cooldown; float serves[NEED_COUNT]; /* -1 = not served, else 0..1 strength */ int skillGate; float skillMin; } Activity;`
  - `const Activity *activitiesGet(int id);` — NULL for unknown. **This NULL is the Review Focus #5 guard**: every caller must treat it as a reported error, never a silent no-op.
  - `int activitiesCount(void); const Activity *activitiesAt(int index);`
  - `int jobUrgency(const Agent *a, const Job *j);` — resolves the job's activity by `j->kind`, walks `serves[]`, takes `wantUrgency(a->needs, n) * serves[n]` for each served need, and returns the **maximum** (a job is as urgent as its most pressing served need). Unknown activity → returns `-1` and the caller logs an error and fails the job.
  - `int agentBestFill(const Agent *a, const JobBoard *b, float now);` — score every activity whose cooldown has elapsed and whose place requirement is satisfiable, return the best job to enqueue. Used by `agentsUpdate` when the agent is idle and its board has nothing above the margin.
  - The seed catalogue, at least these rows: `ACT_WANDER` (in place, 3s, serves FULFILMENT 0.2), `ACT_TALK` (needs another agent within `TALK_RADIUS 3` tiles, 5s, serves SOCIAL 0.9), `ACT_PLAY_ARCADE` (needs an arcade fixture tile, 20s, serves FULFILMENT 0.9), `ACT_NAP` (in place, 15s, serves ENERGY 0.9), `ACT_TIDY` (needs a machine, 4s, serves ESTEEM 0.6, `skillGate SKILL_REPAIR skillMin 0`).
  - `void activityMarkDone(int id, float now);` — cooldown bookkeeping, per agent.

- [ ] **Step 1: Write the failing catalogue and priority tests**

```c
/* THE priority-inversion property (Review Focus #4): a critical tier-1
 * errand continuation outranks a severe tier-4 fill. */
Agent a; needSetInit(&a.needs);
a.needs.value[NEED_ENERGY] = 0.02f;     /* critical */
a.needs.value[NEED_FULFILMENT] = 0.01f; /* severe, but lower tier */
Job fill = wanderJob;                   /* serves FULFILMENT 0.2 */
Job errandJob = napJob;                 /* serves ENERGY 0.9 */
TEST_ASSERT_TRUE(jobUrgency(&a, &errandJob) > jobUrgency(&a, &fill));
/* an unknown activity is an error, not a silent zero */
TEST_ASSERT_EQUAL_INT(-1, jobUrgency(&a, &bogusJob));
TEST_ASSERT_NULL(activitiesGet(9999));
/* the same activity is reachable from both producers (the arcade rule) */
TEST_ASSERT_EQUAL_PTR(activitiesGet(JOB_PLAY_ARCADE),
                      activitiesGet(JOB_PLAY_ARCADE));
```

Plus: `jobUrgency` returns the max over served needs (not the sum) — a job serving two weak needs loses to one serving a single critical need; an in-place activity never needs a fixture; `ACT_TALK` is unavailable with no other agent in range and available with one; cooldowns suppress a re-pick until they expire; a full catalogue is bounded at `ACTIVITY_MAX` and `activitiesAt` is NULL out of range.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `ninja -C build && ./build/tests/test_pure 2>&1 | tail -3`
Expected: FAIL — `src/agents/activity.h` does not exist.

- [ ] **Step 3: Implement `activity.c`**

The catalogue is a `static const Activity ACTIVITIES[]` table with one comment per row naming the need it serves — the spec's authorability requirement, made literal. `jobUrgency` is the only algorithm: resolve, guard NULL with `-1`, take the max of `wantUrgency × serves[n]` rounded to an integer band. `agentBestFill` filters by cooldown and place requirement, then returns the highest-urgency candidate.

- [ ] **Step 4: Wire job bands into the tick and add fill activities**

In `agentsUpdate`: when idle (or when nothing clears the margin), call `agentBestFill`, enqueue the chosen fill job on the agent's personal board with `JOB_PRIO_FILL`, and claim it. Refresh each live job's `band` field with `jobUrgency` — this replaces the hand-set bands Task 3's tests used, so update those tests to set a band through the real path or mark the job's band explicitly.

- [ ] **Step 5: Add the band-tint overlay and the fill-behaviour evidence**

Tint agent billboards by their top `wantUrgency` when the light-debug overlay is on (`st->lightDebug`), so a crowd going restless or panicking is visible as colour. Xvfb: critters wander, then visibly switch to fill activities when idle — capture a sequence showing an idle critter starting a fill activity, with the debug log naming the activity and its band.

- [ ] **Step 6: Run tests, verify, commit**

Run: `./build/tests/test_pure`, `./meson/check.sh`, `./meson/matrix.sh valgrind`.

```bash
git add src/agents/activity.h src/agents/activity.c tests/test_activity.c \
        src/scenes/level_scene.c meson.build meson/coverage-min.txt \
        android/app/jni/CMakeLists.txt
git commit -m "feat(agents): activity catalogue with urgency computed from served needs"
```

---

### Task 5: Errands, the customer migration, and abandonment

What an entity is *trying to do*. This task makes the laundromat real under the framework: the customer's hardcoded flow becomes an errand template, blocked time produces the satisfaction loss, and a customer whose satisfaction falls past the threshold walks out — the business loop's first half.

**Files:**
- Create: `src/agents/errand.h`, `src/agents/errand.c`, `tests/test_errand.c`
- Modify: `src/sim/brain.c`, `src/sim/brain.h` (customer driver rewritten onto errands; `BrainPhase` deleted), `src/scenes/level_scene.c`, `tests/test_brain.c`, `meson.build`, `android/app/jni/CMakeLists.txt`, `meson/coverage-min.txt`

**Interfaces:**
- Consumes: Tasks 1–4 (`Agent`, `Job`, `JobBoard`, activities, `agentsUpdate`), plus existing `machineClaim`/`machineStartRun`/`machineRelease`/`machinesUpdate` (`src/sim/machines.h`) and `garments.h` — a claimed machine *is* a claimed thing, so jobs target machines by `MachineHandle` rather than duplicating the claim model.
- Produces:
  - `typedef enum ErrandStepKind { STEP_JOB, STEP_GATE } ErrandStepKind;`
  - `typedef enum ErrandState { ERRAND_ACTIVE, ERRAND_DONE, ERRAND_FAILED, ERRAND_ABANDONED, ERRAND_STATE_COUNT } ErrandState;`
  - `typedef enum ErrandStepKind... STEP_GATE_WAIT_MACHINE` (resume on the machine's PHASE_DONE wake), `STEP_GATE_WALK_TO` (resume on arrival).
  - `typedef struct ErrandStep { int kind; int jobKind; int gate; } ErrandStep;`
  - `typedef struct ErrandTemplate { int id; const char *name; int role; int serves[NEED_COUNT]; ErrandStep steps[ERRAND_MAX_STEPS]; int stepCount; int priorityFloor; bool worldSeeded; } ErrandTemplate;` (`ERRAND_MAX_STEPS 8`)
  - `typedef struct Errand { int tmplId; int step; int state; int targetKind; int targetId; int failureCount; float startedAt; int garbage; /* garment index */ } Errand;`
  - `#define ERRAND_MAX 4` per agent — a small active list, so the queue of errands is bounded and visible.
  - `int errandAdopt(Agent *a, const ErrandTemplate *t, int now);` — refuses if `a->role != t->role`; **world-seeded templates are adopted only at their declared `priorityFloor`, never forced** (recorded in the errand so the ladder arbitrates it).
  - `int errandAdoptByNeed(Agent *a, float now);` — scans the templates registered for the agent's role, adopts the one whose served-need urgency is highest and that is not already active. This is the need-driven producer.
  - `void errandAdvance(Errand *e, Agent *a, float now, JobBoard *b);` — a finished step enqueues the next; `STEP_GATE` steps wait; a failed job re-selects (and blacklists the failed machine for this errand instance only); no candidate left → `ERRAND_FAILED`.
  - `const ErrandTemplate *errandsTemplate(int id);` — NULL for unknown, with a diagnostic (Review Focus #5).
  - `ERRAND_LEAVE_THRESHOLD 0.25f` — satisfaction below this abandons the errand and walks the agent to the door.

- [ ] **Step 1: Write the failing errand tests**

```c
/* need-driven adoption: a dirty-garment customer adopts the wash errand */
TEST_ASSERT_EQUAL_INT(ERR_LAUNDRY_CLEAN, errandAdoptByNeed(&customer, now));
/* the world-seeded errand is adopted at its floor, not forced above it */
TEST_ASSERT_TRUE(seeded->priorityFloor >= JOB_PRIO_ASSIGNED);

/* every gate resumes on exactly the right wake */
/* a preempted USE_MACHINE releases its machine claim (Review Focus #2) */
TEST_ASSERT_EQUAL_INT(MACHINE_STATE_FREE, machinesStateOf(&machines, h));
/* abandoning the errand releases the claim too — no deadlock */
```

Plus the whole story pinned as one integration scenario in the pure suite, asserting **invariants and replayability** rather than a brittle interleaving: with a fixed dt, both customers eventually leave clean; no customer is claimed-and-abandoned; `satisfaction` never falls below 0 for more than `ERRAND_LEAVE_THRESHOLD_WINDOW` seconds without the customer leaving; two runs with identical inputs produce identical event traces (`memcmp`). Also: a customer whose errand is blocked decays **satisfaction** (tier 1), not survival — the tier assignment the spec fixes.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `ninja -C build && ./build/tests/test_pure 2>&1 | tail -3`
Expected: FAIL — `src/agents/errand.h` does not exist.

- [ ] **Step 3: Implement `errand.c`**

Templates are a `static const ErrandTemplate ERRAND_TEMPLATES[]` table with a per-row comment; `ERR_LAUNDRY_CLEAN` encodes the existing customer flow as steps (select+claim a washer → pay → load → gate on the machine's phase → collect → gate → repeat for the dryer → leave). Step advance is a switch on `step.kind`; job failures increment `failureCount` and re-select, blacklisting the failed `MachineHandle` in a bounded per-errand list.

- [ ] **Step 4: Rewrite the customer driver onto errands**

Replace `brain.c`'s `BrainPhase` switch with `errandAdvance` over an agent holding the errand. The existing heuristic selection (`src/sim/selection.c` — `closest`, `spread_k`, `cheapest`) stays as-is and is called from the errand's select step, so the demo's visible divergence between policies is preserved. The T23 integration scenarios in `tests/test_brain.c` migrate to assert the invariant form. Add satisfaction decay to `agentsDecay` when an agent's errand is blocked, and the abandon-and-leave path when it crosses `ERRAND_LEAVE_THRESHOLD`.

- [ ] **Step 5: Evidence — the laundromat under the framework**

Xvfb capture sequence telling the story: customers arrive and adopt the wash errand (log line naming the template and the urgency that chose it), queue behind each other, fill time while a machine runs (log naming the fill activity), get interrupted by the scripted break and re-plan, and eventually leave clean. Emulator frame. APK published per the standing rule (rebuild from HEAD first — appi_k rejects byte-identical uploads).

- [ ] **Step 6: Run tests, verify, commit**

Run: `./build/tests/test_pure` (all green), `./meson/check.sh`, `./meson/matrix.sh valgrind`.

```bash
git add src/agents/errand.h src/agents/errand.c tests/test_errand.c \
        src/sim/brain.c src/sim/brain.h tests/test_brain.c src/scenes/level_scene.c \
        meson.build meson/coverage-min.txt android/app/jni/CMakeLists.txt
git commit -m "feat(agents): errand templates with need-driven adoption and abandonment"
```

---

### Task 6: Shared board staffing, competencies, and world-seeded errands

The staff half. Proves the shared board's claim discipline under real contention, brings competencies online as modifiers (not gates), and makes a world event seed an errand.

**Files:**
- Modify: `src/agents/board.c`, `src/agents/agent.c` (competency application, staff role profile already present), `src/agents/errand.c` (world-seeded adoption path)
- Create: `tests/test_staff.c` (twin for the staffing rules; pairs with no new source, so it must be added to the suite explicitly)
- Modify: `src/scenes/level_scene.c`, `meson.build`, `android/app/jni/CMakeLists.txt`, `meson/coverage-min.txt`

**Interfaces:**
- Consumes: Tasks 1–5, `EntityHandle`, `Entity` (`src/entities/entities.h`).
- Produces:
  - `int agentsClaimFromShared(AgentRegistry *r, JobBoard *shared, float now);` — one pass over one shared board; each idle staff agent claims the best job it is *competent enough* for (never disqualified — see below) and increments `AgentStats.agentDecisions`.
  - `float jobDurationFor(const Agent *a, const Job *j);` — `base duration / (1.0f + skills[a->...])`, so a fast worker is quicker; `flaws[SKILL_SPEED]` can multiply the duration by `FLAW_PENALTY 2.0f` (a deficiency makes a job *fail*, per the spec).
  - `bool agentCanAttempt(const Agent *a, const Job *j);` — **always true in v1**: low competency modifies duration and quality, it never disqualifies, so a bad hire cannot deadlock the sim. The function exists so a later game can add hard gates without touching callers.
  - `int errandSeedWorld(SimWorldLike *w, int templateId, float now);` — the world-seeded producer: offers an errand at its declared `priorityFloor` to every agent of the matching role; adoption still goes through the ladder.
  - `ERRAND_REPAIR` template — seeded when a machine enters a faulted state; `JOB_REPAIR` targets that machine, `skillGate SKILL_REPAIR skillMin 0` (a modify, not a gate).

- [ ] **Step 1: Write the failing staffing tests**

```c
/* one shared board, contention resolved by claim exclusivity */
TEST_ASSERT_TRUE(boardClaim(&shared, jh, staffIdA));
TEST_ASSERT_FALSE(agentsClaimFromShared(&reg, &shared, now)); /* nobody else gets it */
/* reservation expiry returns it to the pool (Review Focus #3: no hoarding) */
agentsClaimFromShared(&reg, &shared, now + BOARD_RESERVE_SECS + 0.1f);
TEST_ASSERT_EQUAL_INT(2, staffWithJobs(&reg));
/* competencies modify, never gate (the spec's anti-deadlock rule) */
TEST_ASSERT_TRUE(agentCanAttempt(&clumsy, &repairJob));
TEST_ASSERT_TRUE(jobDurationFor(&slow, &job) > jobDurationFor(&fast, &job));
/* a deficiency can fail a job outright */
TEST_ASSERT_TRUE(jobDurationFor(&clumsySpeed, &job) >= jobDurationFor(&clumsy, &job));
/* a world-seeded errand is offered at its floor, not forced */
```

Plus the starvation property: over a long fake-dt run, every staff agent's `liveCount` of claimed jobs returns to 0 when the board empties, and no staff agent holds more than one job.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `ninja -C build && ./build/tests/test_pure 2>&1 | tail -3`
Expected: FAIL — `agentsClaimFromShared` is undefined.

- [ ] **Step 3: Implement the staffing rules**

`agentsClaimFromShared` is a single pass: for each idle staff agent, `boardBestFor` filtered by `agentCanAttempt`, then `boardClaim`. `jobDurationFor` applies the skill divisor and flaw penalty. Both are small; the tests determine them.

- [ ] **Step 4: Wire the repairer into the level**

Script a machine fault in `level_scene.c` (the existing T23 scripted break becomes a *fault*, seedable), spawn a staff agent with a repair competency and a distinct tint, and Xvfb-evidence: the faulted machine darkens, the repairer is claimed onto it by the shared board, the repair completes, and a second staff agent visibly does *not* double-claim.

- [ ] **Step 5: Run tests, verify, commit**

Run: `./build/tests/test_pure`, `./meson/check.sh`, `./meson/matrix.sh valgrind`.

```bash
git add src/agents/board.c src/agents/agent.c src/agents/errand.c tests/test_staff.c \
        src/scenes/level_scene.c meson.build meson/coverage-min.txt \
        android/app/jni/CMakeLists.txt
git commit -m "feat(agents): shared board staffing with competencies and world-seeded repair"
```

---

### Task 7: The business ledger and the framework report

Closes the loop the user's original bar describes: satisfaction converts to money, reputation, and return behaviour — and an annoyed customer takes their business elsewhere.

**Files:**
- Create: `src/agents/ledger.h`, `src/agents/ledger.c`, `tests/test_ledger.c`
- Modify: `src/agents/errand.c` (departure hook), `src/scenes/level_scene.c` (reputation readout), `docs/superpowers/plans/2026-10-09-isomata-phase2.md` (close out the roadmap), `meson.build`, `android/app/jni/CMakeLists.txt`, `meson/coverage-min.txt`

**Interfaces:**
- Consumes: Tasks 1–6, `EntityHandle`, the entity's `money` field (`src/entities/entities.h`, added in T23).
- Produces:
  - `typedef struct Ledger { int revenue; int costs; int reputation; int servedCustomers; int defections; } Ledger;`
  - `void ledgerDeparture(Ledger *l, int satisfaction, int spent);` — revenue `+= spent`, reputation `+= satisfaction >= DEPART_HAPPY (0.5f) ? 1 : (satisfaction < DEPART_ANGRY (0.25f) ? -2 : 0)`, `servedCustomers++`.
  - `bool ledgerWouldReturn(const Ledger *l, float lastSatisfaction);` — false when reputation is below `LEDGER_REPUTATION_FLOOR` or the last satisfaction was below the angry threshold: **defection**. The departing agent's memory is stored on its (soon-destroyed) record and reported in the log.
  - `void ledgerReport(const Ledger *l);` — the summary the final report and the in-game overlay both read.

- [ ] **Step 1: Write the failing ledger tests**

```c
/* a happy customer pays and lifts reputation */
ledgerDeparture(&l, 0.9f, 7); TEST_ASSERT_EQUAL_INT(7, l.revenue);
/* an annoyed one costs reputation and takes their business elsewhere */
ledgerDeparture(&l, 0.1f, 7); TEST_ASSERT_TRUE(l.reputation < 0);
TEST_ASSERT_FALSE(ledgerWouldReturn(&l, 0.1f));
```

Plus boundary tests at exactly `0.5f` and `0.25f`, and the balance invariant `revenue - costs == sum of spends`.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `ninja -C build && ./build/tests/test_pure 2>&1 | tail -3`
Expected: FAIL — `src/agents/ledger.h` does not exist.

- [ ] **Step 3: Implement `ledger.c` and wire the departure hook**

Call `ledgerDeparture` where a departing customer currently despawns (the abandon-and-leave path from Task 5), and log the defection case with the satisfaction that caused it.

- [ ] **Step 4: Measure performance and report it**

Build a headless benchmark (a `.tmp_files/` harness or a test-only entry that steps a 200-agent world for 6000 fixed-dt ticks) and report: agent decisions/second, A\* invocations/second, wall-clock per `simUpdate` at 200 and 300 agents, on the low-end profile. **Report the numbers whether or not they clear 60fps** — this is a measured note, not a gate, and a short result is a finding to state, not a reason to tune silently.

- [ ] **Step 5: Final whole-branch evidence and the framework report**

Xvfb and emulator captures of the complete story: customers adopting errands, filling blocked time, contending for machines, being interrupted, abandoning, leaving clean-dry or defecting; staff claiming repairs; the debug overlay showing needs, bands and the board. Write `docs/superpowers/plans/2026-10-11-agent-framework-report.md` covering: what was built, the measured perf numbers, the emergent behaviours actually observed (with log excerpts), everything the spec deferred that remains deferred, and an honest list of anything the design did not achieve. Publish the APK.

- [ ] **Step 6: Run everything, verify, commit**

Run: `./build/tests/test_pure`, `./meson/check.sh`, `./meson/matrix.sh valgrind`, `./meson/matrix.sh asan`.

```bash
git add src/agents/ledger.h src/agents/ledger.c tests/test_ledger.c \
        src/agents/errand.c src/scenes/level_scene.c \
        docs/superpowers/plans/2026-10-11-agent-framework-report.md \
        meson.build meson/coverage-min.txt android/app/jni/CMakeLists.txt
git commit -m "feat(agents): departure ledger — revenue, reputation and defection"
```