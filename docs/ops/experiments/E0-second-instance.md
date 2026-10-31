# E0 — a second copy / VM (superseded as the *first* step by E0-A)

> **Read `E0-A-two-instance.md` first.** A second copy is not required to get two
> processes: the plugin already supports two instances from one install. This card
> is the upgrade path for when you want a *distinct IP* (network realism) or a
> *second human*.

Status: decided 2026-10-29. Owner: QA.
This card exists because the programme's critical path is "a second machine is
available". A second *copy of the game on this PC* is not that, but it is much
better than the loopback self-test the project has been running, and it can be
standing up today.

---

## 1. The one condition that makes it worth anything

The second copy must be **byte-identical** to the build every address in the
project was derived from:

```
size    45,056,040
sha256  bc11ad7924e1eca7363ee86aa3332c05386d0a05896c0b7370131daee9ab5150
```

Check before anything else:

```powershell
Get-FileHash "<second copy>\AC4BFSP.exe" -Algorithm SHA256
(Get-Item "<second copy>\AC4BFSP.exe").Length
```

If the hash differs, it is a different patch level and **every offset in the
project is invalid for it** — the ASI will install patches into the wrong
instructions and the build gate will (correctly) refuse. A refused build is the
good outcome; a `ForceUnknownBuild=1` on the wrong exe is a crash generator.

Also required: the second copy has no anti-tamper/anti-cheat of its own, and it is
a copy the user owns.

---

## 2. What each option actually tests

| option | two processes | distinct IP | distinct input | isolated saves | buys |
|---|---|---|---|---|---|
| **A. loopback, one process** (today) | no | no | no | n/a | codec + apply path only. **Zero network evidence.** |
| **B. second copy, same PC, 127.0.0.1** | yes | no | no | no (same user profile) | real two-process networking, join/leave, two independent worlds, timing, packet loss, repeatable gate scenes. The single biggest step available without a second human. |
| **C. second copy in a VM on this PC** | yes | yes | no | yes | B **plus** a real non-loopback stack and a real second machine boundary. Perf is poor; correctness is not. |
| **D. second PC** | yes | yes | yes | yes | everything, including the human "does it feel right" checks. |

**B is worth building today. C is worth building this week. D is still required
for M1's human sign-off.**

### Why B is not enough on its own

Both instances would report `peer=127.0.0.1`, which is exactly the pattern the
audit flags as "self-talk". The *evidence* problem is not the packets — two
processes are genuinely talking — it is that the log line cannot distinguish
"two processes" from "one process talking to itself". So:

* B runs must log a **process id + role** in the peer line:
  `[Net] cfg peer=127.0.0.1 pid=1234 role=host inst=2`.
* The gate for M1 still requires a non-loopback peer address (option C or D).
* B can be used for **M2/M3/M4/M5 functional gates** if the peer address check is
  replaced by "two distinct pids, both with rx>0, both with build hash" — a
  decision (D-011) that must be written down before it is used, not after.

---

## 3. Setup recipe for option B (second copy, same PC)

1. Copy the install directory to e.g. `D:\AC4BF-coop\`. Do **not** copy the
   Steam library metadata; run the exe directly (the multi-instance hook exists
   for exactly this).
2. Verify the exe hash (above). Record it in `ops/evidence/E0/hash.txt`.
3. Give the copy its **own** `AC4Coop.ini` with:
   `[Net] LocalPort=47801 PeerPort=47802 PeerIP=127.0.0.1 Enabled=1`.
   The first instance gets the mirror image (`47802`/`47801`).
4. Both instances must run the **same ASI hash**. Deploy from the same kit.
5. Saves: the game writes per-user. Two instances on the same user share the save
   directory and will fight. Mitigation, in order of preference:
   * run the second instance as a **different Windows user** (GUI session switch
     or `psexec -u`), or
   * copy the save folder aside before the run and restore after, or
   * accept the collision for functional tests only (never for progression tests).
6. Launch order: host first (so it binds `47801`), guest second.
7. Both logs go to `ops/evidence/E0-<date>/` with the pids in the filename.

## 4. Setup recipe for option C (VM)

Same as B, plus:

* bridged (not NAT) networking so the host can reach the VM by its own IP;
* `PeerIP` = the other machine's LAN/VM IP, not 127.0.0.1;
* GPU: the VM needs a working D3D9 path; a software rasteriser is enough to boot
  but may make the frame budget meaningless. Correctness tests only.
* It is a real second machine for **networking** and **process isolation**, but
  not for **input** or **visual feel**.

## 5. What still needs a human at a second keyboard

* M1 sign-off ("the gait matches", "he turns to face me").
* Any judgement of fun, feel, or "does the puppet look wrong".
* Two players actually playing together — which is the product.

Everything else (networking, event ordering, id matching, death races, packet
loss, save/load) can be driven by one human with two windows.

## 6. Decision

**D-011 (open):** allow option B as sufficient evidence for M2–M5 functional
gates, with the two-pid + build-hash replacement for the peer-address check.
M1 always requires option C or D.

Owner: human + ORCH. Written here so a later agent cannot quietly downgrade the
standard.
