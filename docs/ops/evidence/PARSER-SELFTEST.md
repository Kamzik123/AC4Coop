# gate_parse.py — offline selftest

`ops/tools/gate_parse.py` decides M1 and M2 from the two plugin logs without the
game running. This file records the proof that the parser works, produced offline
on 2026-10-29. No game process, no `gate.py` and no `STATE.json` change is involved.

Stdlib only. Python is the non-Store install:

```powershell
C:\Users\Administrator\AppData\Local\Python\bin\python.exe ops\tools\gate_parse.py --selftest
```

```
gate_parse.py selftest - synthetic fixtures in C:\Users\ADMINI~1\AppData\Local\Temp\gate_parse_selftest_1xlfom_7

case                         expect    got       result
---------------------------  --------  --------  ------
good pair                    PASS      PASS      ok
same bound port              FAIL      FAIL      ok
                             failing: M1-02 E0-A bound ports differ
rx==tx on loopback           FAIL      FAIL      ok
                             failing: M1-03 peer not loopback; M1-06 rx!=tx (no self-count)
crash marker                 FAIL      FAIL      ok
                             failing: M1-10 no crash marker
M2 good table                PASS      PASS      ok
M2 late death + resurrected  FAIL      FAIL      ok
                             failing: M2-04 p95 latency<=200ms; M2-06 no resurrected

selftest: 6/6 expectations met -> OK
```

Exit code 0 on `OK`, 1 if any expectation is wrong. The four required cases are
the first four; the last two exercise the M2 `kills.tsv` path. The selftest also
asserts *which* check failed, so a fixture cannot pass for the wrong reason — the
same-port fixture must fail on `M1-02` (E0-A), the loopback fixture on `M1-03`/
`M1-06`, the crash fixture on `M1-10`.

Fixtures are generated in a temp directory (`%TEMP%\gate_parse_selftest_*`), and
are deleted when every expectation holds. The good pair claims a 700 s span by
setting mtime forward with `os.utime`; the parser reads duration from the file
span because the log body has no timestamps.

## What the real logs actually contain

Verified by running the parser on three logs from disk (read-only, `--out` pointed
at `%TEMP%`, never at `ops/evidence/`):

```powershell
python ops/tools/gate_parse.py M1 --a "...\phase1_log_5844.txt" --b "...\phase1_log_7172.txt" --out "$env:TEMP\gp_real"
```

```
M1 verdict: FAIL
checks: 13, passing: 6, failing: 5, info: 2
A: phase1_log_5844.txt pid=5844 port=47801([Net] cfg local=) peer=127.0.0.1 tx=129 rx=129 apply=116 actions=81 maxD=8.6878 drop=0 wipe=0 clamp=0 crash=none duration=119 s
A: build asi=field absent (field absent)
B: phase1_log_7172.txt pid=7172 port=47801([Net] cfg local=) peer=127.0.0.1 tx=404 rx=404 apply=104 actions=45 maxD=4.5595 drop=0 wipe=0 clamp=0 crash=none duration=129 s
B: build asi=field absent (field absent)

failing checks:
FAIL M1-02 E0-A bound ports differ [both]
  expected: A.boundPort != B.boundPort
  actual  : A=47801 ([Net] cfg local=) B=47801 ([Net] cfg local=) -> SAME PORT: single socket, both sides share one endpoint
  line    : [Net] cfg enabled=1 peer=127.0.0.1 local=47801 peerPort=47801 strictPeer=1
FAIL M1-03 peer not loopback [both]
  actual  : A peer=127.0.0.1, B peer=127.0.0.1 -> loopback peer (A,B): no packets crossed a network
FAIL M1-06 rx!=tx (no self-count) [both]
  actual  : A tx=129 rx=129, B tx=404 rx=404 -> tx==rx on A,B: counters describe a self-loop
FAIL M1-09 maxD<=2.0 [both]
  actual  : A maxD=8.6878, B maxD=4.5595
  line    : [Smooth] frames=173 hold=0 ext=0 clamp=0 eqT=0 drop=0 wipe=0 extra=77 avgD=0.1919 maxD=8.6878
  line    : [Smooth] frames=195 hold=0 ext=0 clamp=0 eqT=0 drop=0 wipe=0 extra=73 avgD=0.1659 maxD=4.5595
FAIL M1-11 duration>=10min [both]
  actual  : A=119 s, B=129 s (file span (mtime - ctime))
```

(the block above is trimmed to the check/expected/actual/line tuple of each
failure; the on-disk `verdict.txt` is untrimmed)

### Fields that were not where the plan said

1. **`[Net] up: bound:<p> -> <ip>:<p>` does not exist in any log on disk.** The
   plugin itself says so on line 2: `[Net] init=1  (bound/peer line goes to
   OutputDebugString)` — it goes to a debugger, not stdout. The bound port is only
   available as `local=<p>` on `[Net] cfg ...`. The parser accepts both and records
   which one it used next to the number; on today's build it is always
   `[Net] cfg local=`.
2. **`[Build] asi=<sha256>` is absent from all three logs.** `GATES.md` M0 requires
   it on line <= 3. It is therefore reported as an INFO row (`M1-i2`), not a
   silent pass, and the verdict says the two machines' builds cannot be compared.
3. **There are no timestamps and no heartbeat lines**, so no in-log duration. It
   is read from the file span (`mtime - ctime`) and labelled as such; the real logs
   are 119-148 s, far under the 600 s M1 scene.
4. **`rx` always equals `tx`** in every `[Net] stats` line of all three logs, and
   `peer=127.0.0.1` with `local == peerPort == 47801`. The counters describe a
   socket talking to itself, not a peer exchange. That is why `M1-06` exists and
   why the same-port fixture is the E0-A fixture.
5. `[Net] cfg` appears **once**, on line 2, before any traffic; the last
   `[Net] stats` line is the session total and is what the checks use.

## M2

`--kills` takes the `GATES.md` table (`tick, victimObjId, killerSide, aDeadMs,
bDeadMs, corpseA, corpseB`, tab separated, header required).

| check | rule |
|---|---|
| `M2-00` | every required column present; a missing column is `field absent`, and every dependent check then fails rather than skipping |
| `M2-01` | rows with a death on both sides == 20 |
| `M2-02` | 0 rows where exactly one side reports a death (a death with no cause on the peer) |
| `M2-03` | all 20 matched rows have `corpseA` and `corpseB` truthy |
| `M2-04` | p95 of `abs(bDeadMs - aDeadMs)` over matched rows <= 200 ms (nearest rank); max is reported as INFO |
| `M2-05` | per duplicated `victimObjId`: exactly one death row, at most one corpse per side |
| `M2-06` | no victim whose row appears dead and is seen alive again at a later tick |
| `M2-07` | no crash marker in either log |

A missing `kills.tsv` scores 7 FAIL rows and exit 1 — it never becomes "not
applicable".

## Output files

M1 writes `m1.tsv` (columns `check, scope, status, expected, actual, source_line`)
and `verdict.txt`; M2 writes `m2.tsv` and `m2-verdict.txt`. Exit code is 0 only
when no blocking check fails. `INFO` rows never decide a gate. The tool does not
write `STATE.json` and does not flip anything green — that stays with `gate.py`.
