# Two-machine runbook

The first gate that needs a second PC. Everything below assumes both machines are
already running the **same** game build and the **same** ASI.

---

## 0. What "same" means (check first, not later)

| item | machine A | machine B |
|---|---|---|
| `AC4BFSP.exe` sha256 | `bc11ad79…ab5150` | identical |
| `AC4Coop.asi` sha256 | whatever `ops/STATE.json` says is deployed | identical |
| `AC4Coop.ini` | differs only in `PeerIP` | differs only in `PeerIP` |

If the exe hash differs, stop: every offset in the project is wrong for that copy.

## 1. Network

* Prefer a virtual LAN (Radmin VPN / ZeroTier / Hamachi) or the same physical LAN.
* Both machines must be able to ping each other.
* Windows Firewall: allow the game exe's inbound UDP on the chosen port (default
  `47801`) on **both** machines, or the session looks like "nothing happens".
* Record both IPs. A is the one that starts first.

## 2. Configuration

Two-machine mode uses **`LocalPort == PeerPort`** (each side binds the same port
locally and sends to the peer's same port). This is different from the same-PC
two-instance layout, which needs distinct ports because the sockets share a host.

`AC4Coop.ini` on A:

```ini
[Net]
Enabled=1
LocalPort=47801
PeerPort=47801
PeerIP=<B's IP>
```

On B: identical except `PeerIP=<A's IP>`.

`[Coop]` — **this is the part that differs per machine.** Both PCs are "instance 1" on
their own machine (the slot marker is per-process), so if both say `EntityHost=1` they
both send the entity stream and both try to resolve kills. Exactly one host.

On A (host):

```ini
[Coop]
EntitySync=1
EntityHost=1
EntitySelfTest=0
IdentityBand=0
ActionDeathLog=0
FixNullReturn=1
CombatTest=0
```

On B (guest): identical except **`EntityHost=0`**.

Why each line:
* `EntitySync=1` — without it the entity lane never runs, so there is no cross-machine
  id and no kill relay at all. `0` silently disables the feature the test is about.
* `EntitySelfTest=0` — with it on, `EntitySyncTick` returns before the incoming-packet
  poll and the host never processes a kill.
* `IdentityBand=0` — at `1` it writes the entire entity band to the log once per second;
  it produced a 470 MB file in three minutes and froze the game.
* `ActionDeathLog=0` — that is the clip capture, and it is only for single-machine runs.

## 3. Saves (do this before launching, both machines)

The plugin never touches saves, but two machines with the same profile can still
overwrite each other through the game. Back up the slot on each machine:

```powershell
$save = "$env:LOCALAPPDATA\Ubisoft Game Launcher\savegames"
Copy-Item $save "$save.backup-$(Get-Date -f yyyyMMdd-HHmm)" -Recurse
```

Both players should load **the same mission/checkpoint**, in the same district.

## 4. Launch and verify (before any scene)

1. Start A, load the save, get in-world. Start B, same.
2. On each machine, the newest `phase1_log_<pid>.txt` must contain:

```
[Build] asi=<hash> commit=<short> tree=clean
[Net] init=1  (bound/peer line goes to OutputDebugString)
[Net] cfg enabled=1 peer=<peer IP> local=47801 peerPort=47801 strictPeer=1
[Net] stats tx=N pkt … rx=N pkt …        <- both N > 0 within 10 s
[Net] apply seq=…                        <- the peer's packets are being applied
```

3. Both logs must show `peer=<the other machine>`, **not** `127.0.0.1`.
4. If either shows `rx=0`, the cause is one of: firewall, wrong `PeerIP`, or
   `NetStrictPeer=1` rejecting a different source address.

## 5. The M1 scene (10 minutes)

Scripted so the evidence is comparable:

| t | action | both players |
|---|---|---|
| 0–60 s | walk in a straight line | same direction, 20 m apart |
| 60–90 s | sprint | |
| 90–120 s | climb one wall | |
| 120–150 s | jump from a roof | |
| 150–180 s | enter and exit water (swim 20 s) | |
| 180–300 s | free roam, cross one district boundary | |
| 300–600 s | stand still, then walk away 100 m and back | streaming |

Stop, quit normally on both.

## 6. Evidence (`ops/evidence/M1-<date>/`)

* `phase1_log_<pid>.txt` from **both** machines
* `AC4Coop_build_<pid>.log` from both (the exe provenance)
* `SIGNOFF.md` with `asi=<hash> tree=clean|dirty` and, per player:
  `gait matches [ ] facing correct [ ] no visible pop [ ]`
* the two ini files, with the IPs redacted

Then:

```powershell
py ops\tools\gate.py check M1 --evidence ops\evidence\M1-<date>
```

## 7. Pass conditions

| # | check | pass |
|---|---|---|
| 1 | both `peer=` are non-loopback | both |
| 2 | `rx > 0` and `tx > 0` on both | both |
| 3 | `[Net] apply` ≈ `rx` | ≥ 90 % |
| 4 | `[Smooth] drop=0 wipe=0 clamp=0` | both |
| 5 | `maxD` | p99 ≤ 1.0 m, max ≤ 2.0 m |
| 6 | crash | 0 in 10 min |
| 7 | human sign-off | both |

## 8. Known failure modes, ranked

1. **Firewall** — the most common. Symptom: `rx=0` on both, `tx>0`.
2. **`PeerIP` left at 127.0.0.1** — symptom: `rx>0` but from yourself; `peer=` in
   the log is still loopback.
3. **Different ASI** — the build gate refuses (`verdict=UNKNOWN`), or the proto
   version check drops every packet silently.
4. **`EntitySelfTest=1`** — presence works, entity/kill lanes are dead.
5. **Save/profile collision** — the game may refuse to load or overwrite progress;
   that is why §3 exists.
