# Agents — current roster and how to read them

Every seat is a full OpenCode session with its own context and tools. Read any of them with:

```powershell
py ops\tools\session_read.py <session-id>          # last 10 turns, 1800 chars each
```

or dump everything:

```powershell
py "C:\Users\Administrator\AppData\Local\Temp\opencode\dump_session.py" <session-id>
```

## Hive seats (live)

| seat | session | owns |
|---|---|---|
| NET | `ses_e741f0becffevKuDnylqDe7rph` | `src/net.cpp`, `src/net.h` |
| IMPL | `ses_e741f0b73ffeeVFuPV4Qu0iltx` | `src/phase1.cpp` — the only writer |
| RE | `ses_e741f0b23ffemD1twBuCaV2rgl` | the binary (IDA), `ops/RE-*.md` |
| QA | `ses_e741f0ad5ffeoIwLMuj5xt0Tun` | verification, `ops/tools/coop_report.py` |
| AI | `ses_e741f0a85ffehWi7MKTgSQJzJM` | stealth/combat design, `ops/AI-*.md` |

Created with `ops/tools/mkseat.py`, which sends the only model payload the API accepts:
`{"model": {"id": "deepseek-flash", "providerID": "deepseek"}}`. A bare `POST /api/session`
inherits a free default model and times out.

## Report channel

`ops/tools/bus.py` — reports are files in `ops/bus/outbox/`.

```powershell
py ops\tools\bus.py inbox --ack        # what the orchestrator reads
py ops\tools\bus.py status             # last report per seat + freeze detector
py ops\tools\bus.py send QA "<text>"   # outbound, journalled in ops/bus/sent/
```

`ops/tools/bus_watch.py` (running, `ops/bus/bus_watch.pid`) wakes the orchestrator when a new
report file appears. It has no quiet heuristic and is fully recoverable: if it dies, nothing is
lost, the report stays on the bus.

## Retired

- `ops/tools/watch_seats.py` — the polling watcher. Required 45 s of quiet, seats stay `busy`, so
  finished reports were dropped. Kept for history, not running.
- `ops/tools/dm.py` — direct session messaging, superseded by `bus.py send`.
- Five sessions created on the free default model (`ses_e74276a0…`, `ses_e7427696…`, `ses_e742769b…`,
  `ses_e7427692…`, `ses_e742768c…`) — abandoned, not reused.
- Dozens of one-shot worker sessions spawned by the seats themselves; they are ephemeral by design.

## Research sessions from this run (read-only, finished)

| id | result |
|---|---|
| `ses_e74290e3fffePWQRB6CkF5XQxb` | verified C1 (component count at `+0x66`) independently |
| `ses_e74321c72ffelqu5M3v8uRrpK6` | discovered `+0x66` (with `GetComponentByType @0x5200C0`) |
| `ses_e742ddd77ffeoK4ZwT0JWNaP81` | no archetype spawn factory; clone-a-guard is the route |
| `ses_e742c25e9ffeRMERkZgIjeoPKk` | net receive-path audit (12 findings) |

## Killed

- `ses_e73dfe2f1fferk6GOWns9DtwWz` (GFX seat) - deadlocked on the `question` tool: called it, never returned, `busy` with no progress. Interrupted and abandoned. Seed inventory left at `ops/GFX-INVENTORY.md`; resume in a fresh seat from that file.
