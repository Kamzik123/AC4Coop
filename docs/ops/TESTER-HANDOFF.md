# Tester handoff — what to send, what to ask for

The kit is ready at `dist\2pc-kit-2026-10-28\` (5.5 MB). It is self-contained: plugin,
ASI loader, both inis, build allowlist, hash manifest, collector, and one README the
tester can follow without you explaining anything.

## Send them

1. `dist\2pc-kit-2026-10-28\` as a zip.
2. One line: **host or guest** — they get `HOST.ini` or `GUEST.ini`, and the two machines
   must not both be host.

## Ask them for two facts before you schedule the hour

| question | why it decides something |
|---|---|
| **Are they on the same LAN as you, or remote?** | same LAN → 2 minutes of setup; remote → they need Radmin VPN / ZeroTier first, which is the step people get stuck on |
| **Do they own AC4 on Steam (or another copy)?** | the exe hash must be `bc11ad79…`; a different release means stop before installing anything |

## Then the session itself

`README-2PC.md` inside the kit is the whole procedure: hash check → install → firewall →
host launches first → the 10-minute scene → `collect.ps1` → send the two folders back.

## What I want back

Two folders (`ac4coop-<pcname>-<time>\`), untouched. I will read:

* `[Build] asi=…` — same build on both
* `[Net] cfg … peer=<ip>` — not loopback
* `rx`/`tx` both non-zero on both
* `[Net] apply` per packet
* `[Smooth] maxD` over the session
* `[Kill]` / `[KF]` lines from the guard kills — including the `unresolved` ones, which
  are the bug I am hunting
* any crash address

M1's acceptance is machine-decided from those two logs plus one human line each ("the
gait matched, yes/no"). That is the gate that has been open since day one.

## Do not

* do not set `IdentityBand=1` (470 MB log, freezes the game)
* do not set `EntitySelfTest=1` (disables the incoming kill path)
* do not both set `EntityHost=1`
* do not edit or trim the logs before sending
