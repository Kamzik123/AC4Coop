# E0-A — two instances from ONE install (15 minutes, no second copy)

Status: ready to run. Owner: QA + human.
This exists because the programme's critical path was "a second machine". It turns
out the shipped code already contains a same-PC two-instance path. That path has
never been exercised in any log on disk.

---

## 1. Why this can work at all (verified, not assumed)

**The game's only single-instance guard is one `CreateMutexW` call.**

* `KERNEL32!CreateMutexW` IAT slot `0x1E3A20C` has **exactly one caller**:
  `_WinMain@16` at `0x406D25` — verified with `xrefs_to` on the IAT slot; every
  other synchronisation import (`CreateEventW` and friends) has unrelated callers.
* The guard is **not** "exit if `ERROR_ALREADY_EXISTS`". It is:
  ```
  0x406D31  CreateMutexW(nullptr, 0, &Name)
  0x406D3A  v5 = WaitForSingleObject(MutexW, 0) != 0
  0x406D46  v6 = WaitForSingleObject(MutexW, 0x1388)   ; 5 s
  0x406D4A  if (!v6) { ...the whole game... }
  ```
  So a second instance **waits** for the first, in 5-second slices. That is the
  "one launch per boot" zombie hazard the project already documented.
* The plugin hooks exactly that slot (`dllmain.cpp:321`) and renames the mutex to
  `<name>_ac4coop_<pid>`, so `WaitForSingleObject` returns `WAIT_OBJECT_0`
  immediately and the second instance proceeds.

**The plugin already implements a same-PC second instance in the transport.**

`net.cpp:335-344`:

```cpp
int bindPort = localPort, sendPort = peerPort;
if (bind(g_sock, ...) == SOCKET_ERROR) {
    bindPort = peerPort; sendPort = localPort;
    g_secondInstance = true;   // LocalPort taken -> same-PC 2nd instance (guest)
```

The comment in the source says exactly this. Together the two mechanisms are a
designed, shipped, **never-tested** two-process path.

---

## 2. The one thing that can make it fail silently

`net.cpp:333` sets `SO_REUSEADDR` **before every bind**. On Windows,
`SO_REUSEADDR` permits two sockets to bind the same UDP port, and incoming
datagrams go to the **last** binder. If that happens here:

* the guest's bind of `LocalPort` **succeeds**, so the fallback never runs,
  `g_secondInstance` stays false, and both processes claim the same port;
* the host then receives nothing, the guest receives the host's packets, and the
  send port points at a port nobody is listening on.

The symptom is precise and must be checked, not guessed: **both logs say
`bound:47801`**. The PASS condition below is therefore *different bound ports*,
not merely "it ran".

If that is what happens, the fix is one of:

1. `SO_EXCLUSIVEADDRUSE` instead of `SO_REUSEADDR` on the **first** bind (the
   project's own `COOP-REVIEW.md` already claims this; `CODE-REVIEW.md` claims the
   opposite — this experiment settles which is true);
2. or an explicit per-instance port: `AC4COOP_INSTANCE=1` env var / `--instance`
   reading `LocalPort` from a different section, so no bind is ever contested.

Either is a 20-line change. Both are cheaper than a second PC.

---

## 3. Recipe

Do this on the machine that has the Steam install.

1. **Back up saves first.** Copy the save folder aside; two instances share the
   same profile and the same save directory. Restore afterwards.
2. Confirm the deployed `AC4Coop.asi` is the build under test (`f474422f…` or
   whatever `ops/STATE.json` says). Record it.
3. In `AC4Coop.ini`, set:
   ```ini
   [Net]
   Enabled=1
   LocalPort=47801
   PeerPort=47802
   PeerIP=127.0.0.1
   ```
   One file, read by both instances. Do not make a second config.
4. Launch instance A normally. Load a save, get in-world, stand still.
5. Launch instance B from the same exe (the mutex hook is default-on). Load the
   same save, get in-world.
6. Look at both `phase1_log_<pid>.txt` files, first lines.

---

## 4. PASS / FAIL

| # | check | PASS | meaning if it fails |
|---|---|---|---|
| 1 | both processes are alive after 60 s | yes | the mutex/launcher path is still blocked — capture the dump + log |
| 2 | A logs `bound:47801 -> 127.0.0.1:47802` | yes | config not read (CWD wrong) |
| 3 | B logs `bound:47802 -> 127.0.0.1:47801` | yes | `SO_REUSEADDR` ate the fallback (§2) — fix as above |
| 4 | both logs show `rx > 0` and `tx > 0` after 60 s | yes | one-way: port/peer mismatch |
| 5 | both logs show `[Net] apply` > 0 | yes | packets arrive but the puppet is not applied (separate bug) |
| 6 | each sees the other's puppet move | yes | **this is C0 on one PC** |
| 7 | no crash in 5 min | yes | report the fault address |

Checks 1–5 are machine-decidable from the logs. Check 6 needs eyes. Check 7 needs
the dump path.

---

## 5. What this buys, and what it does not

**Buys:** two real processes, two independent worlds, real sockets, the join
path, packet loss, event ordering, death races, id matching, save/load — i.e.
almost every functional test in M1–M5. It can be run repeatedly by one human,
which is what makes regression testing possible at all.

**Does not buy:** routing/NAT realism (still loopback), a second pair of hands,
a second GPU, or a trustworthy frame-time measurement. The *network* claim still
needs a distinct IP eventually; the *behaviour* claims do not.

**Consequence for D-011:** if this passes, M1–M5 functional gates may be run on a
single PC with two instances, provided the evidence folder contains **two logs
with different bound ports and two distinct pids**. M1's "non-loopback peer" line
moves to a separate, later check — it tests the transport, not the co-op.

---

## 6. If it passes

Immediately do, in this order:

1. `E1.1` (identity stability) — now observable with two clients, not just one.
2. `E2.1` (damage filter) — the COMBAT card is already written for one machine,
   and two instances make the apply-side observable.
3. The M1 scene, twice, to get the first real `SIGNOFF.md`.

## 7. If it fails

Post the two log heads to `ops/bus/inbox/orch/` with the exact `bound:` lines.
The failure mode names the fix, and the fix is in `net.cpp` — not in a shopping
list.
