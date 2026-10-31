2026-10-29: resolved all ten entries in `audit/ledger/contradictions.jsonl` (SHIP-SYNC-HANDOFF, COOP-ROADMAP, COOP-REVIEW, CODE-MAP, REVIEW-PACKET, MODLOG headlines); see `ops/evidence/DOC-2026-10-29/report.md`.
Authority model is per-system, not host-authoritative — `ops/DECISIONS.md` D-002.
The two-machine co-op report belongs to the 2026-10-18 build `64aa4256`; the deployed build `f474422f` (2026-10-28) has no two-machine run (audit claim D-01).
Combat relay: host side proven solo 2026-10-08 (`MODLOG.md:1118`); the two-machine datagram was unproven until G2.
Ship hull HP: `*(ship+0x5C)+0x84` is not hull HP (regenerates +6/tick, `MODLOG.md:2702`); the real field is still unfound.
MODLOG's 2026-10-09 night-#5 animation headline is superseded by the night-#5b correction (`MODLOG.md:2029`).
Transport option in the shipped code is `SO_REUSEADDR` (`ref/AC4Coop/src/net.cpp:333`), not `SO_EXCLUSIVEADDRUSE`.
The rest of this file is generated; edit this prose in `ops/status-prose.md` and re-run `ops/tools/gen_status.py`.
