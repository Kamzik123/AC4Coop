# QA — T4 adversarial verification of C3/C4/C5/C6

Read-only on `src/`. Revision verified: `net.cpp` sha256 `965F9D0E4F7265D3…` (32,376 B,
2026-10-30 11:17:31), `phase1.cpp` sha256 `CE5671594EA88B09…` (489,223 B, 11:17:07).
**Both files changed during this audit** (an earlier read of `EncodeEntityPacket` showed the
pre-fix code). Every line number below is against the hashes above. `CLAIMS.md` cites
`net.cpp:352` / `:487-495` and `phase1.cpp:3415`, which no longer exist for these claims —
the ledger is stale by 15–20 lines.

Verdict summary: **all four claims are DISPROVEN as current statements.** Three were fixed
by patches that landed after the claims were filed; C3 was never reachable at all.

---

## C3 — "EncodeEntityPacket can overflow g_txBuf" — DISPROVEN

Arithmetic, from the encoder itself:

| item | bytes | source |
|---|---|---|
| wire header (5 × u32, packed) | 20 | `netstate.h:172-180` |
| record fixed part (u32 key, u8 kind, u8 idKind, u16 mask, u64 wireId) | 16 | `net.cpp:194-198` |
| record optional part, all 8 field bits set | 42 | `net.cpp:199-206` (12+16+4+4+2+2+1+1) |
| **max record** | **58** | 16 + 42 |
| **max packet** = 20 + 128 × 58 | **7,444** | `MAX_SYNC_ENTITIES=128`, `netstate.h:125` |
| capacity `g_txBuf` | 16,384 | `net.cpp:99`, passed at `net.cpp:505` |

Headroom 8,940 B (54 %). The overflow only becomes possible above
`floor((16384-20)/58) = **282**` entities (248 using the declared `ENTITY_WIRE_RECORD_MAX=66`,
`netstate.h:182`, which over-counts by 8 — `EF_OBJID` is never emitted, `net.cpp:173`).
`T2`'s taskboard wording "heap overflow" is also wrong: `g_txBuf` is a **static** array
(`net.cpp:99`), so a smash would hit adjacent statics, not the heap.

The defect *was* real in the older revision: a 16-byte reservation followed by up to 42
unguarded bytes (max 42 B past `cap`), and the failure mode was `return 0` **after** the
shadow table had already been mutated for earlier records — a silent per-epoch desync, not
a crash. Current code computes the true `need` from the mask and `break`s:
`net.cpp:184-193`. Claim C3 = **DISPROVEN** (unreachable then, correctly bounded now).

## C4 — "g_haveEntities is never cleared" — DISPROVEN

Every reference in the tree (5, all in `net.cpp`):

| line | kind | code |
|---|---|---|
| `net.cpp:23` | W (init) | `bool g_haveEntities = false;` |
| `net.cpp:372` | W (true) | `g_remoteEntities = decoded; g_haveEntities = true;` |
| `net.cpp:373` | W (stamp) | `g_entitiesMs = GetTickCount64();` |
| `net.cpp:528-531` | **CLEAR** | `if (g_haveEntities && g_entitiesMs != 0 && (GetTickCount64() - g_entitiesMs) > NET_ENTITIES_TTL_MS) { g_haveEntities = false; memset(&g_remoteEntities, 0, …); }` |
| `net.cpp:538` | R | `have = g_haveEntities;` |

`NET_ENTITIES_TTL_MS = 3000` (`net.cpp:25`). Residuals, both unproven to matter:
(a) `net.cpp:522` returns before the TTL branch when `!g_enabled`, so a disabled link keeps
the stale snapshot indefinitely; (b) the clear is lazy — it happens only when a frame polls
`GetEntities`, and the "expired" debug line is one-shot per process.
Claim C4 = **DISPROVEN**. Was true before the TTL landed.

## C5 — "a guest has no death detection (CaptureGuards is host-only)" — DISPROVEN (half true)

* `CaptureGuards` is host-only on the normal path: gate `if (g_entHost) {` at
  `phase1.cpp:4305`, call at `:4315`, guest branch `:4320-4324` never calls it. **True.**
* Counter-example to "host-only" as an absolute: `phase1.cpp:4293` calls it whenever
  `s_selfTest` (`EntitySelfTest=1`), regardless of role.
* **The first clause is now false**: the guest has its own lane — `GuestDeathScan`
  defined `phase1.cpp:4054`, called `if (!g_entHost) GuestDeathScan();` at `:4328`.
  It really detects: `life = *(u16*)(hpSvc+0x5C)` and
  `dead = (*(u32*)(hpSvc+0x60) & 0x01000000) != 0` (`:4086-4087`), gated on a previously
  seen-alive body (`:4112-4116`), pushing via `GuestPushDeath` (`:4123`) with a
  256-entry tombstone (`GktMarkPushed`, `:3391-3407`) and a vanish lane (`:4147-4159`).
* The claim's citation `phase1.cpp:3415` is that tombstone check inside `GuestPushDeath`,
  not the host gate; the comment at `:3339` carries the same stale reference.

Claim C5 = **DISPROVEN as written**; it described the pre-patch build. Residual: the guest
watch table is bounded at 256 bodies (`[GKT] watch table FULL (256)`, `phase1.cpp:4096`).

## C6 — "the one-shot death packet was overwritten by the next snapshot" — DISPROVEN as current

* Kill-only datagrams are now kept **out of** the snapshot slot: `hadNonKill` is computed at
  `net.cpp:359-370` and the slot is written only under `if (hadNonKill || decoded.count == 0)`
  (`net.cpp:371`).
* Deaths go to a 32-slot ring (`NET_KILLQ 32`, `net.cpp:37`), written at `:367-369`, drained
  by `GetKillEntities` (`net.cpp:548`) and applied unconditionally each tick at
  `phase1.cpp:4287` — not gated on `haveIn`/`protoVer`/role.

Residuals (unproven severity):
1. ring overflow at 32 is silent **in the log** — the drop is reported through
   `dbg()`/`OutputDebugString` (`net.cpp:383-391`), so `coop_report.py` can never see it;
2. a *mixed* packet (kill + snapshot in one datagram) is still whole-slot last-writer-wins,
   and its kill entries would be applied twice (`phase1.cpp:4280` and `:4287`), deduped only
   by `ApplyPeerHealth`'s tombstone — **unproven**, that body was not read;
3. there is no `Net::Reset/Shutdown/Deinit` at all (`net.h:6-34`), so entity state is never
   torn down between sessions.

Claim C6 = **DISPROVEN as current** (verified history, fixed by B-4).

---

## T6 — `ops/tools/coop_report.py`

Stdlib only, read-only, exit 0/1/2. File or folder; `--expect-hash`, `--json`, `--selftest`.
Tag formats were taken from the printf strings, not guessed: `[Sync] bind` `phase1.cpp:7699`
(note: a real `maxErr=` field exists), `[Slot] bind` `:3915`, `[GKT] push` `:3429`,
`[KT] recv … applied=` `:6109/6112`, `[KT] death push` `:5719`. Crash rule copied verbatim
from `gate_parse.py:72-76` so the two tools cannot disagree; swallowed-`EXC` lines are
reported as INFO, not failures.

**Known defect (found by its own selftest, must be fixed before M2):** `check_deaths` sums
`kt_recv_applied` across the whole run and applies that single total to *both* lanes, so a
guest-lane receive can satisfy the host lane. The selftest case `death pushed, never applied`
therefore passes for the wrong reason — it should FAIL on `death host->guest`. Correct fix:
join each `[KT] death recv objId=` against the objIds pushed on that specific lane, instead
of counting. Harmless on today's logs (they contain zero death lines), which is why the
acceptance run does not expose it.

Also not yet seen on disk: `[Sync]`, `[Slot]`, `[GKT]`, `[KT]` appear in **no** existing log
(`ops/evidence/RUN-2026-10-29-*` has none), so every check above currently reports
"field absent" — correct behaviour, but the parser is still unvalidated against a real
death run.
