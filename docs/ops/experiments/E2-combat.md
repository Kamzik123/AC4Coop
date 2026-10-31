# E2 — Combat: stage 2A experiment + stage 2B design

Status: design, 2026-10-29. Seat: COMBAT. Implements `PROGRAM-PLAN.md` §4 Phase 2,
gated by `GATES.md` M2. Wire extension on top of `ref/AC4Coop/src/netstate.h`
(NET_PROTO_VER=9 → 10).

Evidence tags: **[V]** = verified fact supplied with this dispatch or observed in the
files named inline. **[H]** = hypothesis, carries the experiment that settles it.
Anything not one of the two is in §7 UNVERIFIED and is not to be implemented against.

Engine constants used throughout, all [V]:

| name | value | meaning |
|---|---|---|
| `FN_DMG_FILTER` | `0x17106C0` | 0x68 B, `__thiscall(this, a2, a3)`; tests `*(a2+0x9C)&7==2`, then `(&0x1F8)==152`, then kind table `3,6,14,23,5,25,26`, then `sub_1A90920(*(*(this+1)+28))` |
| `FN_DMG_REACT` | `0x173E190` | 0x5E9 B, `__thiscall(this, event)` |
| `FN_GET_SERVICE` | `0x10EF8C0` | `GetService(entity)` class id `0x5EB03CEC` |
| `CLS_NPCHEALTH` | `0x57EEEEC7` | `CSrvNPCHealth`, vtable `0x02712F60` |
| `FN_SET_LIFE` | `0x19FB4C0` | `__int16* this`, value stored at byte `+0x5C`; refuses to raise an already-negative life; negative = death |
| `FN_ALIVE` | `0x133F550` | `entity != 0 && !(ai && (*(ai+0xB8)&0xC)==4) && !(svc && (*(svc+4)&0x1000))` |

`AC4BFSP.exe` has no DYNAMICBASE and no `.reloc` **[V, plan §0]**, so these absolute
VAs are valid on build `bc11ad79…` only. Every hook installs a build-hash check first
and refuses to arm on a mismatch.

---

## 1. Stage 2A experiment card — is `sub_17106C0` the damage filter?

One session, one machine, no peer. Scene: a fight arena with one guard (use
`ops/tools/scene_fight.py` arena, 1 guard, player unarmed-then-armed).

### 1.1 The hypothesis under test

> H2A: `sub_17106C0` is entered exactly once per incoming damage event on a guard,
> after the hit is registered and before the health write, and its return value
> gates whether the damage is applied.

H2A has three separable clauses. The card proves or kills each one independently,
because "it fired" and "it gated" are different claims and only the second makes it
usable as a seam.

### 1.2 Instrument: four simultaneous execution breakpoints, no code patching

Primary instrument is a hardware execution breakpoint driven from inside the ASI, so
nothing in `.text` is modified during the observational phase. 4 debug registers =
4 functions observed in one punch:

| slot | VA | what it answers |
|---|---|---|
| DR0 | `0x17106C0` | does the filter fire, with what args, in what order |
| DR1 | `0x173E190` | does the reaction handler fire on the same hit (E2.2) |
| DR2 | `0x19FB4C0` | **ground truth**: what life value the engine actually wrote |
| DR3 | `0x133F550` | aliveness of the pair at the moment of the hit |

DR2 is what makes the card decisive: a punch that fires DR0 but never fires DR2 did
**zero** damage (a whiff / blocked hit), so a firing DR0 with no DR2 is not evidence
of an applied damage event.

```cpp
// ---- repl/exp/e2_1.cpp  (instrument build only; gated by cfg.experiments.e2_1) ----
static const uintptr_t VA[4] = { 0x17106C0u, 0x173E190u, 0x19FB4C0u, 0x133F550u };
static volatile LONG   g_hits[4];        // per-VA entry counters, monotonic
static volatile LONG   g_ordinal;        // global monotonic event ordinal (ordering proof)

static void ArmThread(HANDLE hThread, uintptr_t va0, uintptr_t va1,
                      uintptr_t va2, uintptr_t va3) {
    CONTEXT c{}; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    GetThreadContext(hThread, &c);
    c.Dr0 = va0; c.Dr1 = va1; c.Dr2 = va2; c.Dr3 = va3;
    // L0..L3 local, exact breakpoint, RW0=00 (execute) for all four.
    c.Dr7 = 0x00000555;   // L0 L1 L2 L3 enabled, LE/GE clear, RWn=00 LENn=00
    SetThreadContext(hThread, &c);
}

// VEH: first-chance. Write the CSV line, clear the hit bit for THIS slot, set TF,
// and on the single-step exception re-arm the slot and clear TF.
static LONG CALLBACK E2Veh(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP) { RearmAndClearTf(ep); return EXCEPTION_CONTINUE_EXECUTION; }
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT &&
        ep->ExceptionRecord->ExceptionCode != STATUS_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t ip = (uintptr_t)ep->ContextRecord->Eip;
    int slot = SlotFor(ip);                       // -1 if not one of ours
    if (slot < 0 || !IsGameThread(GetCurrentThreadId())) return EXCEPTION_CONTINUE_SEARCH;

    CONTEXT* c = ep->ContextRecord;
    const uint32_t esp = c->Esp, ecx = c->Ecx;
    const uint32_t a0 = *(uint32_t*)(esp + 0x04);  // first stack arg
    const uint32_t a1 = *(uint32_t*)(esp + 0x08);
    const uint32_t ret = *(uint32_t*)(esp + 0x00); // caller return address

    switch (slot) {
    case 0: LogFilter(c, ecx, a0, a1, ret); break;   // fields in 1.3
    case 1: LogReact (c, ecx, a0, ret);     break;
    case 2: LogSetLife(c, ecx, a0, a1, ret);break;   // ecx = __int16* this, a0 = value
    case 3: LogAlive (c, ecx, a0, ret);     break;
    }
    c->Dr6 = 0; c->Dr7 &= ~(1u << (slot * 2));       // disable this slot
    c->EFlags |= 0x100;                              // TF: single-step past the bp
    return EXCEPTION_CONTINUE_EXECUTION;
}
```

**Entry-point requirement (no disassembly needed):** the breakpoint fires on the
first byte of the function, so no prologue length has to be known and no bytes are
copied. This is why hardware breakpoints are the primary instrument and the
`0xCC` patch (§1.6) is the fallback.

**Thread scope:** arm on every thread of the process (Toolhelp
`TH32CS_SNAPTHREAD`, `th32OwnerProcessID == GetCurrentProcessId()`). Re-arm on
`DLL_THREAD_ATTACH` through the ASI's existing thread hook. Threads that fault on
`SetThreadContext` are dropped and logged; a session where the simulation thread is
dropped is INVALID (see 1.4).

**Return-value capture:** DR slots are per-thread and all four are used, so EAX at
return is captured by a second phase of the same hook: on the DR0 hit, record
`ret` (from `[esp]`), temporarily re-point DR0 at `ret`, and log EAX on that hit.
Two hits per damage event, correlated by `ordinal`. DR1/DR2/DR3 stay on their VAs.

### 1.3 Exact log fields

One file, `ops/evidence/E2.1/filter-17106c0.log`, one line per hit, CSV, header
line first:

```
t_rdtsc,ord,va,slot,tid,this,a0,a1,a2,ret_addr,ret_in_dmg_react,
w9C,z7,z1F8,k10,k20,k_hit,life_before,life_after,alive,ecx_svc
```

| column | expression | why |
|---|---|---|
| `t_rdtsc` | `__rdtsc()` | sub-ms ordering of the four slots |
| `ord` | `InterlockedIncrement(&g_ordinal)` | total order across slots and threads |
| `va`,`slot` | hit address, DR index | which function |
| `this` | `ECX` | the object the method runs on |
| `a0`,`a1` | `[esp+4]`,`[esp+8]` | `a2`,`a3` for slot 0 |
| `a2` | `[esp+0xC]` | third stack arg if present (logged even when unused) |
| `ret_addr` | `[esp]` | **the caller VA — this is a deliverable of the experiment, not an assumption** |
| `ret_in_dmg_react` | `0x173E190 <= ret_addr < 0x173E779` | is the caller the reaction handler |
| `w9C` | `*(u32*)(a0+0x9C)` | the field the filter masks |
| `z7` | `w9C & 7` | must be 2 for the first test |
| `z1F8` | `w9C & 0x1F8` | must be `152` for the second |
| `k10`,`k20` | `*(u32*)(a1+0x10)`, `*(u32*)(a1+0x20)` | the two fields compared against the kind table |
| `k_hit` | `k10 in {3,6,14,23,5,25,26} \|\| k20 in {…}` | which of the two is the kind |
| `life_before` | read `*(i16*)(svc+0x5C)` at entry, `svc = FN_GET_SERVICE(target)` | cross-check against DR2 |
| `life_after` | same read at the DR2 hit | proves the write happened |
| `alive` | `FN_ALIVE(target) != 0` | pre/post death-band state |
| `ecx_svc` | `ECX` at slot 2 | the health object address (ties DR2 to the entity) |

Slot 0 additionally logs the two globals the function ends with:
`svc_arg = *(u32*)(*(u32*)(this+4)+28)` — the value handed to `sub_1A90920` [V
offset]. If `svc_arg` correlates 1:1 with `ecx_svc` from slot 2, `this+4` is the
service pointer and the filter is operating on the same object the health write
touches. That correlation is what upgrades "it fired" to "it is on the damage path".

### 1.4 Session script and PASS / FAIL

Script, single session, single machine, ~12 minutes:

| step | action | duration |
|---|---|---|
| S0 | idle in the arena, guard walking, no combat | 10 s |
| S1 | 10 single punches on the guard, ≥2 s apart, no kill (guard at full life) | 40 s |
| S2 | 1 punch that kills (continue until the guard dies), repeat on a fresh guard ×3 | 90 s |
| S3 | 5 punches that are blocked/whiffed (punch the air next to the guard) | 20 s |
| S4 | 3 gunshots (separate kind) if the scene kit has a firearm | 30 s |
| S5 | 1 air assassination | 20 s |
| S6 | idle again | 10 s |

Validity precondition: the file contains ≥1 line per slot **and** the DR0 lines and
DR2 lines interleave in a plausible pipeline order (`ord` DR0 < `ord` DR2 for a
hit that dealt damage). If slot 3 (`0x133F550`) never fires, the session is
INVALID: the predicate is called on a different path than assumed; stop and re-scope.

Verdicts, all machine-decidable by `ops/tools/e2_1_verdict.py` (§1.5):

| claim | PASS | FAIL |
|---|---|---|
| **C1 — it fires on damage** | every S1 punch that also produced a DR2 `life_after < life_before` has ≥1 DR0 hit with `ord` between the hit's animation start and that DR2 hit; ≥10/10 | ≤0 paired DR2→DR0 ⇒ **not on the damage path** |
| **C2 — it is a filter, not a state poll** | 0 DR0 hits during S0 and S6 (10 s each) | >0 hits while idle ⇒ it is a predicate polled per frame; it is not a per-event filter |
| **C3 — the masks are the documented tests** | every DR0 line has `z7 == 2` and `z1F8 == 152` | any line with `z7 != 2` ⇒ `+0x9C` is not the damage-kind field, or `a2` is not the event |
| **C4 — the kind table** | `k_hit == 1` on every damaging hit, and `k_hit == 0` on whiffs (S3) | damaging hit with `k_hit == 0` ⇒ the table is not the accept list; log `k10`,`k20` raw and re-read |
| **C5 — the verdict is consumed** | force-return test §1.6 suppresses the DR2 write | damage still applies when forced 0 ⇒ it is bookkeeping, not a gate |

C1–C4 come out of one observational session. C5 needs the second (destructive) pass
in the same session, after S6, in a separate log file.

### 1.5 Machine verdict

```python
# ops/tools/e2_1_verdict.py  — reads filter-17106c0.log, prints PASS/FAIL per claim
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1])))
hits = [r for r in rows if r["slot"] == "0"]
setlife = [r for r in rows if r["slot"] == "2"]
react = [r for r in rows if r["slot"] == "1"]
damaging = [s for s in setlife if int(s["a0"]) < 0 or int(s["life_after"]) < int(s["life_before"])]
paired = sum(1 for s in damaging if any(int(h["ord"]) < int(s["ord"]) for h in hits))
def claim(name, ok): print(("PASS " if ok else "FAIL ") + name)
claim("C1 fires-on-damage", damaging and paired >= max(10, len(damaging)))
claim("C2 not-a-poll",     not any(h["phase"] in ("S0", "S6") for h in hits))
claim("C3 masks",          hits and all(h["z7"] == "2" and h["z1F8"] == "152" for h in hits))
claim("C4 kind-table",     hits and all(h["k_hit"] == "1" for h in hits if h["va"] == "0x17106C0"))
claim("E2.2 react-fires",  bool(react))
```

(`phase` is written by the scene script into the log as a marker line; the parser
slices on markers.)

### 1.6 C5 — the decisive test: force the filter to reject

Runs after the observational pass, separate log, guard re-spawned.

Precondition, self-checked at install with no IDA:

```cpp
// The callee pops its two stack args: the last 3 bytes are `C2 08 00`.
const uint8_t* fn = (const uint8_t*)0x17106C0;
if (fn[0x66] != 0xC2) { Log("E2.1 abort: epilogue is not ret imm16 at +0x66"); return; }
Log("E2.1 ret imm16 = 0x%04X", *(const uint16_t*)(fn + 0x67));   // expect 0x0008
```

Force-reject patch (6 bytes, restored after 5 hits or 3 s, whichever first):

```asm
; at 0x17106C0, saved to a buffer and restored verbatim on disarm
33 C0        xor  eax, eax
C2 08 00     ret  8
```

Then punch the guard 5 times.

| outcome | reading |
|---|---|
| DR2 (`0x19FB4C0`) does **not** fire, guard life unchanged, no hit reaction animation | the return value gates application ⇒ **`0x17106C0` is the damage filter. Stage 2A PASS.** Implement the §3 applier against its call site. |
| DR2 fires with the full damage value anyway | the return value is ignored by the caller ⇒ it is bookkeeping. **Stage 2A FAIL on this function.** Go to 1.7. |
| crash on the forced return | the caller consumes a side effect of the body, not the return. Log the return address (`ret_addr` from 1.3) and re-examine that caller. Not a filter seam. |

Restoration is a byte-for-byte `memcpy` of the 6 saved bytes under
`VirtualProtect(PAGE_EXECUTE_READWRITE)`, verified by re-reading and comparing, and
it runs on a `__try/__finally` so a crash mid-test still restores on the next launch
(the ASI checks a persisted "armed" flag at load and restores before anything else).

### 1.7 What to try next, in order

| order | target | instrument | what ends it |
|---|---|---|---|
| A1 | `0x173E190` (reaction handler) | same DR0 mechanism; log `this`, `event`, then walk `event` for a victim handle: `*(u32*)(event+0x00..0x100)` snapshotted, and `FN_GET_SERVICE(*(void**)(event+0x08))` on each dword that resolves as an entity | **E2.2**: if a dword in `event` resolves through `ResolveRef` to the same objectId as the guard whose `SetLife` fires in the same `ord` window, the event carries victim identity and can be recorded and replayed. If no dword resolves, record/replay of that event is dead and A2 is the only route. |
| A2 | health service | DR on `0x10EF8C0` + `0x19FB4C0`; log entity → service address map, `this+0x5C` before/after, `+4` flags word | establishes whether `CSrvNPCHealth` is per-entity and stable (needed by both §3 and §4) |
| A3 | the `sub_1A90920` callee of the filter | not instrumented — this is the *consumer* of the filter's verdict, not the applier | only read if forced-return crashes (case 3 above) |

Rule for the session: A1 runs **in the same session** as §1.2 (DR1 is already armed
on `0x173E190`), so E2.1 and E2.2 are one session, not two. A2's DR slots conflict
with A1's, so A2 is a second pass after the guard respawns.

### 1.8 Decision recorded at the end of 2A

Write one line into `ops/DECISIONS.md` (ORCH-only file; send a `DECIDE` message):

```
D-0xx E2.1: <17106C0 filter | 173E190 event | health-service only>
      applier = <VA>, arg shape = <__thiscall(this,event) | __cdecl(...) | SetLife(i16*,int)>,
      victim identity in event = <yes @offset | no>, evidence = ops/evidence/E2.1/
```

---

## 2. Event schema

### 2.1 What v10 adds

`netstate.h` line 116 is `NET_PROTO_VER = 9`. `net.cpp:268-269` dispatches received
datagrams by first-word magic: `NET_MAGIC` with `n == sizeof(PlayerState)` is a
player packet, `NET_ENTITY_MAGIC` with `n >= 20` is an entity packet. Everything
else is dropped **[V, net.cpp:265-269]**.

**v10 adds exactly one thing: a third datagram magic, `NET_COMBAT_MAGIC`, carrying
an unreliable, sequenced, idempotent combat channel.** No field of `PlayerState` or
`EntityState` is added, removed or reordered; those two structs stay byte-identical
to v9. `NET_PROTO_VER` becomes `10` so every existing `protoVer != NET_PROTO_VER`
check rejects a mismatched peer loudly at the first packet instead of mis-parsing, as
designed in plan §1.3.

The player-packet check requires an **exact** size match, so a combat packet can
never be misread as a player packet. The entity reader already rejects unknown magic
at `net.cpp:221` **[V]**.

### 2.2 EntityRef (wire)

```cpp
#pragma pack(push, 1)
// 16 bytes, u64 at offset 8 (aligned inside the struct, unaligned inside a record
// is fine on x86; the layout keeps memcpy of a whole array of these cheap).
struct EntityRef {
    uint32_t epoch;     // sender's world epoch; bumps on world/mission load
    uint16_t classTag;  // crc32(class name) & 0xFFFF
    uint16_t reserved;  // 0 — keeps objectId 8-aligned
    uint64_t objectId;  // HandleContainer::m_ObjectID  [D] sub_A200D0
};
```
`sizeof == 16`. Same three semantic fields as `PROGRAM-PLAN.md` §2.1; the `reserved`
u16 is the only addition and it exists so `objectId` never straddles a cache line
boundary inside a 48-byte record. `reserved` is written as 0 and ignored on read.

### 2.3 Datagram header

```cpp
enum CombatKind : uint8_t { CK_DAMAGE = 1, CK_DEATH = 2, CK_KILL = 3, CK_LIFESYNC = 4 };
enum CombatFlags : uint8_t {
    CF_RETRANSMIT = 1u << 0,   // copy of an earlier send, same (epoch,seq)
    CF_LETHAL     = 1u << 1,   // sender's own life reached <= 0 on this record
    CF_UNRESOLVED = 1u << 2,   // receiver-side annotation, never sent
};

struct CombatHeader {
    uint32_t magic;      // NET_COMBAT_MAGIC 0x44344341 'AC4D'
    uint16_t protoVer;   // NET_PROTO_VER == 10
    uint8_t  kind;       // CombatKind — uniform per datagram
    uint8_t  flags;      // CombatFlags, datagram-wide bits only
    uint32_t epoch;      // channel epoch; a change resets the receiver's dedup ring
    uint32_t seq;        // per-sender monotonic, first record's seq in this datagram
    uint32_t tick;       // per-sender monotonic
    uint32_t ackSeq;     // highest contiguous remote seq applied (piggyback ack)
    uint16_t count;      // records in this datagram, 1..MAX_COMBAT_RECORDS
};
```
`sizeof == 26`. Record `i` owns `seq = header.seq + i`; this is why a datagram is
homogeneous in `kind` (a resend of one lost record is one datagram, and mixing kinds
breaks the contiguous-`ackSeq` rule).

### 2.4 Records

```cpp
struct DamageRecord {           // 48 bytes
    EntityRef actor;            // who dealt it; epoch 0 + objectId 0 = unknown cause
    EntityRef target;           // who took it
    uint32_t  seq;              // idempotency key part 1 (scoped by sender + epoch)
    uint32_t  epoch;            // idempotency key part 2; per-record so a record is
                                // self-contained after a delayed resend
    uint32_t  tick;             // sender frame; ordering key
    uint16_t  amount;           // damage points, 0..0xFFFF; Life is __int16 so 0x7FFF
                                // is the practical clamp. NOT the resulting life.
    uint16_t  kind;             // engine damage kind — the value from the accepted
                                // table (3,6,14,23,5,25,26) when E2.1 confirms it
};

struct DeathRecord {            // 48 bytes; CK_DEATH and CK_KILL share the layout
    EntityRef target;           // the entity that died
    EntityRef killer;           // credit target; epoch 0/objectId 0 for CK_DEATH
    uint32_t  seq;
    uint32_t  epoch;
    uint32_t  tick;
    uint32_t  causeSeq;         // seq of the lethal DamageRecord, 0 if not the cause
};

struct LifeSyncRecord {         // 32 bytes — reconciliation, see 2.7
    EntityRef target;
    uint32_t  seq;
    uint32_t  epoch;
    uint32_t  tick;
    int16_t   life;             // absolute, as read from service+0x5C
    uint16_t  pad;              // 0
};
```

Sizes: header 26 + 48 = **74 B** for one damage event; `26 + 32 = 58 B` for a
lifesync; `26 + 10*48 = 506 B` worst-case datagram, under 512 and far under any
path MTU.

### 2.5 Idempotency key and dedup

Key = `(senderId, record.epoch, record.seq)`. `senderId` is the peer identity from
the session handshake (not on the wire; there is exactly one peer).

Receiver keeps, per `(senderId, epoch)`, a 512-bit seen-map and `base = seq & ~511`.
On `record.epoch != currentEpoch` the map is cleared and `currentEpoch` set — a
world load invalidates everything in flight by construction.

```cpp
bool SeenBefore(uint32_t epoch, uint32_t seq) {
    if (epoch != g_rxEpoch) { g_rxEpoch = epoch; g_seenBase = seq & ~511u; memset(g_seen,0,64); }
    if (seq + 512 < g_seenBase) return true;            // far behind: treat as stale
    if ((seq - g_seenBase) >= 512) { /* slide */ uint32_t k = ((seq - g_seenBase) / 512) * 512;
        /* bitmap shift-by-k with zero fill; k >= 512 ⇒ full clear */ }
    uint32_t b = (seq - g_seenBase);
    if (g_seen[b >> 6] & (1u << (b & 63))) return true;
    g_seen[b >> 6] |= 1u << (b & 63);
    return false;
}
```

### 2.6 Ordering semantics (the reason the schema is this small)

| pair | order matters? | why |
|---|---|---|
| Damage, Damage | **no** | damage is additive and monotone-decreasing on the target; `Apply(a); Apply(b) == Apply(b); Apply(a)` in effect, and only the sum is observable. Reordering is absorbed. |
| Damage, Death | **no** | death is a sticky tombstone keyed by `EntityRef`. A Damage that arrives after the Death is dropped (§3.5), whether or not its `tick` is larger. |
| Death, Death | **no** | tombstones are a grow-only set (plan §2.1). First insert wins; later inserts are no-ops. |
| LifeSync, Damage | **only for LifeSync** | a LifeSync with `tick` older than the newest Damage applied to that target is discarded; otherwise it wins. This is the single LWW field in the schema. |

`tick` is therefore carried for **diagnosis and LifeSync only**, not for correctness.
This is deliberate: none of the correctness rules needs a clock, so no clock skew
between two machines can produce a wrong outcome.

### 2.7 LifeSync and divergence

Machines diverge because each applies `amount` to its own current life. Two guards at
50 and 45 that both take 30 end at 20 and 15. Death still agrees (both ≤ 0 only if
both cross zero — see §6 risk R2). Reconciliation: the **causer** sends one
`CK_LIFESYNC` per damaged target at 5 Hz while that target is within 2 s of its last
local hit, carrying the causer's absolute `life`. The receiver calls `SetLife` toward
it. `SetLife` refuses to raise an already-negative life **[V]**, so a lif sync can
never resurrect; that is a second, engine-level tombstone guarantee.

### 2.8 Rate and budget at 20 Hz

Combat is event-driven; 20 Hz is the ceiling, not the rate. Worst case (a 20 Hz
stream of single-record damage datagrams):

| quantity | value |
|---|---|
| payload per damage event | 74 B |
| + IPv4/UDP | 28 B → **102 B on the wire** |
| 20 Hz single-record | 1 480 B/s payload, 2 040 B/s on the wire = 16.3 kbit/s |
| 10 kills in 2 s, batched | 2 datagrams of 5 records = 2 × 266 B |
| Max datagram | 10 records = 506 B payload (cap enforced by `MAX_COMBAT_RECORDS = 10`) |
| LifeSync, 20 damaged guards at 5 Hz | 20 × 58 × 5 = 5 800 B/s — **capped at 8 targets/datagram, 4 datagrams/s = 1 856 B/s** |

The LifeSync cap is a hard design constraint: without it the reconciliation channel
costs 3.4× the damage channel.

---

## 3. Apply path

### 3.1 Backend selection

The applier is written against one interface with three implementations, selected by
`cfg.combat.backend` after 2A, so 2B code lands before 2A closes:

```cpp
enum class ApplyResult : uint8_t {
    Applied, Deduped, StaleEpoch, Unresolved, ClassMismatch, AlreadyDead,
    Tombstoned, EngineRefused, Deferred,
};

struct ICombatBackend {
    // Resolve + validate + apply one hit. Must not write Life/flags directly.
    virtual ApplyResult Apply(EntityRef actor, EntityRef target,
                              uint16_t amount, uint16_t kind,
                              uint32_t tick) = 0;
    virtual bool        Kill(EntityRef target, EntityRef killer, uint32_t tick) = 0;
    virtual const char* Name() const = 0;
};
// B_FILTER  — discovered in E2.1 (case C5 PASS): call the filter's caller with a
//             synthesised event. Exact VA + event layout come from the experiment
//             log (ret_addr and the a3 object snapshot), never from this document.
// B_REACT   — sub_173E190 __thiscall(this, event), only if E2.2 found a victim id.
// B_SETLIFE — sub_19FB4C0(hp, life). Fallback, §4 semantics.
```

### 3.2 `ApplyRemoteDamage`

```cpp
ApplyResult ApplyRemoteDamage(const DamageRecord& r) {
    // ---- 1. idempotency, before any resolution work -------------------------
    if (SeenBefore(r.epoch, r.seq))                       return ApplyResult::Deduped;

    // ---- 2. epoch ----------------------------------------------------------
    if (r.epoch != g_worldEpoch)                          return ApplyResult::StaleEpoch;

    // ---- 3. tombstone before resolution ------------------------------------
    // A ref that cannot be resolved can still be dead. The tombstone is keyed by
    // the ref itself, so a death that arrives before the entity streams in is
    // remembered and applied at resolve time (§3.4).
    if (TombstoneHas(r.target))                           return ApplyResult::Tombstoned;

    // ---- 4. resolve --------------------------------------------------------
    void* ent = nullptr; uint32_t gen = 0;
    if (!ResolveRef(r.target, &ent, &gen)) {
        DeferOrDrop(r);                                   // §3.4
        return ApplyResult::Deferred;
    }

    // ---- 5. class tag ------------------------------------------------------
    if (!ClassTagMatches(ent, r.target.classTag))         return ApplyResult::ClassMismatch;

    // ---- 6. aliveness ------------------------------------------------------
    if (!IsAlive(ent)) {                                  // FN_ALIVE == 0
        // Already dead on this machine with no tombstone we recorded: adopt it.
        InsertTombstone(r.target, r.epoch, r.seq, r.tick);
        return ApplyResult::AlreadyDead;
    }

    // ---- 7. health service ------------------------------------------------
    void* svc = GetService(ent, 0x5EB03CEC);              // FN_GET_SERVICE
    if (!svc || *(uint32_t*)svc != 0x02712F60) {          // vtable check
        Log("apply: svc=%p vtable=%08X target=%016llX", svc,
            svc ? *(uint32_t*)svc : 0, (unsigned long long)r.target.objectId);
        return ApplyResult::ClassMismatch;                 // do not call blindly
    }

    // ---- 8. engine path ---------------------------------------------------
    ApplyResult res = g_backend->Apply(r.actor, r.target, r.amount, r.kind, r.tick);

    // ---- 9. bookkeeping ---------------------------------------------------
    if (res == ApplyResult::Applied) {
        int16_t now = *(int16_t*)((char*)svc + 0x5C);
        if (now <= 0) {
            InsertTombstone(r.target, r.epoch, r.seq, r.tick);   // death, causally ours
            SendDeath(r.target, r.actor, r.seq, r.tick);
        }
        g_lifeShadow[ent] = now;          // §4: suppress the diff poll for 2 intervals
        g_shadowHoldUntil[ent] = g_nowMs + 250;
        if (r.amount) ScheduleLifeSync(r.target, now);
    } else {
        // No silent downgrade. A downgrade to SetLife turns a hit into a health
        // delta and loses the corpse — exactly the disproven forged-state shape.
        Log("apply: FAIL target=%016llX res=%u amount=%u kind=%u seq=%u",
            (unsigned long long)r.target.objectId, (unsigned)res, r.amount, r.kind, r.seq);
    }
    return res;
}
```

### 3.3 Backend: `B_SETLIFE`

```cpp
ApplyResult BackendSetLife::Apply(EntityRef, EntityRef target, uint16_t amount,
                                  uint16_t, uint32_t) {
    void* ent = nullptr; uint32_t gen;
    if (!ResolveRef(target, &ent, &gen)) return ApplyResult::Unresolved;
    void* svc = GetService(ent, 0x5EB03CEC);
    if (!svc) return ApplyResult::ClassMismatch;
    int16_t cur = *(int16_t*)((char*)svc + 0x5C);
    int     want = (int)cur - (int)amount;
    if (want < -32768) want = -32768;         // int16 clamp, engine would UB otherwise
    if (cur < 0) return ApplyResult::AlreadyDead;   // SetLife refuses to raise it anyway
    typedef void (__thiscall *SetLifeFn)(int16_t*, int);
    ((SetLifeFn)0x19FB4C0)((int16_t*)svc, want);
    // Engine refusal is observable: read back. A refused write means the service
    // re-derives life elsewhere; log it once and mark the target "uncontrollable".
    if (*(int16_t*)((char*)svc + 0x5C) != (int16_t)want) {
        MarkUncontrollable(target);
        return ApplyResult::EngineRefused;
    }
    return ApplyResult::Applied;
}
```

`B_SETLIFE::Kill` calls `SetLife(svc, -1)` — negative is the death path **[V]** — and
never writes flags. That is the whole difference from the disproven
`Life=0xFFFF + flags|=0x01000000` recipe: that recipe wrote the *forged* state, this
one goes through the engine's own setter and lets the engine own the consequences.

### 3.4 Deferred / unresolved

```cpp
void DeferOrDrop(const DamageRecord& r) {
    // Streaming order is not guaranteed: a hit can arrive before the victim's cell
    // is loaded on this machine. Retry is bounded and cheap.
    Pending& p = g_pending.emplace(r.target, 0);
    p.rec = r;
    p.attempts++;
    p.nextMs = g_nowMs + 500 * p.attempts;     // 500, 1000, 1500 ms
    if (p.attempts > 3) {
        // Give up on the damage, keep the tombstone if it was lethal. The victim
        // applies its own death locally; the death record is what must not be lost.
        if (r.amount) InsertTombstone(r.target, r.epoch, r.seq, r.tick);
        Log("apply: dropped-unresolved target=%016llX seq=%u",
            (unsigned long long)r.target.objectId, r.seq);
        g_pending.erase(r.target);
    }
}
// Each frame, for every pending entry whose nextMs has passed and which resolves:
// run the normal path, and if the tombstone is set apply only the Kill.
```

### 3.5 Double-death race

Both machines land a lethal hit on the same guard in the same window. Local damage is
applied immediately and never waits for the peer (C2's 200 ms budget forbids a round
trip), so both machines produce a death locally. The rules that make this safe:

1. **The death handler is idempotent on the tombstone.** `DeathOnce(ref)` returns
   false if the tombstone exists; the XP grant, the mission credit and the corpse
   spawn all sit behind it. One corpse per machine, one credit per machine.
2. **The peer's CK_DEATH is absorbed, not applied.** `TombstoneHas(target)` is true,
   so `Kill` is never called twice. `g_doubleDeaths++` is incremented for the M2
   report; a non-zero counter is expected on race rows and is not a failure.
3. **The tombstone keeps the earlier key** for diagnosis but the set itself is
   order-free. Both machines converge because "dead" is a set membership test.
4. **Killer attribution can disagree.** A kills with 30 at tick 100, B with 30 at
   tick 101, victim at 30 life: both apply locally, both credit locally. This is
   accepted: credit is a per-machine cosmetic (XP), and the plan's authority table
   gives NPC death to "the causer" without a tiebreak. A tiebreak would need a
   shared clock, which the design deliberately does not have.

### 3.6 Late packet after respawn

```cpp
bool TombstoneHas(const EntityRef& ref, uint32_t* outGen) {
    // Keyed by (epoch, objectId). generation disambiguates a reused objectId.
    auto it = g_tomb.find({ref.epoch, ref.objectId});
    if (it == g_tomb.end()) return false;
    if (outGen) *outGen = it->second.gen;
    return true;
}
// On resolve of a live entity against an existing tombstone:
//   gen_now == tomb.gen  -> the same object; it is dead (or its death was applied).
//   gen_now != tomb.gen  -> the id was reused by a respawn; the tombstone is
//                           cleared, logged as RESURRECT_ID_REUSE, and the entity
//                           is live. This is the ONLY path that clears a tombstone
//                           inside an epoch.
```

A late CK_DAMAGE after that respawn carries the old `seq` and is caught by the dedup
window if it is ≤512 behind, and by `TombstoneHas` before the clear if it is older
than the clear. A late packet with a *newer* seq than the respawn is indistinguishable
from a legitimate hit on the respawned guard; it applies. That asymmetry is accepted
and logged (`RESPAWN_AMBIGUOUS`) because the alternative — refusing damage on any
freshly respawned guard — produces false survivals, which break C2 worse than one
extra hit does.

`ResolveRef` returning `gen` is the load-bearing capability here; its stability across
respawn is U5 (§7) and E1.1 is the experiment.

---

## 4. Fallback — health-diff

Used when 2A produces no callable apply path (§1.7 A1 and A2 both fail) or when
`cfg.combat.backend = setlife` is forced.

### 4.1 Shape

```cpp
struct HealthWatch {
    EntityRef ref;
    void*     ent;
    void*     svc;
    int16_t   lastLife;
    int16_t   expectLife;      // what our own apply says the value is now
    uint32_t  holdUntilMs;     // do not emit while set (§4.3)
    uint32_t  lastChangeMs;
};

// 10 Hz, one pass over the interest set (§4.4).
void HealthDiffTick(uint32_t nowMs) {
    for (HealthWatch& w : g_watch) {
        if (!w.svc) { w.svc = GetService(w.ent, 0x5EB03CEC); if (!w.svc) continue; }
        int16_t life = *(int16_t*)((char*)w.svc + 0x5C);

        if (w.holdUntilMs > nowMs) {          // our own apply is landing
            if (life == w.expectLife) { w.lastLife = life; w.holdUntilMs = 0; }
            continue;                          // never emit while held
        }
        if (life == w.lastLife) continue;

        if (life < w.lastLife) {               // a drop: this is a damage event
            uint16_t delta = (uint16_t)(w.lastLife - life);
            SendDamage(EntityRef{0,0,0,0} /*cause unknown*/, w.ref, delta,
                       /*kind*/ 0 /*unknown*/, nowMs);
            w.lastChangeMs = nowMs;            // used by LifeSync arbitration
        }                                      // a rise: regen or a respawn; no event
        w.lastLife = life;
    }
}
```

### 4.2 Parameters

| parameter | value | reason |
|---|---|---|
| poll interval | 100 ms (10 Hz) | 10 Hz is the plan's number; it bounds detection latency at 100 ms, which fits inside the 200 ms M2 budget with 100 ms of transport left |
| emit threshold | `delta >= 1` | 1 point is a real hit; any threshold above 1 silently drops chip damage and makes the matched-kill count wrong |
| rise handling | no event, resync shadow | regen and respawn both look like a rise; `SetLife` refuses to raise a negative life **[V]**, so a true resurrection cannot come out of this path |
| interest set refresh | 1 Hz | 20 guards typical, 128 max (`MAX_SYNC_ENTITIES`) |

### 4.3 Avoiding double-counting our own applied damage

Three independent guards, because any one of them can be defeated by the engine
clamping:

1. **Expect-write before the call.** `expectLife = cur - amount`, `holdUntilMs =
   now + 250` (2.5 poll intervals), both set *before* `SetLife` is called.
2. **Hold does not emit, but does resync.** While held, any observed value is
   absorbed into `lastLife` without an event. If the engine clamps (writes 0 instead
   of −10), the loop absorbs the clamp and clears the hold on the first match-or-
   stable read, so a clamp cannot leak an event.
3. **Direction check.** An emitted delta is dropped if a `DamageRecord` with the same
   `(target, ±1 tick window)` was applied locally in the last 250 ms. This catches the
   case where `holdUntilMs` expired because the engine was slow.

A remote-applied damage on the *peer* is not affected: the peer's applier sets the
peer's own hold on its own watch.

### 4.4 What the fallback does and does not prove

| property | primary path (§3) | health-diff (§4) |
|---|---|---|
| victim's life drops by the same amount on both machines | yes | yes |
| death occurs at the same moment on both | yes | yes, ±100 ms |
| damage **kind** replicated | yes | **no** — `kind = 0` on the wire |
| attacker identity replicated | yes | **no** — `actor` is null; kill credit is per-machine |
| hit direction / animation variant | yes | **no** |
| corpse appears | engine-owned, once | engine-owned, once — **only if** `SetLife(-n)` is what spawns the corpse; U4 |
| exact per-hit timing (hit-by-hit feel) | yes | no — a 3-hit burst arrives as fewer, larger deltas |

**Acceptance for the fallback, stated so it is weaker on purpose:** M2 checks 1
(matched kills), 3 (corpse), 5 (race) and 7 (no crash) are the only checks the
fallback is required to pass. Check 2 (false deaths) is stricter: the fallback must
produce 0 false deaths, and a *damage* event with no observable life change on the
emitter is impossible by construction (the event is generated from an observed drop),
which makes the fallback's false-death rate structurally 0 while the primary path's
is not (a replay bug can fabricate one). Checks on per-hit fidelity are explicitly
**out of scope** for the fallback and are recorded as `C2-lite` in the gate evidence.
The gate row for kind fidelity (`kills.tsv` column `kindMatched`) is `n/a` under the
fallback and M2 is **not** green unless ORCH accepts the downgrade in
`DECISIONS.md` — the plan's §4 Phase-2 rollback text ("host-authoritative highlight")
is the precedent for that acceptance.

---

## 5. M2 test plan

Scene: `scene_fight.py`, arena, 1 protagonist + 1 peer puppet + 40 guards in
`MAX_SYNC_ENTITIES` reach. Every victim is inside the other machine's interest set
at the moment of the kill (checked before the hit, logged as `victimVisible`).
Artifact: `ops/evidence/M2-<date>/kills.tsv`,
columns `id,tick,victimObjId,killerSide,amount,kind,aDeadMs,bDeadMs,corpseA,corpseB,dupDeaths,outcome`.

### 5.1 Kill matrix (20)

| id | side | scenario | amount | what it isolates | pass |
|---|---|---|---|---|---|
| K01 | A | single light hit, lethal | current life | baseline record→apply→death | death on B ≤200 ms, corpse both |
| K02 | A | single light hit, non-lethal | 1 | damage without death replicates | B life −1 exactly |
| K03 | A | medium hit | 25 | mid-magnitude exactness | B life −25 exactly |
| K04 | A | heavy hit | 100 | large delta, single record | B life −100 |
| K05 | A | overkill | 0x7FFF | clamp behaviour, int16 edge | death, no wraparound (B life ≤0, not positive) |
| K06 | A | 3-hit burst in 100 ms | 10,10,10 | batching + dedup under burst | B life −30 total, ≤1 extra record |
| K07 | A | two attackers, same guard, same 200 ms | 40+40 | multi-actor on one target | death once, `dupDeaths ≤1` |
| K08 | A | guard at 1 life, hit for 1 | 1 | exact-lethal boundary (life == 0 vs < 0) | death on both |
| K09 | A | hit on a guard that has not streamed to B yet | 30 | `Deferred` path, §3.4 | B applies within 2 s of streaming, or death-only |
| K10 | A | hit on a guard outside B's interest radius | 30 | interest-set boundary | **expected: not replicated**; logged `outOfInterest`, not a failure |
| K11 | B | K01 mirrored | current life | direction symmetry | ≤200 ms, corpse both |
| K12 | B | K02 mirrored | 1 | symmetry of exactness | A life −1 |
| K13 | B | K03 mirrored | 25 | — | A life −25 |
| K14 | B | K04 mirrored | 100 | — | A life −100 |
| K15 | B | K05 mirrored | 0x7FFF | clamp symmetry | death, no wraparound |
| K16 | B | 3-hit burst | 10,10,10 | — | A life −30 |
| K17 | B | two attackers | 40+40 | — | death once |
| K18 | B | guard at 1 life | 1 | — | death both |
| K19 | B | kill while A's player is mid-animation (pose stream saturated) | current life | channel independence (combat vs pose) | ≤200 ms, no pose regression |
| K20 | B | kill while A is firing (entity packet at max count) | current life | MTU / batching interaction | ≤200 ms, no entity-packet drop |

K10 is in the table to make the boundary explicit and machine-checked; it is excluded
from the 20/20 numerator in `gate.py` and the exclusion is named in the evidence file.

### 5.2 Races (5)

| id | scenario | expected | pass |
|---|---|---|---|
| R01 | A and B both land lethal in the same tick | one death, one corpse per machine | `corpseA==1 && corpseB==1`, `dupDeaths <= 1` |
| R02 | A lethal, B lethal 100 ms later | second hit arrives after the tombstone | B's record dropped as `Tombstoned`, corpse count unchanged |
| R03 | A lethal, B non-lethal in between | B's damage applies before its own death | B life monotone, one death |
| R04 | both non-lethal, sum lethal, 50 ms apart | each machine crosses zero on its own | death on both without a death record crossing the wire |
| R05 | A lethal while B is still resolving the victim (`Deferred`) | B's deferral resolves into a kill only | one corpse, no double death, no `ApplyResult::Applied` for the damage |

### 5.3 Loss and reordering (5 + 3 runs)

| id | scenario | expected | pass |
|---|---|---|---|
| L01 | CK_DEATH dropped once | 1 retransmit at 40 ms recovers it | death ≤200 ms p95 |
| L02 | CK_DEATH dropped twice | retransmits at 40/120/360 ms | death ≤400 ms, corpse both |
| L03 | CK_DEATH delivered before its CK_DAMAGE | tombstone wins, damage dropped | 0 resurrect, 1 corpse |
| L04 | CK_DEATH duplicated ×3 | dedup by `(epoch,seq)` | `dupDeaths == 0` |
| L05 | CK_DAMAGE arrives after the tombstone, tick newer | dropped by tombstone, `RESPAWN_AMBIGUOUS` not raised | 0 resurrect |
| L06–L08 | 10-kill scripted run at 0 %, 10 %, 20 %, 40 % uniform loss | kill count preserved, 0 false deaths, 0 resurrect | M2 check 1, 2, 6 |

Loss is injected at the receiver (`cfg.net.dropPct`) so it is reproducible and does
not need a proxy.

### 5.4 Gate rollup

`gate.py` computes M2 from `kills.tsv` + both logs:

```python
kills  = [r for r in rows if r["id"].startswith("K") and r["id"] != "K10"]
matched = sum(1 for r in kills if r["outcome"] == "matched")
false_deaths = sum(1 for r in rows if r["killerSide"] == "" and r["outcome"] == "death")
corpse = sum(1 for r in kills if r["corpseA"] == "1" and r["corpseB"] == "1")
assert matched == 20,                      "M2.1 matched kills 20/20"
assert false_deaths == 0,                  "M2.2 false deaths 0"
assert corpse == 20,                       "M2.3 corpse both 20/20"
assert p95([int(r["bDeadMs"]) - int(r["aDeadMs"]) for r in kills]) <= 200, "M2.4 latency"
assert all(int(r["dupDeaths"]) <= 1 for r in rows if r["id"].startswith("R")), "M2.5 races"
assert all(r["resurrected"] == "0" for r in rows if r["id"].startswith("L")),  "M2.6 late packet"
assert crash_count == 0,                   "M2.7 no crash in 15 min"
```

---

## 6. Risks

| id | risk | why it is real | mitigation / kill criterion |
|---|---|---|---|
| R1 | **No callable apply path.** `0x17106C0` is a filter whose caller needs an event object we cannot construct safely, and `0x173E190`'s event has no victim id. | The two candidate functions are [V] to exist and [H] in meaning. The event object layout is the part that makes synthesis dangerous: a 0x5E9-byte consumer reading `+0x9C`, `+0x10`, `+0x20` from an `a3`-shaped object means the object has at least 0xA0 bytes we did not author. | Health-diff (§4) is the floor and it is implementable today against [V] `SetLife`. **Kill criterion: stage 2A consumes 2 sessions without a callable applier ⇒ declare `B_SETLIFE` the backend and record `C2-lite`.** |
| R2 | **Divergent lives produce divergent deaths.** A guard at 20 life on A and 12 on B that takes 15 dies on B and survives on A. | `amount` is applied to each machine's own life; §2.7 is a 5 Hz reconciliation, not a synchronisation. | LifeSync at 5 Hz bounds the window to 200 ms. **Detection: `g_divergentDeaths` counter, incremented when a CK_DEATH arrives for a target our local life says is >0. Kill criterion: >0 on any M2 run ⇒ M2 red and LifeSync moves to 20 Hz before re-running.** |
| R3 | **`SetLife` does not produce the corpse.** | The disproven forged-state recipe produced no corpse **[V]**; `SetLife` producing one is U4, untested. | M2 check 3 is the test. **Kill criterion: corpse absent under `B_SETLIFE` ⇒ the fallback cannot pass M2 as written and C2 drops to "matched life + death flag only" (§4.4), needing ORCH acceptance in `DECISIONS.md`.** |
| R4 | **Tombstone leaks across world loads.** A tombstone keyed only by `objectId` kills a different guard after a mission load. | `epoch` is in the key [design]. Whether `epoch` actually bumps on every load is U10. | Key is `(epoch, objectId)`. **Detection: `STALE_EPOCH_ACCEPT` counter — a death applied to an entity whose `SetLife` reads full health and whose `gen` differs. Kill criterion: any occurrence ⇒ epoch is not bumping; add a save-fingerprint-derived epoch from the §1.3 hello.** |
| R5 | **`ResolveRef` is not stable enough for a 200 ms budget.** If `objectId` is not stable across respawn, the tombstone and the deferral queue both mis-key. | E1.1 is open (`PROGRAM-PLAN.md` §6) and is called the cheapest, most load-bearing experiment. | **Hard blocker: C2 does not start until E1.1 is answered.** This document assumes E1.1 returns "stable within an epoch". If it returns "not stable", §3's `gen` mechanism is replaced by a per-entity alias table and this file is revised before implementation. |
| R6 | **Hardware breakpoints are unavailable or are stolen.** A DR slot taken by another tool (a profiler, an anti-cheat, another seat's hook) silently drops the instrument. | 4 slots, 4 users in §1.2 — zero headroom. | The `0xCC` single-byte fallback (§1.6 shape, without code copying) is the documented alternative. **Kill criterion: DR install fails on the simulation thread twice ⇒ switch to `0xCC` and restart the session; do not proceed with a partially instrumented session.** |
| R7 | **Combat channel is starved by the entity channel.** `EntityPacket` is up to 128 records and already re-sends every captured entity per packet (`netstate.h:156`). | Adding 74 B datagrams to one socket means the 20 Hz cadence is shared. | Combat datagrams are sent immediately on event, not on the tick, and are ≤506 B. **Detection: `combatTxDeferred` counter — incremented when a combat datagram waits >1 tick behind an entity datagram. Kill criterion: p95 >20 ms ⇒ move combat to its own socket/port pair in v11.** |
| R8 | **False death by mis-resolved ref.** `classTag` is `crc32(name)&0xFFFF`, a 16-bit space with 40+ guard classes in reach. | A collision resolves an `EntityRef` to the wrong class and applies damage to an innocent. | Check is `objectId` (primary) **and** classTag **and** vtable (`0x02712F60`) for the service. **Kill criterion: any `ClassMismatch` on a `ResolveRef` that succeeded ⇒ raise `classTag` to a full 32-bit crc in v11 and treat v10 refs as invalid.** |

**Programme kill criterion for stage 2B** (mirrors `PROGRAM-PLAN.md` §4 Phase 2):
20/20 not reached in 3 weeks of 2B, or ≥3 crashes traced to a combat guard ⇒ ship
"host-authoritative highlight": replicate only `Death{targetRef, tick}` and the corpse
outcome, no per-hit damage. That is C2-lite, it uses §3.5's tombstone unchanged, and
it needs neither the applier nor the diff poll.

---

## 7. UNVERIFIED

Each row names the experiment that settles it. None of these is implemented against.

| id | claim needing proof | how it is settled |
|---|---|---|
| U1 | `0x17106C0` returns a boolean and the caller consumes it | §1.6 force-return test (E2.1 C5) |
| U2 | the caller VA of `0x17106C0` | `ret_addr` column of §1.3, first session |
| U3 | the layout of the event object the caller builds (needed to synthesise one for `B_FILTER`) | E2.1 pass 2: snapshot `a3` bytes `0x00..0xA0` on each hit, diff between two hits of different kinds; a field that changes with `kind` and one that changes with `amount` identify the pair |
| U4 | `SetLife` with a value ≤0 spawns the corpse and the death animation (it is the *whole* death path) | M2 check 3 under `B_SETLIFE`; if absent, DR on the corpse-spawn site and call it explicitly |
| U5 | `ResolveRef`'s `generation` out-param changes on respawn and stays fixed otherwise | E1.1 (`PROGRAM-PLAN.md` §6) |
| U6 | the `CSrvNPCHealth` pointer from `GetService` is stable for an entity's lifetime | A2 pass: log entity→svc, kill and respawn, compare |
| U7 | "interest radius" for the §4.4 bounding of the diff poll | instrumented run: at what distance does the peer's `EntityPacket` stop carrying the entity; use that number, do not invent one |
| U8 | the engine's damage is applied to the service at `+0x5C` or to another object that syncs to it | A2 pass: DR on `0x19FB4C0` during a punch, compare `this` to `GetService`'s result |
| U9 | `0x133F550`'s AI band (`*(ai+0xB8)&0xC == 4`) is set before or after `SetLife` on a kill | §1.2 `alive` column at DR0 vs DR2 on the same `ord` window |
| U10 | `epoch` can be derived from something that bumps on every world/mission load | instrument the load path and log a counter across three loads; if nothing bumps, use a save fingerprint as the epoch |
| U11 | the transport has no reliability/ack layer anywhere (only `recvfrom`/`sendto` were reviewed at `net.cpp:265-406`) | read `net.cpp` in full; if an ack exists, `ackSeq` reuses it instead of adding a field |
| U12 | the engine's `kind` domain for non-melee damage (bullet, cannon) matches the same table `3,6,14,23,5,25,26` | S4 (gunshots) in §1.4; a `kind` outside the table on a damaging hit means the table is melee-only and `kind` on the wire must be treated as opaque |
