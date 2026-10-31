# RUN SHEET — first two-machine session (2026-10-31)

Build: **REL-fbd38635fc81** (commit `3c80c85`, clean tree, 303,616 B).
Kit: `dist\2pc-kit-2026-10-31\` — `AC4Coop.asi`, `dinput8.dll`, `HOST.ini`, `GUEST.ini`, `collect.ps1`.

**Both machines must run this exact hash.** A mixed build makes every failure unreadable.

---

## 0. Pre-flight (no game, 5 min)

1. Copy the kit into the game folder on both machines
   (`...\steamapps\common\Assassin's Creed IV Black Flag\`). The existing `dinput8.dll` is the ASI loader.
2. On **machine A (HOST)**: rename `HOST.ini` → `AC4Coop.ini`, then set `PeerIP=<machine B's LAN IP>`.
3. On **machine B (GUEST)**: rename `GUEST.ini` → `AC4Coop.ini`, then set `PeerIP=<machine A's LAN IP>`.
4. Both: `LocalPort=47801`, `PeerPort=47801` (already set). Allow `AC4BFSP.exe` through the firewall on
   both machines, on UDP and TCP.
5. Both: `EntityHost` is **1 on A, 0 on B**. This is the only asymmetry. Do not change it mid-run.
6. Confirm the two `AC4Coop.asi` files have identical SHA256:
   `FBD38635FC81DCC1FDC653F4E2B9881D58DF23F317C0102B5A50CF25BBE3B768`

If any of that is unclear, stop — a wrong IP or a mismatched build wastes the session.

---

## Run A — does the link work and do we see each other? (10 min, world empty)

**Purpose:** isolate the transport + puppet lane. Nothing else is being tested.
**Setting:** both ini files already have `DisableSpawns=0`; for this run **set `DisableSpawns=1` on BOTH**.

1. Both start AC4, load the same save, stand in Havana.
2. A presses nothing; B walks 5 m, jumps, stops.
3. A watches B's puppet; then swap roles.

**PASS:** each sees the other's body move within ~0.35 m, no flicker, both logs show `rx>0`.
**FAIL → stop the session and send both logs.** There is no point testing anything downstream of a dead link.

---

## Run B — G6: does the HOST classify the soldiers? (15 min, world live)

**Purpose:** the discriminator has never been confirmed against a live world. This is the highest-value
question after Run C.

**Setting:** set `DisableSpawns=0` on **BOTH** (restart both games).

1. A walks into a guard post with ≥ 4 visible soldiers and stands still for 20 s.
2. A walks to a pure-civilian street and stands still for 20 s.
3. B stays out of the area (its own world is independent; that is expected).

**PASS:** in the guard post the host log shows a `[Sync] kindrole` window with `soldier > 0`, and the
per-body `[KR]` lines for those bodies show `kind=3` with a fighter role. In the civilian street
`soldier` drops to 0 and `civilian > 0`.
**FAIL:** `soldier=0` next to visible soldiers, or the `[KR]` rows show `kind=-1`/`comps=` garbage.
Send the `[KR]` block verbatim — it says exactly which offset is wrong.

---

## Run C — G10: do the host's guards see the guest's body? (15 min)

**Purpose:** the gate that decides whether co-op stealth exists at all. Has never been answered.

**Setting:** same live world as Run B.

1. B stands 40 m away, out of sight (behind a building), crouched, still, for 30 s.
2. Record the host's guard detection state (`[G10]`/StageLog lines). Should stay **Unaware**.
3. B walks into the guard post, in plain view, to 15 m, and stands still for 20 s.
4. Record detection again.

**PASS (co-op stealth is possible):** guards transition Unaware → Investigating/Alerted when B is exposed,
and stay Unaware while B is hidden.
**FAIL (stealth is not possible without AI work):** detection never changes while B stands in front of a
guard. That is a **decision**, not a bug — record it and we cut Phase D.

---

## Run D — G9: does a kill land on both screens? (15 min)

Alternate killer: A kills 5, B kills 5. Use isolated guards, not mission targets.

**PASS:** 10/10 die on both screens, guest log shows `applied=1`, no `0x67B2EA`, corpse stays down 10 s.
**FAIL:** note which direction (host→guest or guest→host) and whether the corpse revives.

---

## Run E — G7/G8: binding quality (10 min)

A walks a loop through a crowd for 60 s. Capture the `[Sync] bind` window line.

**PASS:** `meanErr < 0.5 m` for bound bodies across the loop. `unmatched > 0` is expected and fine — that is
Phase B's job, not a failure here.

---

## Collect and hand back

On **each** machine, in the game folder:

```
powershell -ExecutionPolicy Bypass -File collect.ps1
```

That produces one zip. Send both zips. The log file is `phase1_log_<pid>.txt` next to the exe.

**Do not re-run the ini, rebuild, or edit the asi between runs.** If something must change, note it and make
the change once, then restart both games so the whole session is consistent.

## What we are deciding with this session

| run | question | if PASS | if FAIL |
|---|---|---|---|
| A | is the link alive | proceed | stop, fix transport |
| B | does G6 fire live | enemy sync is real | fix the offset that the `[KR]` rows name |
| C | **does co-op stealth exist** | Phase D continues | cut Phase D, ship a combat-only co-op |
| D | do kills land | G9 green | fix the lane that failed |
| E | is binding usable | Phase B worth starting | Phase B is required, not optional |
