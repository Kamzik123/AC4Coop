#!/usr/bin/env python3
"""Generate STATUS.md from audit/report/audit.json + ops/STATE.json.

Two inputs, one output:

  ops/status-prose.md        hand-written, at most 10 lines (the only prose)
  audit/report/audit.json    factual half (gates, claims, artifacts, facts, docs)
  ops/STATE.json             factual half (build, seats, decisions, next)

Everything after the PROSE:END marker is derived; edit the inputs and re-run.

    python ops/tools/gen_status.py            # write STATUS.md
    python ops/tools/gen_status.py --check    # exit 1 if STATUS.md is stale

No clock is read: the generated timestamp is audit.json's own `generated` value,
so the same inputs always produce the same file.
"""
from __future__ import annotations

import argparse
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
STATUS = os.path.join(ROOT, "STATUS.md")
PROSE = os.path.join(ROOT, "ops", "status-prose.md")
AUDIT = os.path.join(ROOT, "audit", "report", "audit.json")
STATE = os.path.join(ROOT, "ops", "STATE.json")

PROSE_MAX = 10


def load(path):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def short(value, n=12):
    if not value:
        return "-"
    return str(value)[:n]


def yn(value):
    return "yes" if value else "no"


def md_cell(value):
    return str(value).replace("|", "\\|").replace("\n", " ")


def read_prose(path):
    if not os.path.isfile(path):
        return ["_No `ops/status-prose.md`; the prose section is empty._"]
    lines = [ln.rstrip() for ln in open(path, encoding="utf-8").read().splitlines()]
    # Drop leading/trailing blanks, then keep the first PROSE_MAX non-empty lines.
    while lines and not lines[0].strip():
        lines.pop(0)
    while lines and not lines[-1].strip():
        lines.pop()
    kept = [ln for ln in lines if ln.strip()][:PROSE_MAX]
    if len([ln for ln in lines if ln.strip()]) > PROSE_MAX:
        print(f"gen_status: {path} has more than {PROSE_MAX} non-empty lines; extra lines dropped",
              file=sys.stderr)
    return kept


def table(headers, rows):
    out = ["| " + " | ".join(headers) + " |",
           "|" + "|".join("---" for _ in headers) + "|"]
    for row in rows:
        out.append("| " + " | ".join(md_cell(c) for c in row) + " |")
    return out


def artifact_row(label, a):
    if not isinstance(a, dict):
        return [label, "-", "-", "-", "-"]
    return [label, a.get("path", "-"), a.get("size", "-"),
            short(a.get("sha256")), a.get("mtime", "-")]


def render(audit, state, prose):
    a = audit
    stats = a.get("stats", {})
    facts = a.get("facts", {})
    docs = a.get("docs", {})
    gates = a.get("gates", [])
    build = state.get("build", {})

    out = []
    out.append("# AC4 Black Flag co-op — STATUS")
    out.append("")
    out.append("<!-- PROSE:BEGIN — hand-written, max 10 lines; source: ops/status-prose.md -->")
    out.extend(prose)
    out.append("<!-- PROSE:END -->")
    out.append("")
    out.append(f"> Generated {a.get('generated', '?')} from `audit/report/audit.json` and "
               f"`ops/STATE.json` by `ops/tools/gen_status.py`. Do not edit below the PROSE:END marker.")
    out.append("")

    # ---- gates -----------------------------------------------------------
    out.append("## Audit gates")
    out.append("")
    rows = []
    for g in gates:
        rows.append([g.get("id"), g.get("name"), g.get("question"),
                     "PASS" if g.get("pass") else "FAIL", g.get("rows"),
                     g.get("fail"), g.get("score")])
    out.extend(table(["gate", "name", "question", "result", "claims", "fail", "score"], rows))
    out.append("")
    gate_line = ", ".join(f"{g.get('id')}={'PASS' if g.get('pass') else 'FAIL'}" for g in gates)
    out.append(f"**Audit line:** `{gate_line}` — claims "
               f"{stats.get('passed', '?')}/{stats.get('total', '?')} "
               f"({stats.get('score', '?')}%), "
               f"{stats.get('critical_fail', '?')} critical / "
               f"{stats.get('high_fail', '?')} high / "
               f"{stats.get('medium_fail', '?')} medium failures.")
    out.append("")

    # ---- build -----------------------------------------------------------
    out.append("## Build in STATE")
    out.append("")
    out.extend(table(["key", "value"], [
        ["asi", f"`{short(build.get('asi'))}`"],
        ["exe", f"`{short(build.get('exe'))}`"],
        ["commit", f"`{build.get('commit', '-')}`"],
        ["branch", f"`{build.get('branch', '-')}`"],
        ["dirty", ", ".join(build.get("dirty", [])) or "none"],
        ["note", build.get("note", "-")],
    ]))
    out.append("")

    # ---- artifacts -------------------------------------------------------
    art = a.get("artifacts", {})
    out.append("## Artifact hashes")
    out.append("")
    rows = [
        artifact_row("exe", art.get("exe")),
        artifact_row("idb", art.get("idb")),
        artifact_row("asi (repo)", art.get("asi_bin")),
        artifact_row("asi (deployed)", art.get("asi_deployed")),
    ]
    for kit in sorted(art.get("kits", []), key=lambda k: k.get("path", "")):
        rows.append(artifact_row(f"kit: {os.path.basename(os.path.dirname(kit.get('path', '')))}",
                                 kit))
    out.extend(table(["artifact", "path", "size", "sha256 (12)", "mtime"], rows))
    out.append("")
    out.append(f"Pinned exe sha256: `{a.get('pinned_sha', '-')}`.")
    out.append("")

    # ---- facts -----------------------------------------------------------
    out.append("## Runtime and static facts")
    out.append("")
    fact_rows = [
        ("two-machine logs found", facts.get("runtime.two_machine_logs_found", 0)),
        ("loopback self-talk (rx == tx)", yn(facts.get("logs.rx_equals_tx_loopback"))),
        ("an ASI hash is logged", yn(facts.get("logs.asi_hash_logged"))),
        ("action guard disabled by config", yn(facts.get("runtime.action_guard_disabled"))),
        ("deref guard covers its class", yn(facts.get("deref.guard_covers_class"))),
        ("authority conflict in docs", yn(facts.get("docs.authority_conflict"))),
        ("live contradictions", facts.get("docs.stale_headlines", 0)),
        ("hook bytes match", yn(facts.get("hooks.bytes_match"))),
        ("hook overlaps", facts.get("hooks.overlaps", "-")),
        ("py launcher available", yn(facts.get("env.py_launcher_works"))),
    ]
    out.extend(table(["fact", "value"], [[k, v] for k, v in fact_rows]))
    out.append("")

    # ---- failing claims --------------------------------------------------
    out.append("## Claims that do not pass")
    out.append("")
    failing = [c for c in a.get("claims", []) if c.get("status") != "PASS"]
    if failing:
        out.extend(table(["id", "severity", "status"],
                         [[c.get("id"), c.get("sev"), c.get("status")] for c in failing]))
    else:
        out.append("_None._")
    out.append("")

    # ---- contradictions --------------------------------------------------
    contra = docs.get("contradictions", [])
    live = [c for c in contra if c.get("live")]
    out.append("## Contradiction ledger")
    out.append("")
    if contra:
        rows = []
        for c in contra:
            rows.append([c.get("id"), c.get("topic"), "LIVE" if c.get("live") else "resolved",
                         f"{c.get('a', {}).get('file', '?')} ({c.get('a_state', '?')})",
                         f"{c.get('b', {}).get('file', '?')} ({c.get('b_state', '?')})"])
        out.extend(table(["id", "topic", "state", "side A", "side B"], rows))
        out.append("")
        out.append(f"**Live contradictions: {len(live)} / {len(contra)}.**")
    else:
        out.append("_No ledger entries._")
    out.append("")

    # ---- document integrity ---------------------------------------------
    headlines = docs.get("headlines", [])
    if headlines:
        out.append("## Document integrity (headline scan)")
        out.append("")
        rows = [[h.get("file"), h.get("headline_claims"), h.get("tagged"),
                 f"{h.get('untagged_pct', 0)}%", h.get("header_date") or "-",
                 h.get("max_date_in_body") or "-", yn(h.get("stale_header"))]
                for h in headlines]
        out.extend(table(["document", "bold claims", "tagged", "untagged",
                          "header date", "newest date", "stale header"], rows))
        out.append("")

    # ---- state -----------------------------------------------------------
    if state.get("gates"):
        out.append("## Gates in STATE")
        out.append("")
        out.extend(table(["gate", "state"],
                         [[k, v] for k, v in sorted(state["gates"].items())]))
        out.append("")
        blocked = state.get("gates_blocked_by", {})
        for gate in sorted(blocked):
            out.append(f"- {gate}: {blocked[gate]}")
        if blocked:
            out.append("")

    if state.get("seats"):
        out.append("## Seats")
        out.append("")
        rows = []
        for name in sorted(state["seats"]):
            s = state["seats"][name]
            rows.append([name, s.get("state", "-"), s.get("task", "-"),
                         s.get("since", "-")])
        out.extend(table(["seat", "state", "task", "since"], rows))
        out.append("")

    if state.get("decisions_open"):
        out.append("## Decisions open")
        out.append("")
        out.extend(f"- {d}" for d in state["decisions_open"])
        out.append("")

    if state.get("next"):
        out.append("## Next")
        out.append("")
        out.extend(f"- {n}" for n in state["next"])
        out.append("")

    out.append("---")
    out.append(f"Inputs: `audit/report/audit.json` (generated {a.get('generated', '?')}), "
               f"`ops/STATE.json` (updated {state.get('updated', '?')}).")
    return "\n".join(out) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="exit 1 if STATUS.md differs from the generated text")
    args = ap.parse_args(argv)

    audit = load(AUDIT)
    state = load(STATE)
    text = render(audit, state, read_prose(PROSE))

    if args.check:
        current = open(STATUS, encoding="utf-8").read() if os.path.isfile(STATUS) else ""
        if current != text:
            print("STATUS.md is stale; run python ops/tools/gen_status.py", file=sys.stderr)
            return 1
        print("STATUS.md is current")
        return 0

    with open(STATUS, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    print(f"wrote {STATUS} ({len(text)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
