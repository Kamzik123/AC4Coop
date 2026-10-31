#!/usr/bin/env python3
"""Gate runner for the co-op programme.

    python ops/tools/gate.py status
    python ops/tools/gate.py check M1 --evidence ops/evidence/M1-2026-11-03
    python ops/tools/gate.py set   M1 --evidence ops/evidence/M1-2026-11-03
    python ops/tools/gate.py reset

Rules enforced here (not by good intentions):
  * a gate cannot be green unless its evidence folder exists and contains the
    required artifacts (globs, minimum counts and content checks);
  * a gate cannot be green unless the build hash in STATE matches the ASI on disk;
  * changing or losing the build resets every gate to "unknown".

The programme milestones are M0-M5. The audit rig's gates are AG0-AG5; they are
only consulted for M0 and are never confused with these.
"""
from __future__ import annotations

import argparse
import datetime as dt
import fnmatch
import hashlib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OPS = os.path.dirname(HERE)
ROOT = os.path.dirname(OPS)
STATE_PATH = os.path.join(OPS, "STATE.json")
AUDIT_PATH = os.path.join(ROOT, "audit", "report", "audit.json")
ASI = os.path.join(ROOT, "ref", "AC4Coop", "bin", "AC4Coop.asi")

# Each entry: globs that must be present, with a minimum count, plus content
# checks (substring) for the files that are supposed to say something.
GATE_EVIDENCE = {
    "M0": {
        "globs": [("audit/report/AUDIT-REPORT-*.md", 1)],
        "content": {},
        "note": "audit aggregate report; AG0/AG2/AG4/AG5 must be green",
    },
    "M1": {
        "globs": [("phase1_log_*.txt", 2), ("AC4Coop_build_*.log", 2), ("SIGNOFF.md", 1)],
        "content": {"SIGNOFF.md": ["asi", "dirty"]},
        "note": "two logs (one per machine) + their build sidecars + human sign-off",
    },
    "M2": {
        "globs": [("kills.tsv", 1), ("phase1_log_*.txt", 2)],
        "content": {"kills.tsv": ["victimObjId", "aDeadMs", "bDeadMs"]},
        "note": "per-kill table + both logs",
    },
    "M3": {
        "globs": [("trials.tsv", 1), ("phase1_log_*.txt", 2)],
        "content": {"trials.tsv": ["guardObjId", "acquired", "alertMs"]},
        "note": "per-trial stealth table + both logs",
    },
    "M4": {
        "globs": [("hull.tsv", 1), ("phase1_log_*.txt", 2)],
        "content": {"hull.tsv": ["hit", "hullA", "hullB"]},
        "note": "hull-delta table + both logs",
    },
    "M5": {
        "globs": [("objectives.tsv", 1), ("phase1_log_*.txt", 2)],
        "content": {"objectives.tsv": ["missionId", "objectiveId", "completions"]},
        "note": "objective table + both logs",
    },
}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def load_state():
    with open(STATE_PATH, encoding="utf-8") as f:
        return json.load(f)


def save_state(st):
    st["updated"] = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    tmp = STATE_PATH + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(st, f, indent=2)
        f.write("\n")
    os.replace(tmp, STATE_PATH)


def current_build():
    return sha256(ASI) if os.path.isfile(ASI) else None


def sync_build(st):
    """Reset gates when the binary changed *or disappeared*."""
    cur = current_build()
    known = st.get("build", {}).get("asi")
    if cur is None:
        if any(v == "green" for v in st.get("gates", {}).values()):
            for g in st.get("gates", {}):
                st["gates"][g] = "unknown"
            save_state(st)
            print("ASI missing: all gates reset to unknown")
        return st
    if known != cur:
        st.setdefault("build", {})["asi"] = cur
        for g in st.get("gates", {}):
            st["gates"][g] = "unknown"
        st["gates_blocked_by"] = {g: "build changed" for g in st.get("gates", {})}
        save_state(st)
        print(f"build changed -> {cur[:16]}…: all gates reset to unknown")
    return st


def check_evidence(gate, folder, build_sha):
    spec = GATE_EVIDENCE.get(gate)
    if spec is None:
        return False, f"no evidence spec for {gate}"
    if not folder or not os.path.isdir(folder):
        return False, "evidence folder missing"
    present = sorted(os.listdir(folder))
    missing = []
    for pattern, minimum in spec["globs"]:
        hits = [f for f in present if fnmatch.fnmatch(f, pattern)]
        if len(hits) < minimum:
            missing.append(f"{pattern} (need {minimum}, have {len(hits)})")
    if missing:
        return False, "missing: " + "; ".join(missing)
    for fname, needles in spec["content"].items():
        path = os.path.join(folder, fname)
        if not os.path.isfile(path):
            continue
        txt = open(path, encoding="utf-8", errors="replace").read().lower()
        for needle in needles:
            if needle.lower() not in txt:
                return False, f"{fname} does not mention '{needle}'"
    if build_sha:
        # every gate folder must be tied to the build under test
        stamp = build_sha[:12].lower()
        if not any(stamp in open(os.path.join(folder, f), encoding="utf-8",
                                errors="replace").read().lower()
                   for f in present if os.path.isfile(os.path.join(folder, f))):
            return False, f"no file mentions the build hash {stamp}"
    return True, f"{len(present)} file(s)"


def audit_gates():
    if not os.path.isfile(AUDIT_PATH):
        return None, "audit.json missing (run audit/tools/run_audit.ps1)"
    a = json.load(open(AUDIT_PATH, encoding="utf-8"))
    return {g["id"]: g["pass"] for g in a.get("gates", [])}, None


def cmd_status(args):
    st = sync_build(load_state())
    print(f"build  asi={st['build'].get('asi','?')[:16]}…  commit={st['build'].get('commit','?')}")
    for g in sorted(st.get("gates", {})):
        why = st.get("gates_blocked_by", {}).get(g, "")
        print(f"  {g}: {st['gates'][g]:<8} {why}")
    print("next:", ", ".join(st.get("next", [])) or "-")
    return 0


def cmd_check(args):
    st = sync_build(load_state())
    if args.gate == "M0":
        gates, err = audit_gates()
        if err:
            print(f"M0: FAIL — {err}")
            return 1
        bad = [g for g in ("AG0", "AG2", "AG4", "AG5") if not gates.get(g)]
        ok = not bad
        why = "audit gates green" if ok else "audit gates red: " + ",".join(bad)
        # still require the report inside the evidence folder
        ok2, why2 = check_evidence("M0", args.evidence, st["build"].get("asi"))
        if not ok2:
            ok, why = False, f"{why}; evidence: {why2}"
    else:
        ok, why = check_evidence(args.gate, args.evidence, st["build"].get("asi"))
    print(f"{args.gate}: {'PASS' if ok else 'FAIL'} — {why}")
    return 0 if ok else 1


def cmd_set(args):
    st = sync_build(load_state())
    rc = cmd_check(args)
    if rc != 0:
        print("refusing to set green")
        return 1
    st["gates"][args.gate] = "green"
    st.setdefault("gates_blocked_by", {}).pop(args.gate, None)
    st.setdefault("evidence", {})[args.gate] = os.path.relpath(args.evidence, ROOT)
    save_state(st)
    print(f"{args.gate} -> green")
    return 0


def cmd_reset(args):
    st = sync_build(load_state())
    for g in st.get("gates", {}):
        st["gates"][g] = "unknown"
    save_state(st)
    print("all gates -> unknown")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("status"); s.set_defaults(fn=cmd_status)
    for name, fn in (("check", cmd_check), ("set", cmd_set)):
        c = sub.add_parser(name); c.add_argument("gate"); c.add_argument("--evidence", required=True)
        c.set_defaults(fn=fn)
    r = sub.add_parser("reset"); r.set_defaults(fn=cmd_reset)
    a = ap.parse_args(argv)
    a.evidence = getattr(a, "evidence", None)
    if a.evidence and not os.path.isabs(a.evidence):
        a.evidence = os.path.join(ROOT, a.evidence)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
