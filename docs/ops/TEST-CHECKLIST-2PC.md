# Two-machine checklist — quick

Host = you (`26.113.208.88`). Guest = Loob (`26.170.50.44`).

## Before launch

| # | who | do | pass |
|---|---|---|---|
| 1 | Loob | `Get-FileHash "<game>\AC4BFSP.exe" -Algorithm SHA256` | `bc11ad79…ab5150`, 45,056,040 B |
| 2 | Loob | unzip kit, copy `AC4Coop.asi`, `dinput8.dll`, `known_builds.txt` into the game folder | 3 files present |
| 3 | Loob | rename `GUEST.ini` → `AC4Coop.ini` (PeerIP is already your address) | `PeerIP=26.113.208.88` |
| 4 | both | `ping <the other's 26.x address>` | replies |
| 5 | Loob | `New-NetFirewallRule -DisplayName "AC4 co-op UDP" -Direction Inbound -Protocol UDP -LocalPort 47801 -Action Allow -Profile Any` | no error |
| 6 | you | `AC4Coop.ini` already set: `PeerIP=26.170.50.44`, ports 47801/47801, `EntityHost=1` | — |

## Launch

| # | who | do |
|---|---|---|
| 7 | you | launch, load, get in-world, stand still |
| 8 | Loob | launch, load the same checkpoint, get in-world |

## 60-second check (both, newest `phase1_log_*.txt`)

```powershell
Get-Content "<game>\phase1_log_*.txt" -Tail 40
```

| look for | pass |
|---|---|
| `[Build] asi=36be76b2…` | same on both |
| `[Net] cfg … peer=26.x.x.x` | **not** 127.0.0.1 |
| `[Net] stats … rx=N pkt` | N > 0 on **both** |
| `[Net] apply seq=…` | appears on both |

`rx=0` → firewall or wrong IP. `127.0.0.1` → ini not read (wrong folder).

## See each other

| # | do | pass |
|---|---|---|
| 9 | walk past each other | the other body is visible, roughly placed |
| 10 | sprint, climb one wall, jump a roof | the ghost animates, does not freeze or teleport wildly |

## Kill test (the important one)

| # | do | pass |
|---|---|---|
| 11 | you kill 2 guards with the sword, in front of Loob | guards drop on Loob's screen within ~200 ms |
| 12 | Loob kills 2 guards | they drop on your screen |

If a kill does not match, the log will contain `[Kill] event objId=… unresolved` or
`[KH] unresolved …` — **that is the bug we are hunting, so keep the logs.**

## Finish

| # | who | do |
|---|---|---|
| 13 | both | quit normally |
| 14 | both | `powershell -ExecutionPolicy Bypass -File collect.ps1` |
| 15 | both | send the `ac4coop-<pc>-<time>\` folder |

## Expected to look wrong — ignore

* air/ledge kills on the guest (right clip, wrong position)
* guards you are not fighting (no ambient sync)
* missions, ships, progression
* the ghost trailing ~200 ms (Radmin hop)

## Stop and collect logs if

a crash, a black screen, or either log shows `0xC0000005`.
