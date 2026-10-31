#!/usr/bin/env python3
"""coop_report.py - offline pass/fail for a co-op run, from the phase1 log alone.

    python ops/tools/coop_report.py ops/evidence/RUN-2026-10-29-killworks
    python ops/tools/coop_report.py path/to/phase1_log_12372.txt --expect-hash 25A601E5AB8A
    python ops/tools/coop_report.py --selftest

Accepts one `phase1_log_*.txt` or a folder of them (a two-machine run, or one log
per process). Prints PASS/FAIL for: build hash, net link, [Sync] bind, [Slot]
binds, deaths in both directions, crash markers, and the top-10 log tags.

Stdlib only. Read-only: this tool never writes a log, never writes STATE.json and
never turns a gate green. Exit 0 = no blocking FAIL, 1 = FAIL, 2 = usage/IO error.

Where every field comes from (verified against ref/AC4Coop/src, not guessed):

  [Build] asi= commit= tree=   dllmain.cpp:162 + phase1.cpp:176 (first line of the file)
  [Net] cfg                    observed on disk; the [Net] up:/bind lines go to
                               OutputDebugString (net.cpp:451/463), so they are NOT in the file
  [Net] stats tx= rx=          observed on disk; session totals
  [Sync] bind sent= bound=     phase1.cpp:7699  ("sent == bound + unmatched" is the
       unmatched= meanErr=            invariant written above it at :7696)
  [Slot] bind slot= objId=     phase1.cpp:3915;  releases phase1.cpp:2739/3778/3787/3887/4044
  [KT] local death detected    phase1.cpp:2988/3293  (host lane: detection)
  [KT] death push              phase1.cpp:5719        (host lane: onto the wire)
  [KT] death recv ... applied= phase1.cpp:6109/6112   (receive + apply, either side)
  [GKT] guest death detected   phase1.cpp:4121/4156   (guest lane: detection)
  [GKT] guest death push       phase1.cpp:3429        (guest lane: onto the wire)

Known blind spots, deliberately reported rather than papered over:
  * a full kill ring logs via dbg()/OutputDebugString (net.cpp:388), so a dropped
    death is invisible in phase1_log and cannot be counted here;
  * the log carries no role (host/guest) marker, so deaths are checked per LANE
    ([KT] = host-originated, [GKT] = guest-originated), not per file. A lane is
    satisfied only when a `[KT] death recv` applied an objId that THAT lane
    pushed - never by a receive belonging to the other lane;
  * the [KT]/[GKT] death lines are rate-limited to 10 per gate per process
    (KtOn, phase1.cpp:2497), so their counts are a sample, never a total: the
    death checks are presence-based on purpose and a counter of 10 is flagged;
  * a `*_head+tail*` file is a truncated excerpt, not a run: flagged, never scored.
"""
from __future__ import annotations

import argparse
import fnmatch
import json
import os
import re
import sys
import tempfile
from collections import Counter

PASS, FAIL, INFO = "PASS", "FAIL", "INFO"

# ---- matchers -----------------------------------------------------------------
# Crash rule copied verbatim from gate_parse.py:72-76 so the two tools cannot
# disagree. Deliberately narrow: `[WB] exception cap reached` is benign.
CRASH_RE = re.compile(
    r"\bcrash(?:ed|ing)?\b|\[crash\]|\bfatal\b|unhandled exception|"
    r"access_violation|0xc0000005|assert(?:ion)? failed|abort(?:ed)?\b",
    re.IGNORECASE,
)
# SEH lines the code prints when it swallows a fault. NOT a crash by the rule
# above, but worth showing next to it.
EXC_RE = re.compile(r"\[[A-Za-z][A-Za-z0-9]*\][^\n]*\b(?:EXC|EXCEPTION)\b")

BUILD_RE = re.compile(
    r"\[Build\]\s+asi=(?P<asi>[0-9a-fA-F]{8,64})(?:\s+commit=(?P<commit>\S+))?(?:\s+tree=(?P<tree>\S+))?"
)
BUILD_UNAVAILABLE_RE = re.compile(r"\[Build\]\s+unavailable")

CFG_RE = re.compile(r"\[Net\]\s+cfg\b")
UP_RE = re.compile(
    r"\[Net\]\s+up:\s*bound:(?P<bound>\d+)\s*(?:->|=>|\u2192)\s*"
    r"(?P<ip>[0-9A-Za-z_.:\[\]-]+):(?P<port>\d+)"
)
STATS_RE = re.compile(r"\[Net\]\s+stats\b")

SYNC_BIND_RE = re.compile(
    r"\[Sync\]\s+bind\s+sent=(?P<sent>\d+)\s+bound=(?P<bound>\d+)\s+"
    r"unmatched=(?P<unmatched>\d+)\s+meanErr=(?P<mean>-?\d+(?:\.\d+)?)"
    r"(?:\s+maxErr=(?P<max>-?\d+(?:\.\d+)?))?"
)
SLOT_BIND_RE = re.compile(
    r"\[Slot\]\s+bind\s+slot=(?P<slot>\d+)\s+objId=(?P<objId>\d+)\s*->\s*"
    r"ent=(?P<ent>[0-9A-Fa-f]{8})\s+via=(?P<via>\S+)"
)
SLOT_REL_RE = re.compile(r"\[Slot\]\s+release\s+slot=(?P<slot>\d+)\s+reason=(?P<reason>[\w-]+)")

KT_LOCAL_RE = re.compile(r"\[KT\]\s+local death detected\s+objId=(?P<objId>\d+)")
KT_PUSH_RE = re.compile(r"\[KT\]\s+death push\s+objId=(?P<objId>\d+)\s+seq=(?P<seq>\d+)")
KT_RECV_RE = re.compile(
    r"\[KT\]\s+death recv\s+objId=(?P<objId>\d+)\s*->\s*ent=(?P<ent>[0-9A-Fa-f]{8})"
    r".*?applied=(?P<applied>\d)"
)
KT_RESOLVE_RE = re.compile(r"\[KT\]\s+death resolve\s+objId=(?P<objId>\d+)")
KT_SKIP_RE = re.compile(
    r"\[KT\]\s+death push SKIPPED\s+objId=(?P<objId>\d+).*?reason=(?P<reason>\S+)"
)
GKT_PUSH_RE = re.compile(r"\[GKT\]\s+guest death push\s+objId=(?P<objId>\d+)")
GKT_DETECT_RE = re.compile(r"\[GKT\]\s+guest death detected\s+objId=(?P<objId>\d+)")
GKT_SKIP_RE = re.compile(
    r"\[GKT\]\s+push SKIPPED\s+objId=(?P<objId>\d+).*?reason=(?P<reason>\S+)"
)

TAG_RE = re.compile(r"^\[(?P<tag>[A-Za-z][A-Za-z0-9_+.-]*)\]")
PID_RE = re.compile(r"phase1_log_(\d+)", re.IGNORECASE)
EXCERPT_RE = re.compile(r"head\+tail|excerpt|truncat", re.IGNORECASE)

# C21 discriminator, one line per enumerated NPC, shipped in REL-80f9a9a7e588
# (phase1.cpp:3255, behind [Coop] KrLog, default 0, capped 96/5s and 600/process):
#   [KR] ent=%08X objId=%llu raw=%08X kind=%d role=%d bhv=%08X skel=%u comps=%u
# Parsed as key=value rather than one fixed regex, so a field added or dropped by
# IMPL does not silently stop matching. NOTE kind/role are SIGNED decimals: a row
# whose +0x9C read failed prints kind=-1 role=-1 and must be reported, not dropped.
KR_TAG_RE = re.compile(r"\[KR\]\s+(?P<body>\S.*)$")
KR_FIELDS = {
    "ent":   re.compile(r"(?<![\w.-])ent=([0-9A-Fa-f]{8})"),
    "objId": re.compile(r"(?<![\w.-])objId=(\d+)"),
    "raw":   re.compile(r"(?<![\w.-])raw=([0-9A-Fa-f]{8})"),
    "kind":  re.compile(r"(?<![\w.-])kind=(-?\d+)"),
    "role":  re.compile(r"(?<![\w.-])role=(-?\d+)"),
    "bhv":   re.compile(r"(?<![\w.-])bhv=([0-9A-Fa-f]{8})"),
    "skel":  re.compile(r"(?<![\w.-])skel=(\d)"),
    "comps": re.compile(r"(?<![\w.-])comps=(\d+)"),
    "stage": re.compile(r"(?<![\w.-])stage=(-?\d+)"),
    "d2":    re.compile(r"(?<![\w.-])d2=(-?\d+(?:\.\d+)?)"),
}
KR_REQUIRED = ("ent", "raw", "kind", "role", "bhv", "skel")

KR_PACK_MASK = 0x1FF          # kind = raw & 7, role = (raw >> 3) & 0x3F  => bits 0..8 only
KR_BHV_GENERIC_NPC = 0x026E34D8
KR_STAGE_FIGHT = 4            # HumanGroundStage.Fight
KR_STAGE_NAVIGATION = 1


# What the deployed build 280D652169D0 actually emits (phase1.cpp:7798): four
# summed counters, no per-body pair. Counted for every candidate BEFORE the
# soldier filter, so `n` is the whole enumerated human population.
KR_TOTAL_RE = re.compile(
    r"\[Sync\]\s+kindrole\s+n=(?P<n>\d+)\s+soldier=(?P<soldier>\d+)\s+"
    r"civilian=(?P<civilian>\d+)\s+other=(?P<other>\d+)"
)

LOOPBACK = {"127.0.0.1", "::1", "localhost", "0.0.0.0", "::"}
LOG_GLOB = "phase1_log_*.txt"


# ---- rows ---------------------------------------------------------------------
class Row:
    __slots__ = ("check", "scope", "status", "expected", "actual", "lines")

    def __init__(self, check, scope, status, expected, actual, lines=None):
        self.check = check
        self.scope = scope
        self.status = status
        self.expected = expected
        self.actual = actual
        self.lines = lines or []

    def as_dict(self):
        return {
            "check": self.check,
            "scope": self.scope,
            "status": self.status,
            "expected": self.expected,
            "actual": self.actual,
            "line": self.lines[0] if self.lines else None,
        }


def kv(line, key, pattern=r"-?\d+(?:\.\d+)?"):
    """Value of `key=<value>` on one line, or None."""
    m = re.search(r"(?<![\w.-])" + re.escape(key) + r"=(" + pattern + r")", line)
    return m.group(1) if m else None


def to_int(text):
    try:
        return int(text)
    except (TypeError, ValueError):
        return None


def to_float(text):
    try:
        return float(text)
    except (TypeError, ValueError):
        return None


def parse_kr_row(body):
    """One `[KR]` payload -> dict, or None if a required field is missing.

    Key/value driven, not positional: IMPL added `comps=` (and may add more), and a
    rigid regex would drop every row the moment a field changed. Optional fields
    come back as None instead of killing the row.
    """
    found = {}
    for name, rx in KR_FIELDS.items():
        m = rx.search(body)
        if m:
            found[name] = m.group(1)
    if any(k not in found for k in KR_REQUIRED):
        return None
    return {
        "ent": found["ent"].upper(),
        "objId": int(found["objId"]) if "objId" in found else 0,
        "raw": int(found["raw"], 16),
        "kind": int(found["kind"]),
        "role": int(found["role"]),
        "bhv": int(found["bhv"], 16),
        "skel": int(found["skel"]),
        "comps": int(found["comps"]) if "comps" in found else None,
        "stage": int(found["stage"]) if "stage" in found else None,
        "d2": to_float(found.get("d2")),
        "lineno": 0,
    }


# ---- discovery ----------------------------------------------------------------
def find_logs(path):
    """One file, or every phase1_log_*.txt directly inside a folder."""
    if os.path.isfile(path):
        return [path]
    if not os.path.isdir(path):
        raise SystemExit(f"coop_report: no such file or folder: {path}")
    names = sorted(
        n for n in os.listdir(path)
        if fnmatch.fnmatch(n.lower(), LOG_GLOB) and os.path.isfile(os.path.join(path, n))
    )
    if not names:
        raise SystemExit(f"coop_report: no {LOG_GLOB} in {path}")
    return [os.path.join(path, n) for n in names]


# ---- one log ------------------------------------------------------------------
class Log:
    def __init__(self, path):
        self.path = path
        self.name = os.path.basename(path)
        self.excerpt = bool(EXCERPT_RE.search(self.name))
        m = PID_RE.search(self.name)
        self.pid = m.group(1) if m else None

        self.lines = 0
        self.tags = Counter()
        self.untagged = 0

        self.build = None          # dict(asi, commit, tree, lineno)
        self.build_absent = False
        self.build_lineno = None

        self.cfg = {}              # enabled/peer/local/peerPort/strictPeer
        self.up = {}               # bound/ip/port
        self.stats = {}            # tx_pkt/tx_bytes/tx_ent/rx_pkt/rx_bytes

        self.sync_binds = []       # dicts
        self.slot_binds = []
        self.slot_releases = Counter()
        self.slot_reasons = Counter()

        self.kt_local = 0
        self.kt_push = 0
        self.kt_resolve = 0
        self.kt_recv_applied = 0
        self.kt_recv_lost = 0
        self.kt_skip = Counter()
        self.gkt_detect = 0
        self.gkt_push = 0
        self.gkt_skip = Counter()

        # objIds per death lane, so a receive can be attributed to the lane that
        # pushed it instead of being counted for both (the T14(1) defect).
        self.kt_push_ids = set()        # host lane, onto the wire
        self.gkt_push_ids = set()       # guest lane, onto the wire
        self.kt_recv_ids = set()        # applied=1
        self.kt_recv_lost_ids = set()   # applied=0

        self.crashes = []          # (lineno, line)
        self.exc = []              # (lineno, line)
        self.kindrole = []         # [KR] rows (C21 live dump, not emitted yet)
        self.kindrole_total = None # last [Sync] kindrole aggregate (what 280D652169D0 emits)

        self.last_line = ""
        self.last_lineno = 0

        self._parse()

    def _parse(self):
        with open(self.path, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
        for lineno, raw in enumerate(text.splitlines(), 1):
            line = raw.rstrip("\r\n")
            self.lines = lineno
            if line.strip():
                self.last_line, self.last_lineno = line, lineno

            m = TAG_RE.match(line)
            if m:
                self.tags[m.group("tag")] += 1
            elif line.strip():
                self.untagged += 1

            # build: only the first one in the file matters (it is line 1 by design)
            if self.build is None and not self.build_absent:
                mb = BUILD_RE.search(line)
                if mb:
                    self.build = {
                        "asi": mb.group("asi").lower(),
                        "commit": mb.group("commit"),
                        "tree": mb.group("tree"),
                        "lineno": lineno,
                    }
                    self.build_lineno = lineno
                elif BUILD_UNAVAILABLE_RE.search(line):
                    self.build_absent = True
                    self.build_lineno = lineno

            # net link
            if CFG_RE.search(line):
                self.cfg = {
                    "enabled": to_int(kv(line, "enabled")),
                    "peer": (re.search(r"(?<![\w.-])peer=([0-9A-Za-z_.:\[\]-]+)", line) or [None, None])[1],
                    "local": to_int(kv(line, "local")),
                    "peerPort": to_int(kv(line, "peerPort")),
                    "strictPeer": to_int(kv(line, "strictPeer")),
                    "lineno": lineno,
                }
            mu = UP_RE.search(line)
            if mu and not self.up:
                self.up = {
                    "bound": int(mu.group("bound")),
                    "ip": mu.group("ip"),
                    "port": int(mu.group("port")),
                    "lineno": lineno,
                }
            if STATS_RE.search(line):
                # "[Net] stats tx=211 pkt, 118160 B, 0 ent  rx=211 pkt, 118160 B  | ..."
                mt = re.search(r"tx=(\d+)\s*pkt,?\s*(\d+)\s*B,\s*(\d+)\s*ent", line)
                mr = re.search(r"rx=(\d+)\s*pkt,?\s*(\d+)\s*B", line)
                if mt:
                    self.stats.update(tx_pkt=int(mt.group(1)), tx_bytes=int(mt.group(2)),
                                      tx_ent=int(mt.group(3)), lineno=lineno)
                if mr:
                    self.stats.update(rx_pkt=int(mr.group(1)), rx_bytes=int(mr.group(2)))

            # [Sync] bind
            ms = SYNC_BIND_RE.search(line)
            if ms:
                self.sync_binds.append({
                    "sent": int(ms.group("sent")),
                    "bound": int(ms.group("bound")),
                    "unmatched": int(ms.group("unmatched")),
                    "meanErr": to_float(ms.group("mean")),
                    "maxErr": to_float(ms.group("max")),
                    "lineno": lineno,
                    "line": line,
                })

            # [Slot]
            mb = SLOT_BIND_RE.search(line)
            if mb:
                self.slot_binds.append({
                    "slot": int(mb.group("slot")),
                    "objId": int(mb.group("objId")),
                    "ent": mb.group("ent").upper(),
                    "via": mb.group("via"),
                    "lineno": lineno,
                })
            mr_ = SLOT_REL_RE.search(line)
            if mr_:
                self.slot_releases[int(mr_.group("slot"))] += 1
                self.slot_reasons[mr_.group("reason")] += 1

            # death lanes
            if KT_LOCAL_RE.search(line):
                self.kt_local += 1
            mpush = KT_PUSH_RE.search(line)
            if mpush:
                self.kt_push += 1
                self.kt_push_ids.add(int(mpush.group("objId")))
            if KT_RESOLVE_RE.search(line):
                self.kt_resolve += 1
            mrecv = KT_RECV_RE.search(line)
            if mrecv:
                oid = int(mrecv.group("objId"))
                if mrecv.group("applied") == "1":
                    self.kt_recv_applied += 1
                    self.kt_recv_ids.add(oid)
                else:
                    self.kt_recv_lost += 1
                    self.kt_recv_lost_ids.add(oid)
            mskip = KT_SKIP_RE.search(line)
            if mskip:
                self.kt_skip[mskip.group("reason")] += 1
            if GKT_DETECT_RE.search(line):
                self.gkt_detect += 1
            mgpush = GKT_PUSH_RE.search(line)
            if mgpush:
                self.gkt_push += 1
                self.gkt_push_ids.add(int(mgpush.group("objId")))
            mskipg = GKT_SKIP_RE.search(line)
            if mskipg:
                self.gkt_skip[mskipg.group("reason")] += 1

            if CRASH_RE.search(line):
                self.crashes.append((lineno, line))
            elif EXC_RE.search(line):
                self.exc.append((lineno, line))

            mkr = KR_TAG_RE.search(line)
            if mkr:
                row = parse_kr_row(mkr.group("body"))
                if row is not None:
                    row["lineno"] = lineno
                    self.kindrole.append(row)
            mkt = KR_TOTAL_RE.search(line)
            if mkt:
                self.kindrole_total = {
                    "n": int(mkt.group("n")),
                    "soldier": int(mkt.group("soldier")),
                    "civilian": int(mkt.group("civilian")),
                    "other": int(mkt.group("other")),
                    "lineno": lineno,
                    "line": line,
                }

    # -- helpers
    @property
    def short_name(self):
        return self.name

    @property
    def build_short(self):
        if not self.build:
            return "absent"
        return "asi=" + self.build["asi"][:12]

    def top_tags(self, n=10):
        return self.tags.most_common(n)


# ---- checks: one log ----------------------------------------------------------
def check_build(log):
    if log.build_absent:
        return Row("build", log.short_name, FAIL,
                   "[Build] asi=<sha256> on line <= 3",
                   "[Build] unavailable (a build that cannot name itself is not evidence)",
                   [f"line {log.build_lineno}"])
    if not log.build:
        return Row("build", log.short_name, FAIL,
                   "[Build] asi=<sha256> on line <= 3",
                   "field absent: no [Build] line in the log")
    b = log.build
    ok_line = b["lineno"] <= 3
    detail = (f"asi={b['asi'][:12]}... commit={b['commit'] or '?'} "
              f"tree={b['tree'] or '?'} (line {b['lineno']})")
    if ok_line:
        return Row("build", log.short_name, PASS, "[Build] asi=<sha256> on line <= 3", detail,
                   [f"line {b['lineno']}"])
    return Row("build", log.short_name, FAIL, "[Build] asi=<sha256> on line <= 3",
               detail + " - not on line <= 3", [f"line {b['lineno']}"])


def check_net(log):
    """A link that exists, crosses a network, and is not a socket talking to itself.

    The last rule is the E0-A lesson recorded in ops/evidence/PARSER-SELFTEST.md:
    tx == rx together with peer=127.0.0.1 means one process looping on itself.
    """
    peer = log.cfg.get("peer") or log.up.get("ip")
    tx = log.stats.get("tx_pkt")
    rx = log.stats.get("rx_pkt")
    src = "[Net] cfg" if log.cfg else ("[Net] up" if log.up else None)

    if src is None and tx is None:
        return Row("net link", log.short_name, FAIL,
                   "link line ([Net] cfg or [Net] up) + [Net] stats",
                   "field absent: no link line and no [Net] stats")
    if peer is None:
        return Row("net link", log.short_name, FAIL, "peer=<ip>", "field absent: peer ip")
    if peer in LOOPBACK:
        return Row("net link", log.short_name, FAIL, "peer not loopback",
                   f"peer={peer} -> loopback: no packet crossed a network",
                   [f"line {log.cfg.get('lineno') or log.up.get('lineno')}"])
    if not tx or not rx:
        return Row("net link", log.short_name, FAIL, "tx > 0 and rx > 0",
                   f"tx={tx} rx={rx}",
                   [f"[Net] stats line {log.stats.get('lineno')}"])
    if tx == rx:
        return Row("net link", log.short_name, FAIL, "rx != tx (no self-count)",
                   f"tx={tx} rx={rx} -> equal: counters describe a self-loop",
                   [f"[Net] stats line {log.stats.get('lineno')}"])
    return Row("net link", log.short_name, PASS,
               "peer not loopback, tx>0, rx>0, rx!=tx",
               f"peer={peer} ({src}) tx={tx} rx={rx} "
               f"{log.stats.get('tx_bytes', 0)} B up",
               [f"line {log.cfg.get('lineno') or log.up.get('lineno')}",
                f"[Net] stats line {log.stats.get('lineno')}"])


def check_sync(log):
    if not log.sync_binds:
        return Row("sync bind", log.short_name, FAIL,
                   "[Sync] bind sent=/bound=/unmatched=/meanErr=",
                   "field absent: no [Sync] bind line (the C14 evidence line never appeared)")
    b = log.sync_binds[-1]
    detail = (f"sent={b['sent']} bound={b['bound']} unmatched={b['unmatched']} "
              f"meanErr={b['meanErr']:.2f} maxErr={b['maxErr']:.2f}"
              if b["maxErr"] is not None else
              f"sent={b['sent']} bound={b['bound']} unmatched={b['unmatched']} "
              f"meanErr={b['meanErr']:.2f}")
    if b["sent"] == 0:
        return Row("sync bind", log.short_name, FAIL, "sent > 0",
                   detail + " - nothing was ever bound", [f"line {b['lineno']}"])
    if b["sent"] != b["bound"] + b["unmatched"]:
        return Row("sync bind", log.short_name, FAIL, "sent == bound + unmatched",
                   detail + f" - {b['sent']} != {b['bound']}+{b['unmatched']}",
                   [f"line {b['lineno']}", b["line"]])
    if b["bound"] == 0:
        return Row("sync bind", log.short_name, FAIL, "bound > 0",
                   detail + " - every id went unmatched", [f"line {b['lineno']}"])
    return Row("sync bind", log.short_name, PASS,
               "present, sent>0, bound>0, sent == bound + unmatched",
               detail, [f"line {b['lineno']}", b["line"]])


def check_slot(log):
    if not log.slot_binds:
        return Row("slot binds", log.short_name, FAIL,
                   "[Slot] bind slot=/objId=/ent=/via=",
                   "field absent: no [Slot] bind line")
    binds = log.slot_binds
    slots = {b["slot"] for b in binds}
    ids = {b["objId"] for b in binds}
    ent = {b["ent"] for b in binds}
    vias = Counter(b["via"] for b in binds)
    relaunched = len(binds) - len(slots)
    detail = (f"binds={len(binds)} slots={len(slots)} objIds={len(ids)} ents={len(ent)} "
              f"via={dict(vias)} rebinds={relaunched}")
    if log.slot_reasons:
        detail += " releases=" + ",".join(f"{k}:{v}" for k, v in sorted(log.slot_reasons.items()))
    if relaunched > 0:
        # not a failure by itself (a slot can legitimately be rebound), but it is
        # the symptom the id-table work is trying to remove, so say so.
        return Row("slot binds", log.short_name, INFO,
                   "one bind per slot", detail + " - slots were rebound", 
                   [f"line {binds[-1]['lineno']}"])
    return Row("slot binds", log.short_name, PASS, ">= 1 bind, one bind per slot",
               detail, [f"line {binds[-1]['lineno']}"])


def check_crash(log):
    if log.crashes:
        first = log.crashes[0]
        return Row("crashes", log.short_name, FAIL, "0 crash markers",
                   f"{len(log.crashes)} crash marker(s), first at line {first[0]}: {first[1][:90]}",
                   [f"line {n}: {t[:110]}" for n, t in log.crashes[:5]])
    extra = ""
    if log.exc:
        extra = (f"; {len(log.exc)} swallowed-EXC line(s) (not crash markers by the rule, "
                 f"first line {log.exc[0][0]})")
    return Row("crashes", log.short_name, PASS, "0 crash markers",
               f"none in {log.lines} lines{extra}")


# ---- checks: the run as a whole ----------------------------------------------
def check_kindrole(logs):
    """C21: the +0x9C discriminator, per enumerated NPC.

    Only decidable once a build emits `[KR]` (ops/QA-T14-C21-probe.md). Absence is
    INFO, never FAIL, so this cannot penalise the runs that predate the emit.

    What is actually decidable, and why the literal request is not:
      * the bit packing is checkable against raw (kind = raw&7, role = (raw>>3)&0x3F);
      * the +0x9C verdict is checkable AGAINST AN INDEPENDENT SIGNAL on the same
        body (skeleton + BhvGenericNPC from the component walk), which is a real
        cross-validation rather than a restatement;
      * "attacker" is only observable as stage == 4 (HumanGroundStage.Fight);
        "fleer" is NOT observable in any field we log - there is no flee state in
        the log, so that half of the criterion cannot be machine-decided yet.
    """
    per_body = [r for log in logs for r in log.kindrole]
    totals = [(log, log.kindrole_total) for log in logs if log.kindrole_total]

    # ---- what the DEPLOYED build emits: four summed counters -------------------
    out = []
    for log, t in totals:
        total = t["soldier"] + t["civilian"] + t["other"]
        pct = (100.0 * t["soldier"] / t["n"]) if t["n"] else 0.0
        if t["n"] == 0:
            out.append(Row("kindrole live", log.short_name, FAIL,
                           "n > 0 (the +0x9C read produced a population)",
                           "n=0: no body was classified",
                           [f"line {t['lineno']}", t["line"]]))
        elif total != t["n"]:
            out.append(Row("kindrole live", log.short_name, FAIL,
                           "soldier + civilian + other == n",
                           f"{t['soldier']}+{t['civilian']}+{t['other']} != {t['n']}",
                           [f"line {t['lineno']}", t["line"]]))
        else:
            out.append(Row("kindrole live", log.short_name, PASS,
                           "n>0 and soldier+civilian+other == n",
                           f"n={t['n']} soldier={t['soldier']} ({pct:.0f}%) "
                           f"civilian={t['civilian']} other={t['other']}",
                           [f"line {t['lineno']}", t["line"]]))
            if t["soldier"] == 0 or t["civilian"] == 0:
                out.append(Row("kindrole coverage", log.short_name, INFO,
                               "both classes present in the scene",
                               "only one class observed: this scene does not exercise "
                               "the split"))

    if not per_body:
        if not out:
            return [Row("kindrole (C21)", "all", INFO,
                        "one [KR] line per enumerated NPC, or a [Sync] kindrole line",
                        "field absent: no kindrole output in this run")]
        out.append(Row("kindrole tuples", "all", INFO,
                       "per-body kind/role, so (3,1) and (2,2) can be checked",
                       "NOT DECIDABLE from the aggregate line - it prints four sums and "
                       "no kind/role, so it cannot show a soldier IS (3,1) or a civilian "
                       "IS (2,2); that needs the [KR] emit (ops/QA-T14-C21-probe.md)"))
        return out

    rows = per_body
    read_failed = [r for r in rows if r["kind"] < 0 or r["role"] < 0]
    if read_failed:
        f = read_failed[0]
        out.append(Row("kindrole read", "all", FAIL,
                       "kind >= 0 and role >= 0 on every row",
                       f"{len(read_failed)} of {len(rows)} rows failed to read entity+0x9C "
                       f"(raw={f['raw']:08X} kind={f['kind']} role={f['role']} ent={f['ent']})",
                       [f"line {f['lineno']}"]))
    rows = [r for r in rows if r["kind"] >= 0 and r["role"] >= 0]

    bad_pack = [r for r in rows if r["raw"] & ~KR_PACK_MASK]
    bad_field = [r for r in rows
                 if r["kind"] != (r["raw"] & 0x7) or r["role"] != ((r["raw"] >> 3) & 0x3F)]
    if bad_pack:
        out.append(Row("kindrole packing", "all", FAIL,
                       "raw has no bits above bit 8",
                       f"{len(bad_pack)} of {len(rows)} rows set bits outside 0x1FF "
                       f"(first ent={bad_pack[0]['ent']} raw={bad_pack[0]['raw']:08X})",
                       [f"line {bad_pack[0]['lineno']}"]))
    elif bad_field:
        out.append(Row("kindrole packing", "all", FAIL,
                       "kind == raw&7 and role == (raw>>3)&0x3F",
                       f"{len(bad_field)} of {len(rows)} rows disagree with raw",
                       [f"line {bad_field[0]['lineno']}"]))
    else:
        out.append(Row("kindrole packing", "all", PASS,
                       "kind == raw&7 and role == (raw>>3)&0x3F",
                       f"{len(rows)} rows, all consistent"))

    humans = [r for r in rows if r["skel"] == 1 and r["bhv"] == KR_BHV_GENERIC_NPC]
    odd = [r for r in humans if r["kind"] not in (2, 3)]
    if not humans:
        out.append(Row("kindrole vs components", "all", INFO,
                       "every BhvGenericNPC human is kind 2 or 3",
                       "no skeleton+generic-NPC body in this run"))
    elif odd:
        out.append(Row("kindrole vs components", "all", FAIL,
                       "every BhvGenericNPC human is kind 2 or 3",
                       f"{len(odd)} of {len(humans)} humans have kind outside {{2,3}}: "
                       + ", ".join(f"ent={r['ent']} k={r['kind']} r={r['role']}" for r in odd[:3]),
                       [f"line {odd[0]['lineno']}"]))
    else:
        out.append(Row("kindrole vs components", "all", PASS,
                       "every BhvGenericNPC human is kind 2 or 3",
                       f"{len(humans)} humans, all kind in {{2,3}}"))

    pairs = Counter((r["kind"], r["role"]) for r in rows)
    split = ", ".join(f"({k},{v}):{n}" for (k, v), n in pairs.most_common(6))
    uniq = {r["ent"] for r in rows}
    comps = [r["comps"] for r in rows if r["comps"] is not None]
    extra = ""
    if comps:
        extra = f", comps {min(comps)}..{max(comps)}"
    out.append(Row("kindrole split", "all", INFO, "reported, not scored",
                   f"{len(rows)} rows, {len(uniq)} distinct bodies{extra}, pairs {split}"))

    if not any(r["stage"] is not None for r in rows):
        out.append(Row("kindrole attackers", "all", INFO,
                       "every stage=Fight body is (3,1)",
                       "the shipped [KR] line carries no stage=, so a fighting body cannot "
                       "be identified per row; criterion not evaluable from this emit"))
    else:
        fighting = [r for r in rows if r["stage"] == KR_STAGE_FIGHT]
        if not fighting:
            out.append(Row("kindrole attackers", "all", INFO,
                           "every stage=Fight body is (3,1)",
                           "no body in stage Fight(=4) in this run - scene produced no fight"))
        else:
            good = [r for r in fighting if r["kind"] == 3 and r["role"] == 1]
            other = [r for r in fighting if not (r["kind"] == 3 and r["role"] == 1)]
            out.append(Row("kindrole attackers", "all", PASS if not other else FAIL,
                           "every stage=Fight body is (3,1)",
                           f"{len(good)}/{len(fighting)} fighting bodies are (3,1)"
                           + ("" if not other else "; others: "
                              + ", ".join(f"({r['kind']},{r['role']})" for r in other[:4]))))
    out.append(Row("kindrole fleers", "all", INFO,
                   "every fleeing body is (2,2)",
                   "NOT MEASURABLE: no field in the log records fleeing; the criterion "
                   "needs a flee signal before it can be scored"))
    return out


def check_build_agreement(logs):
    hashes = {log.build["asi"] for log in logs if log.build}
    missing = [log.name for log in logs if not log.build]
    if missing:
        return Row("build agree", "all", FAIL, "every log names its asi",
                   "no [Build] asi in: " + ", ".join(missing))
    if len(hashes) == 1:
        return Row("build agree", "all", PASS, "one asi across the run",
                   next(iter(hashes))[:12] + "...")
    return Row("build agree", "all", FAIL, "one asi across the run",
               "mixed builds: " + ", ".join(sorted(h[:12] for h in hashes)))


def check_deaths(logs):
    """Deaths in both directions, joined per lane on objId.

    The log carries no role marker, so the direction is taken from the tag:
      host -> guest : [KT] death push        -> receiver logs [KT] death recv applied=1
      guest -> host : [GKT] guest death push -> receiver logs [KT] death recv applied=1

    A `[KT] death recv` proves nothing about which lane it belongs to on its own,
    so it is joined to a lane by objId. Counting receives globally and spending
    that total on both lanes let one direction's receive cover the other's loss -
    that is the T14(1) defect this join removes.

    A push with no matching receive is the C6 signature (the death reached the
    wire and nothing applied it); no push at all just means no death was tried.
    Counts are a sample: the lines are rate-limited to 10 per gate per process.
    """
    def union(attr):
        out = set()
        for log in logs:
            out |= getattr(log, attr)
        return out

    host_ids = union("kt_push_ids")
    guest_ids = union("gkt_push_ids")
    applied_ids = union("kt_recv_ids")
    lost_ids = union("kt_recv_lost_ids")
    host_detect = sum(l.kt_local for l in logs)
    guest_detect = sum(l.gkt_detect for l in logs)
    host_push = sum(l.kt_push for l in logs)
    guest_push = sum(l.gkt_push for l in logs)
    recv_ok = sum(l.kt_recv_applied for l in logs)
    recv_lost = sum(l.kt_recv_lost for l in logs)

    rows = []

    lanes = (
        ("host->guest", host_ids, host_push, host_detect, "[KT]"),
        ("guest->host", guest_ids, guest_push, guest_detect, "[GKT]"),
    )
    for lane, ids, pushed, detected, tag in lanes:
        named = "death " + lane
        expected = f">=1 {tag} death push and >=1 [KT] death recv applied=1 for an objId it pushed"
        matched = sorted((ids - {0}) & applied_ids)
        zero_only = bool(ids) and not (ids - {0})

        if not ids:
            if detected == 0:
                rows.append(Row(named, "all", FAIL, expected,
                                "no death attempted on this lane (GATES M2 needs 20)"))
            else:
                rows.append(Row(named, "all", FAIL, expected,
                                f"detected={detected} but pushed=0 - detection never reached the wire"))
        elif matched:
            rows.append(Row(named, "all", PASS, expected,
                            f"push={pushed} objIds={len(ids)} matched applied={len(matched)} "
                            f"({' '.join(str(m) for m in matched[:3])}), recv applied total={recv_ok}"))
        elif zero_only and recv_ok > 0:
            rows.append(Row(named, "all", PASS, expected,
                            f"push={pushed} all with objId=0: unjoinable, credited by count only "
                            f"(recv applied={recv_ok})"))
        elif applied_ids:
            rows.append(Row(named, "all", FAIL, expected,
                            f"push={pushed} objIds={sorted(ids)[:3]} but applied objIds="
                            f"{sorted(applied_ids)[:3]} - NO OVERLAP: the death was applied to "
                            f"something else"))
        else:
            rows.append(Row(named, "all", FAIL, expected,
                            f"push={pushed} recv applied=0 - PUSHED BUT NEVER APPLIED (C6 signature)"))

    loss = f"skipped: host={sum(sum(l.kt_skip.values()) for l in logs)} " \
           f"guest={sum(sum(l.gkt_skip.values()) for l in logs)}"
    skip_reasons = Counter()
    for log in logs:
        skip_reasons.update(log.kt_skip)
        skip_reasons.update(log.gkt_skip)
    if skip_reasons:
        loss += " (" + ", ".join(f"{k}:{v}" for k, v in sorted(skip_reasons.items())) + ")"
    loss += f"; recv applied=0: {recv_lost}"
    rows.append(Row("death losses", "all", INFO, "reported, not scored", loss))

    orphans = sorted(applied_ids - host_ids - guest_ids)
    if orphans:
        rows.append(Row("death orphans", "all", INFO, "every applied objId was pushed by a lane",
                        f"{len(orphans)} applied objId(s) no lane pushed: "
                        f"{' '.join(str(o) for o in orphans[:3])}"))

    capped = [gate for gate, n in (("KT_PUSH", host_push), ("KT_RECV", recv_ok + recv_lost),
                                  ("GKT_PUSH", guest_push), ("KT_LOCAL", host_detect),
                                  ("GKT_LOCAL", guest_detect)) if n >= 10]
    if capped:
        rows.append(Row("death rate-cap", "all", INFO, "counts are a sample, not a total",
                        "at 10-line budget: " + ", ".join(capped) +
                        " (KtOn caps each gate at 10 lines per process, phase1.cpp:2497)"))
    return rows


# ---- rendering ----------------------------------------------------------------
def print_rows(rows, title=None):
    if title:
        print(title)
    if not rows:
        return
    w_check = max(12, max(len(r.check) for r in rows))
    w_scope = max(5, min(28, max(len(r.scope) for r in rows)))
    print(f"{'check':<{w_check}}  {'scope':<{w_scope}}  {'status':<6}  expected -> actual")
    print(f"{'-' * w_check}  {'-' * w_scope}  {'-' * 6}  {'-' * 20}")
    for r in rows:
        print(f"{r.check:<{w_check}}  {r.scope[:w_scope]:<{w_scope}}  {r.status:<6}  "
              f"{r.expected} -> {r.actual}")
    fails = [r for r in rows if r.status == FAIL]
    if fails:
        print()
        print("failing checks:")
        for r in fails:
            print(f"FAIL {r.check} [{r.scope}]")
            print(f"  expected: {r.expected}")
            print(f"  actual  : {r.actual}")
            for ln in r.lines:
                print(f"  line    : {str(ln)[:160]}")


def print_tags(log, n=10):
    top = log.top_tags(n)
    if not top:
        print("  (no tagged lines)")
        return
    for tag, count in top:
        print(f"  {count:>8}  [{tag}]")
    if log.untagged:
        print(f"  {log.untagged:>8}  (untagged)")


# ---- report -------------------------------------------------------------------
def build_report(logs, expect_hash=None, kindrole=False):
    per_log_rows = []
    set_rows = []

    for log in logs:
        tag = "" if not log.excerpt else " [EXCERPT - not a run]"
        print()
        print(f"== {log.name}{tag}  ({log.lines} lines"
              + (f", pid {log.pid}" if log.pid else "") + ") ==")
        if log.excerpt:
            row = Row("excerpt", log.name, INFO, "a full run",
                      "filename says head+tail/truncated: scored checks suppressed")
            set_rows.append(row)
            rows = [row]
        else:
            rows = [check_build(log), check_net(log), check_sync(log),
                    check_slot(log), check_crash(log)]
        per_log_rows.extend(rows)
        print_rows(rows)
        print("  top tags:")
        print_tags(log)
        if log.last_line:
            print(f"  last line {log.last_lineno}: {log.last_line[:120]}")

    if not all(log.excerpt for log in logs):
        scoring = [log for log in logs if not log.excerpt]
        print()
        print(f"== run summary ({len(scoring)} log(s)) ==")
        rows = [check_build_agreement(scoring)] + check_deaths(scoring)
        if kindrole:
            rows += check_kindrole(scoring)
        if expect_hash:
            want = expect_hash.strip().lower()
            got = sorted({log.build["asi"] for log in scoring if log.build})
            hit = any(g.startswith(want) for g in got)
            rows.append(Row("expect-hash", "all", PASS if hit else FAIL,
                            f"asi starts with {want}",
                            ("all logs: " + ", ".join(g[:12] for g in got)) if hit
                            else ("seen: " + (", ".join(g[:12] for g in got) or "none"))))
        set_rows.extend(rows)
        print_rows(rows)

        if len(scoring) > 1:
            merged = Counter()
            for log in scoring:
                merged.update(log.tags)
            print()
            print("== top tags, all logs merged ==")
            total = sum(merged.values())
            for tag, count in merged.most_common(10):
                share = 100.0 * count / total if total else 0.0
                print(f"  {count:>8}  {share:5.1f}%  [{tag}]")

    all_rows = per_log_rows + set_rows
    fails = [r for r in all_rows if r.status == FAIL]
    passes = [r for r in all_rows if r.status == PASS]
    infos = [r for r in all_rows if r.status == INFO]
    verdict = "FAIL" if fails else "PASS"
    print()
    print(f"verdict: {verdict}  ({len(passes)} pass, {len(fails)} fail, {len(infos)} info)")
    return 0 if not fails else 1, all_rows


# ---- selftest -----------------------------------------------------------------
GOOD = """\
[Build] asi=5e0f71103313dfdaf9de7b6548154f03c7c37c56baf692ffaa6a256c425e68ec commit=f063ea1 tree=clean
[Phase1] DriveContext=0 ctxMask=0x440
[Net] cfg enabled=1 peer=192.168.1.50 local=47801 peerPort=47802 strictPeer=1
[Sync] scope candidates=40 inRadius=12 enemies=8 sent=6 (SyncEnemiesOnly=0, SyncRadiusM=120, SyncMaxEntities=32)
[Sync] bind sent=12 bound=11 unmatched=1 meanErr=0.14 maxErr=0.62
[Slot] bind slot=0 objId=20143895943 -> ent=3540D6E0 via=fresh
[Slot] bind slot=1 objId=35677645477 -> ent=35DCF580 via=sticky
[KT] local death detected objId=20143895943 ent=3540D6E0 hp=88->65535
[KT] death push objId=20143895943 seq=1234
[KT] death recv objId=20143895943 -> ent=35DCF580 ok=1 applied=1
[GKT] guest death detected objId=35677645477 ent=35DCF580 life=90->65535
[GKT] guest death push objId=35677645477 seq=99 ent=35DCF580
[KT] death recv objId=35677645477 -> ent=3540D6E0 ok=1 applied=1
[Net] stats tx=211 pkt, 118160 B, 6 ent  rx=207 pkt, 115000 B  |  15.6 kB/s up, 15.6 kB/s down  |  mirror ok=0 nobod=0 steal=0
"""


def _fixture(tmp, name, body):
    path = os.path.join(tmp, name)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(body)
    return path


def _status(rows, check):
    for r in rows:
        if r.check == check:
            return r.status
    return None


def selftest():
    import io
    from contextlib import redirect_stdout

    # (label, check, expected status of THAT check, body). check=None means the
    # whole verdict must be PASS with no failing row.
    cases = []

    cases.append(("good run, both lanes", None, PASS, GOOD))
    cases.append(("sync invariant broken", "sync bind", FAIL,
                  GOOD.replace("sent=12 bound=11 unmatched=1", "sent=12 bound=10 unmatched=1")))
    cases.append(("crash marker", "crashes", FAIL,
                  GOOD + "[Net] unhandled exception 0xC0000005 at 0x0067B2EA\n"))
    cases.append(("loopback self-loop", "net link", FAIL,
                  GOOD.replace("peer=192.168.1.50", "peer=127.0.0.1")
                      .replace("tx=211 pkt, 118160 B, 6 ent  rx=207 pkt, 115000 B",
                               "tx=211 pkt, 118160 B, 6 ent  rx=211 pkt, 118160 B")))
    cases.append(("no C14 evidence at all", "sync bind", FAIL,
                  "[Build] asi=5e0f71103313dfdaf9de7b6548154f03c7c37c56baf692ffaa6a256c425e68ec commit=f063ea1 tree=clean\n"
                  "[Id] idx=0 objId=4026532617 ent=34F8B990 evt=01E4CE90 bhv=00000000 arch=0000 pos=(0.00,0.00,0.00)\n"
                  "[Id] --- 1 humans logged ---\n"))
    cases.append(("build line missing", "build", FAIL,
                  GOOD.split("\n", 1)[1]))

    # --- T14(1) regression pair: one lane loses its death, the other does not.
    host_lost = GOOD.replace(
        "[KT] death recv objId=20143895943 -> ent=35DCF580 ok=1 applied=1",
        "[KT] death recv objId=20143895943 -> ent=35DCF580 ok=0 applied=0")
    cases.append(("host death never applied", "death host->guest", FAIL, host_lost))
    cases.append(("...guest lane not credited for it", "death guest->host", PASS, host_lost))
    guest_lost = GOOD.replace(
        "[KT] death recv objId=35677645477 -> ent=3540D6E0 ok=1 applied=1",
        "[KT] death recv objId=35677645477 -> ent=3540D6E0 ok=0 applied=0")
    cases.append(("guest death never applied", "death guest->host", FAIL, guest_lost))
    cases.append(("...host lane not credited for it", "death host->guest", PASS, guest_lost))
    cases.append(("no death in the run", "death host->guest", FAIL,
                  GOOD.replace("[KT] local death detected objId=20143895943 ent=3540D6E0 hp=88->65535\n", "")
                      .replace("[KT] death push objId=20143895943 seq=1234\n", "")))

    cases.append(("excerpt file is not scored", None, PASS,
                  "[Build] asi=5e0f71103313dfdaf9de7b6548154f03c7c37c56baf692ffaa6a256c425e68ec commit=f063ea1 tree=clean\n"
                  "[Id] idx=0 objId=1 ent=34F8B990\n"))

    # --- C21 discriminator, in the format REL-80f9a9a7e588 actually emits ------
    cases.append(("C21 aggregate, arithmetic ok", "kindrole live", PASS,
                  GOOD + "[Sync] kindrole n=18 soldier=6 civilian=10 other=2\n"))
    cases.append(("C21 aggregate, classes do not sum", "kindrole live", FAIL,
                  GOOD + "[Sync] kindrole n=18 soldier=6 civilian=10 other=1\n"))
    cases.append(("C21 aggregate, nothing classified", "kindrole live", FAIL,
                  GOOD + "[Sync] kindrole n=0 soldier=0 civilian=0 other=0\n"))
    cases.append(("C21 row, human kind=5", "kindrole vs components", FAIL,
                  GOOD + "[KR] ent=34F8B990 objId=123 raw=0000001D kind=5 role=3 bhv=026E34D8 skel=1 comps=14\n"))
    cases.append(("C21 row, raw bits above 8", "kindrole packing", FAIL,
                  GOOD + "[KR] ent=34F8B990 objId=123 raw=0000021D kind=5 role=3 bhv=026E34D8 skel=1 comps=14\n"))
    cases.append(("C21 row, consistent soldier+civilian", "kindrole packing", PASS,
                  GOOD + "[KR] ent=34F8B990 objId=123 raw=0000000B kind=3 role=1 bhv=026E34D8 skel=1 comps=14\n"
                  "[KR] ent=35DCF580 objId=456 raw=00000012 kind=2 role=2 bhv=026E34D8 skel=1 comps=9\n"))
    cases.append(("C21 row without comps= still parses", "kindrole packing", PASS,
                  GOOD + "[KR] ent=34F8B990 objId=123 raw=0000000B kind=3 role=1 bhv=026E34D8 skel=1\n"))
    cases.append(("C21 row, +0x9C read failed", "kindrole read", FAIL,
                  GOOD + "[KR] ent=34F8B990 objId=123 raw=00000000 kind=-1 role=-1 bhv=00000000 skel=1 comps=14\n"))

    print("coop_report.py selftest - synthetic fixtures, stdlib only")
    print()
    header = f"{'case':<34}{'check':<20}{'expect':<8}{'got':<8}result"
    print(header)
    print(f"{'-' * 34}{'-' * 20}{'-' * 8}{'-' * 8}{'-' * 6}")

    ok_all = True
    tmp = tempfile.mkdtemp(prefix="coop_report_selftest_")
    try:
        for label, check, expected, body in cases:
            name = ("phase1_log_27376_head+tail.txt" if label.startswith("excerpt")
                    else "phase1_log_9001.txt")
            path = _fixture(tmp, name, body)
            buf = io.StringIO()
            with redirect_stdout(buf):
                rc, rows = build_report([Log(path)], kindrole=True)
            if check is None:
                fails = [r for r in rows if r.status == FAIL]
                ok = (rc == 0 and not fails)
                want, got = PASS, (PASS if ok else FAIL)
                shown = "verdict"
            else:
                got = _status(rows, check) or "MISSING"
                # a named FAIL must also move the verdict; a named PASS may sit
                # next to other failures and still be correct
                ok = (got == expected) and (rc != 0 if expected == FAIL else True)
                want = expected
                shown = check
            ok_all = ok_all and ok
            print(f"{label:<34}{shown:<20}{want:<8}{got:<8}{'ok' if ok else 'WRONG'}")
    finally:
        for f in os.listdir(tmp):
            try:
                os.remove(os.path.join(tmp, f))
            except OSError:
                pass
        try:
            os.rmdir(tmp)
        except OSError:
            pass

    print()
    print(f"selftest: {'OK' if ok_all else 'FAILED'}")
    return 0 if ok_all else 1


# ---- cli ----------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="coop_report.py",
        description="Pass/fail a co-op run from phase1_log_*.txt alone.",
    )
    ap.add_argument("target", nargs="?",
                    help="a phase1_log_*.txt file, or a folder containing them")
    ap.add_argument("--expect-hash", metavar="HASH",
                    help="require every log's [Build] asi to start with this (e.g. the deployed build)")
    ap.add_argument("--json", action="store_true", help="machine output instead of the table")
    ap.add_argument("--kindrole", action="store_true",
                    help="also score the C21 [KR] discriminator dump (absent = INFO, not FAIL)")
    ap.add_argument("--selftest", action="store_true", help="run the offline fixture cases")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()
    if not args.target:
        ap.error("give a log, a folder, or --selftest")

    try:
        paths = find_logs(args.target)
    except SystemExit as e:
        print(e, file=sys.stderr)
        return 2

    logs = []
    for p in paths:
        try:
            logs.append(Log(p))
        except OSError as e:
            print(f"coop_report: cannot read {p}: {e}", file=sys.stderr)
            return 2

    if args.json:
        import io
        from contextlib import redirect_stdout
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc, rows = build_report(logs, args.expect_hash, args.kindrole)
        print(json.dumps({
            "verdict": "FAIL" if rc else "PASS",
            "logs": [{"file": l.name, "lines": l.lines, "pid": l.pid,
                      "excerpt": l.excerpt, "asi": l.build["asi"] if l.build else None,
                      "tags": dict(l.tags.most_common(10))} for l in logs],
            "checks": [r.as_dict() for r in rows],
        }, indent=2))
        return rc

    rc, _ = build_report(logs, args.expect_hash, args.kindrole)
    return rc


if __name__ == "__main__":
    sys.exit(main())
