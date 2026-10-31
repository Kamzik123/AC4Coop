#!/usr/bin/env python3
"""Mechanical gate parser for the co-op programme (M1, M2).

    python ops/tools/gate_parse.py M1 --a <logA> --b <logB> --out ops/evidence/M1-<date>
    python ops/tools/gate_parse.py M2 --a <logA> --b <logB> --kills <kills.tsv> --out ops/evidence/M2-<date>
    python ops/tools/gate_parse.py --selftest

What this is
------------
`gate.py` checks that the evidence *files exist*; it never reads the numbers in
them. This tool is the other half: it reads the two `phase1_log_<pid>.txt` files,
decides every numeric check in GATES.md for M1 (and M2 from a `kills.tsv`), and
writes the verdict. It never touches STATE.json, never flips a gate, and never
edits a log.

Rules
-----
* A missing field is a FAILED check whose detail is `field absent: <what>`. It is
  never skipped, defaulted to zero, or guessed.
* Every check row carries the exact log line that decided it, so a human can
  disagree with the code and see why.
* Absolute paths in the output are relative to the log directory where possible.

Log format actually on disk (verified against phase1_log_5844/7172/4196)
-----------------------------------------------------------------------
    [Net] cfg enabled=1 peer=127.0.0.1 local=47801 peerPort=47801 strictPeer=1
    [Net] up: bound:48001 -> 192.168.1.11:48002     # NOT present in current logs
    [Net] stats tx=129 pkt, 119765 B, 1740 ent  rx=129 pkt, 119765 B  | ...
    [Net] apply seq=...                              # counted
    [Net] action -> 18981193 ch=0 ly=0 ...           # counted
    [Smooth] frames=81 hold=0 ext=0 clamp=0 eqT=0 drop=0 wipe=0 extra=27 avgD=0.0129 maxD=0.0304
    [Build] asi=<sha256>                             # NOT present in current logs

Bound port precedence: `[Net] up: bound:<p>` when the build prints it, otherwise
`local=<p>` from `[Net] cfg`. The source used is recorded next to the number, and
both are reported when they disagree.

Duration: there are no timestamps and no heartbeat lines in the log body, so the
duration is the file span (mtime - ctime) and is labelled as such. On Windows
`st_ctime` is the creation time; on POSIX it is the inode change time, so a
duration read on POSIX is only meaningful for a file written once.

A `[Net] stats` counter is cumulative: the last line is the session total.
"""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import math
import os
import re
import shutil
import statistics
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
OPS = os.path.dirname(HERE)
ROOT = os.path.dirname(OPS)

MAXD_LIMIT = 2.0          # GATES.md M1 #5: max <= 2.0 m
MIN_DURATION_M1 = 600.0   # GATES.md M1 #6: 10 min
MIN_DURATION_M2 = 900.0   # GATES.md M2 #7: 15 min
KILL_TARGET = 20          # GATES.md M2 #1
LATENCY_LIMIT_MS = 200.0  # GATES.md M2 #4

LOOPBACK = {"127.0.0.1", "::1", "localhost", "0.0.0.0", "::"}

# Deliberately narrow: `[WB] exception cap reached` is a known benign line and
# must not read as a crash.
CRASH_RE = re.compile(
    r"\bcrash(?:ed|ing)?\b|\[crash\]|\bfatal\b|unhandled exception|"
    r"access_violation|0xc0000005|assert(?:ion)? failed|abort(?:ed)?\b",
    re.IGNORECASE,
)

CFG_RE = re.compile(r"\[Net\]\s+cfg\b")
UP_RE = re.compile(
    r"\[Net\]\s+up:\s*bound:(?P<bound>\d+)\s*(?:->|=>|\u2192)\s*"
    r"(?P<ip>[0-9A-Za-z_.:\[\]-]+):(?P<port>\d+)"
)
STATS_RE = re.compile(r"\[Net\]\s+stats\b")
SMOOTH_RE = re.compile(r"\[Smooth\]\s")
BUILD_RE = re.compile(r"\[Build\]\s+asi=(?P<asi>[0-9a-fA-F]{8,64})")
APPLY_RE = re.compile(r"\[Net\]\s+apply\b")
ACTION_RE = re.compile(r"\[Net\]\s+action\s*->")
PID_RE = re.compile(r"phase1_log_(\d+)\.txt$", re.IGNORECASE)

NUM = r"-?\d+(?:\.\d+)?"


def kv(line, key):
    """Value of `key=<number>` on one line, or None."""
    m = re.search(r"(?<![\w.-])" + re.escape(key) + r"=(" + NUM + r")", line)
    return m.group(1) if m else None


def as_float(value):
    if value is None:
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


class LogFacts(object):
    """Everything the gate needs from one plugin log."""

    def __init__(self, path):
        self.path = path
        self.name = os.path.basename(path)
        self.pid = None
        self.pid_source = None
        self.cfg_line = None
        self.peer = None
        self.local_port = None
        self.peer_port = None
        self.up_line = None
        self.up_bound = None
        self.up_peer = None
        self.up_peer_port = None
        self.stats = []          # list of (tx, rx) in file order
        self.stats_line = None
        self.stats_lineno = None
        self.applies = 0
        self.actions = 0
        self.smooth = []         # list of dicts
        self.smooth_line = None
        self.build_asi = None
        self.crash = None        # (lineno, text)
        self.duration_s = None
        self.duration_source = "field absent"
        self.lines = 0
        self.read_error = None
        self._parse()

    # -- helpers -----------------------------------------------------------
    @property
    def tx(self):
        return self.stats[-1][0] if self.stats else None

    @property
    def rx(self):
        return self.stats[-1][1] if self.stats else None

    @property
    def port(self):
        if self.up_bound is not None:
            return self.up_bound
        return self.local_port

    @property
    def port_source(self):
        if self.up_bound is not None:
            return "[Net] up: bound="
        if self.local_port is not None:
            return "[Net] cfg local="
        return "field absent"

    def smooth_max(self, key):
        vals = [r[key] for r in self.smooth if key in r and r[key] is not None]
        return max(vals) if vals else None

    def smooth_argmax(self, key):
        """The [Smooth] record that carries the largest value for `key`."""
        best = None
        for r in self.smooth:
            v = r.get(key)
            if v is None:
                continue
            if best is None or v > best[key]:
                best = r
        return best

    # -- parsing -----------------------------------------------------------
    def _parse(self):
        try:
            with open(self.path, "r", encoding="utf-8", errors="replace") as f:
                text = f.read()
        except OSError as exc:
            self.read_error = str(exc)
            return

        m = PID_RE.search(self.name)
        if m:
            self.pid = int(m.group(1))
            self.pid_source = "filename"
        else:
            m = re.search(r"(\d+)", os.path.splitext(self.name)[0])
            if m:
                self.pid = int(m.group(1))
                self.pid_source = "filename (last number)"

        for lineno, raw in enumerate(text.splitlines(), 1):
            line = raw.rstrip("\r\n")
            self.lines += 1

            if CRASH_RE.search(line) and self.crash is None:
                self.crash = (lineno, line)

            if self.build_asi is None:
                m = BUILD_RE.search(line)
                if m:
                    self.build_asi = m.group("asi").lower()

            if CFG_RE.search(line):
                if self.cfg_line is None:
                    self.cfg_line = (lineno, line)
                # peer is an address, not a number: `kv()` would truncate 127.0.0.1
                pm = re.search(r"(?<![\w.-])peer=([^\s,]+)", line)
                if pm:
                    self.peer = pm.group(1)
                lp = kv(line, "local")
                if lp is not None:
                    self.local_port = int(float(lp))
                pp = kv(line, "peerPort")
                if pp is not None:
                    self.peer_port = int(float(pp))
                continue

            m = UP_RE.search(line)
            if m:
                if self.up_line is None:
                    self.up_line = (lineno, line)
                self.up_bound = int(m.group("bound"))
                self.up_peer = m.group("ip")
                self.up_peer_port = int(m.group("port"))
                continue

            if STATS_RE.search(line):
                tx, rx = kv(line, "tx"), kv(line, "rx")
                if tx is not None and rx is not None:
                    self.stats.append((int(float(tx)), int(float(rx))))
                    self.stats_line = line
                    self.stats_lineno = lineno
                continue

            if SMOOTH_RE.search(line):
                rec = {}
                for key in ("frames", "hold", "ext", "clamp", "eqT", "drop",
                            "wipe", "extra", "avgD", "maxD"):
                    v = kv(line, key)
                    rec[key] = as_float(v) if v is not None else None
                rec["_line"] = line
                rec["_lineno"] = lineno
                self.smooth.append(rec)
                self.smooth_line = (lineno, line)
                continue

            if APPLY_RE.search(line):
                self.applies += 1
            elif ACTION_RE.search(line):
                self.actions += 1

        try:
            st = os.stat(self.path)
            span = st.st_mtime - st.st_ctime
            if span >= 0:
                self.duration_s = span
                self.duration_source = "file span (mtime - ctime)"
        except OSError:
            pass


# ----------------------------------------------------------------------------
# Check rows
# ----------------------------------------------------------------------------
def row(cid, scope, expected, ok, actual, lines=None, info=False):
    lines = lines or []
    return {
        "check": cid,
        "scope": scope,
        "status": "INFO" if info else ("PASS" if ok else "FAIL"),
        "expected": expected,
        "actual": actual,
        "lines": lines,
    }


def absent_row(cid, scope, expected, field, lines=None):
    return row(cid, scope, expected, False, "field absent: " + field, lines)


def pass_fail(ok):
    return "PASS" if ok else "FAIL"


def fmt(v, nd=4):
    if v is None:
        return "-"
    if isinstance(v, float):
        return ("%." + str(nd) + "f") % v
    return str(v)


def label(facts, side):
    return "%s(%s pid=%s)" % (side, facts.name, facts.pid if facts.pid is not None else "?")


def p95(values):
    """Nearest-rank p95: sort ascending, take ceil(0.95*N)."""
    vals = sorted(values)
    if not vals:
        return None
    rank = max(1, int(math.ceil(0.95 * len(vals))))
    return vals[rank - 1]


# ----------------------------------------------------------------------------
# M1 — C0 + C1
# ----------------------------------------------------------------------------
def eval_m1(a, b):
    rows = []

    # 1. two distinct processes
    if a.pid is None or b.pid is None:
        rows.append(absent_row("M1-01 distinct pids", "both", "two pids",
                              "pid in filename (phase1_log_<pid>.txt)"))
    else:
        rows.append(row("M1-01 distinct pids", "both", "pidA != pidB",
                        a.pid != b.pid,
                        "A pid=%d, B pid=%d%s" % (a.pid, b.pid, "" if a.pid != b.pid else " -> same process"),
                        [label(a, "A"), label(b, "B")]))

    # 2. E0-A: two different bound ports (one socket each)
    lpa, lpb = a.port, b.port
    if lpa is None or lpb is None:
        lines = [x for x in (a.cfg_line, b.cfg_line) if x]
        rows.append(absent_row("M1-02 E0-A bound ports differ", "both",
                              "A.boundPort != B.boundPort",
                              "bound port ([Net] up: bound= or [Net] cfg local=)",
                              [t for _, t in lines]))
    else:
        detail = "A=%d (%s) B=%d (%s)" % (lpa, a.port_source, lpb, b.port_source)
        if lpa == lpb:
            detail += " -> SAME PORT: single socket, both sides share one endpoint"
        rows.append(row("M1-02 E0-A bound ports differ", "both",
                        "A.boundPort != B.boundPort", lpa != lpb, detail,
                        [t for t in (a.cfg_line and a.cfg_line[1], b.cfg_line and b.cfg_line[1],
                                     a.up_line and a.up_line[1], b.up_line and b.up_line[1]) if t]))

    # 3. peer is a real remote (GATES M1 #1)
    if a.peer is None or b.peer is None:
        rows.append(absent_row("M1-03 peer not loopback", "both",
                              "peer=<not 127.0.0.1>", "[Net] cfg peer=",
                              [t for t in (a.cfg_line and a.cfg_line[1],
                                           b.cfg_line and b.cfg_line[1]) if t]))
    else:
        bad = [x for x in ((a, "A"), (b, "B")) if (x[0].peer or "").lower() in LOOPBACK]
        rows.append(row("M1-03 peer not loopback", "both", "peer=<not 127.0.0.1>",
                        not bad,
                        "A peer=%s, B peer=%s%s" % (a.peer, b.peer,
                                                   "" if not bad else
                                                   " -> loopback peer (%s): no packets crossed a network"
                                                   % ",".join(s for _, s in bad)),
                        [t for t in (a.cfg_line and a.cfg_line[1], b.cfg_line and b.cfg_line[1]) if t]))

    # 4. traffic received
    if a.rx is None or b.rx is None:
        rows.append(absent_row("M1-04 rx>0", "both", "rx > 0", "[Net] stats rx="))
    else:
        rows.append(row("M1-04 rx>0", "both", "rx > 0 (both)", a.rx > 0 and b.rx > 0,
                        "A rx=%d, B rx=%d" % (a.rx, b.rx),
                        [t for t in (a.stats_line, b.stats_line) if t]))

    # 5. traffic sent
    if a.tx is None or b.tx is None:
        rows.append(absent_row("M1-05 tx>0", "both", "tx > 0", "[Net] stats tx="))
    else:
        rows.append(row("M1-05 tx>0", "both", "tx > 0 (both)", a.tx > 0 and b.tx > 0,
                        "A tx=%d, B tx=%d" % (a.tx, b.tx),
                        [t for t in (a.stats_line, b.stats_line) if t]))

    # 6. loopback self-count: identical tx and rx is the socket talking to itself
    if a.tx is None or b.tx is None or a.rx is None or b.rx is None:
        rows.append(absent_row("M1-06 rx!=tx (no self-count)", "both",
                              "rx != tx on both", "[Net] stats tx=/rx="))
    else:
        same = [s for (f, s) in ((a, "A"), (b, "B")) if f.rx == f.tx]
        rows.append(row("M1-06 rx!=tx (no self-count)", "both", "rx != tx (both)",
                        not same,
                        "A tx=%d rx=%d, B tx=%d rx=%d%s"
                        % (a.tx, a.rx, b.tx, b.rx,
                           "" if not same else
                           " -> tx==rx on %s: counters describe a self-loop, not a peer exchange"
                           % ",".join(same)),
                        [t for t in (a.stats_line, b.stats_line) if t]))

    # 7. the plugin actually applied peer poses
    rows.append(row("M1-07 apply>0", "both", "[Net] apply > 0 (both)",
                    a.applies > 0 and b.applies > 0,
                    "A apply=%d, B apply=%d" % (a.applies, b.applies)))

    # 8. smoothing counters clean
    keys = ("drop", "wipe", "clamp")
    missing = [k for k in keys
               if a.smooth_max(k) is None or b.smooth_max(k) is None]
    if missing or not a.smooth or not b.smooth:
        rows.append(absent_row("M1-08 drop/wipe/clamp==0", "both",
                              "drop=0 wipe=0 clamp=0",
                              "[Smooth] " + "/".join(missing or keys) + "=",
                              [t for t in (a.smooth_line and a.smooth_line[1],
                                           b.smooth_line and b.smooth_line[1]) if t]))
    else:
        vals = "A drop=%s wipe=%s clamp=%s, B drop=%s wipe=%s clamp=%s" % tuple(
            fmt(x, 0) for x in (a.smooth_max("drop"), a.smooth_max("wipe"), a.smooth_max("clamp"),
                                b.smooth_max("drop"), b.smooth_max("wipe"), b.smooth_max("clamp")))
        ok = all((a.smooth_max(k) or 0) == 0 for k in keys) and \
             all((b.smooth_max(k) or 0) == 0 for k in keys)
        bad_lines, bad_detail = [], []
        for f in (a, b):
            for k in keys:
                v = f.smooth_max(k)
                if v:
                    bad_detail.append("%s %s=%s" % (f.name, k, fmt(v, 0)))
                    worst = f.smooth_argmax(k)
                    if worst:
                        bad_lines.append(worst["_line"])
        rows.append(row("M1-08 drop/wipe/clamp==0", "both", "drop=0 wipe=0 clamp=0",
                        ok, vals + ("" if ok else " -> nonzero: " + ", ".join(bad_detail)),
                        bad_lines or [t for t in (a.smooth_line and a.smooth_line[1],
                                                  b.smooth_line and b.smooth_line[1]) if t]))

    # 9. max deviation
    ma, mb = a.smooth_max("maxD"), b.smooth_max("maxD")
    if ma is None or mb is None:
        rows.append(absent_row("M1-09 maxD<=2.0", "both", "maxD <= %.1f m" % MAXD_LIMIT,
                              "[Smooth] maxD=",
                              [t for t in (a.smooth_line and a.smooth_line[1],
                                           b.smooth_line and b.smooth_line[1]) if t]))
    else:
        ok = ma <= MAXD_LIMIT and mb <= MAXD_LIMIT
        lines = []
        if not ok:
            for f in (a, b):
                worst = f.smooth_argmax("maxD")
                if worst and worst["maxD"] is not None and worst["maxD"] > MAXD_LIMIT:
                    lines.append(worst["_line"])
        rows.append(row("M1-09 maxD<=2.0", "both", "maxD <= %.1f m" % MAXD_LIMIT,
                        ok,
                        "A maxD=%s, B maxD=%s" % (fmt(ma), fmt(mb)),
                        lines or [t for t in (a.smooth_line and a.smooth_line[1],
                                              b.smooth_line and b.smooth_line[1]) if t]))

    # 10. crash
    crashy = [(f, s) for (f, s) in ((a, "A"), (b, "B")) if f.crash]
    rows.append(row("M1-10 no crash marker", "both", "0 crash markers",
                    not crashy,
                    "A=%s, B=%s" % ("none" if not a.crash else "line %d" % a.crash[0],
                                    "none" if not b.crash else "line %d" % b.crash[0]),
                    [f.crash[1] for f, _ in crashy]))

    # 11. the run was long enough for the claim
    if a.duration_s is None or b.duration_s is None:
        rows.append(absent_row("M1-11 duration>=10min", "both",
                              ">= %d s" % MIN_DURATION_M1, "log duration",
                              [a.duration_source, b.duration_source]))
    else:
        rows.append(row("M1-11 duration>=10min", "both", ">= %d s" % MIN_DURATION_M1,
                        a.duration_s >= MIN_DURATION_M1 and b.duration_s >= MIN_DURATION_M1,
                        "A=%.0f s, B=%.0f s (%s)" % (a.duration_s, b.duration_s, a.duration_source)))

    # -- informational (never decides the gate) ----------------------------
    if a.rx and b.rx and a.rx > 0 and b.rx > 0:
        rows.append(row("M1-i1 apply/rx ratio (GATES #3)", "both", ">= 90%",
                        True,
                        "A=%.0f%%, B=%.0f%%" % (100.0 * a.applies / a.rx, 100.0 * b.applies / b.rx),
                        info=True))
    else:
        rows.append(row("M1-i1 apply/rx ratio (GATES #3)", "both", ">= 90%", True,
                        "n/a (rx absent or zero)", info=True))

    asi_a, asi_b = a.build_asi, b.build_asi
    rows.append(row("M1-i2 build hash", "both", "same [Build] asi= in both", True,
                    "A=%s, B=%s%s" % (asi_a[:12] if asi_a else "field absent",
                                      asi_b[:12] if asi_b else "field absent",
                                      "" if (asi_a and asi_b and asi_a == asi_b) else
                                      " -> cannot confirm both machines ran the same ASI"),
                    info=True))
    return rows


# ----------------------------------------------------------------------------
# M2 — C2
# ----------------------------------------------------------------------------
KILL_COLUMNS = ["tick", "victimobjid", "killerside", "adeadms", "bdeadms",
                "corpsea", "corpseb"]

TRUE_TOKENS = {"1", "true", "yes", "y", "t"}
FALSE_TOKENS = {"0", "false", "no", "n", "f", "-1", ""}
NULL_TOKENS = {"", "-", "na", "n/a", "none", "null", "nan"}


def parse_number(value):
    if value is None:
        return None
    s = str(value).strip()
    if s.lower() in NULL_TOKENS:
        return None
    try:
        return float(s)
    except ValueError:
        return None


def parse_flag(value):
    if value is None:
        return None
    s = str(value).strip().lower()
    if s in TRUE_TOKENS:
        return True
    if s in FALSE_TOKENS:
        return False
    n = parse_number(s)
    if n is None:
        return None
    return n > 0


def load_kills(path):
    """-> (rows, error). Rows are dicts with normalised lowercase keys."""
    if not path or not os.path.isfile(path):
        return None, "field absent: kills.tsv (%s)" % path
    try:
        with open(path, "r", encoding="utf-8-sig", errors="replace", newline="") as f:
            rdr = csv.DictReader(f, delimiter="\t")
            if not rdr.fieldnames:
                return None, "field absent: kills.tsv header"
            keys = {k.strip().lower(): k for k in rdr.fieldnames if k}
            missing = [c for c in KILL_COLUMNS if c not in keys]
            if missing:
                return None, "field absent: column(s) " + ", ".join(missing)
            out = []
            for rec in rdr:
                if not any((v or "").strip() for v in rec.values() if isinstance(v, str)):
                    continue
                out.append({c: (rec.get(keys[c]) or "").strip() for c in KILL_COLUMNS})
            return out, None
    except OSError as exc:
        return None, "field absent: kills.tsv (%s)" % exc


def eval_m2(a, b, kills_path):
    rows = []

    kills, err = load_kills(kills_path)
    rows.append(row("M2-00 kills.tsv schema", "-",
                    "columns " + ",".join(KILL_COLUMNS),
                    err is None, "ok" if err is None else err))

    # deaths per row
    if kills is not None:
        parsed = []
        for i, r in enumerate(kills):
            a_ms = parse_number(r["adeadms"])
            b_ms = parse_number(r["bdeadms"])
            parsed.append({
                "n": i + 1,
                "tick": parse_number(r["tick"]),
                "victim": r["victimobjid"],
                "side": r["killerside"].upper(),
                "a_ms": a_ms,
                "b_ms": b_ms,
                "a_dead": a_ms is not None and a_ms >= 0,
                "b_dead": b_ms is not None and b_ms >= 0,
                "ca": parse_flag(r["corpsea"]),
                "cb": parse_flag(r["corpseb"]),
                "raw": "\t".join(r[c] for c in KILL_COLUMNS),
            })

        matched = [p for p in parsed if p["a_dead"] and p["b_dead"]]
        one_sided = [p for p in parsed if p["a_dead"] != p["b_dead"]]

        rows.append(row("M2-01 matched kills == 20", "-", "20/20",
                        len(matched) == KILL_TARGET,
                        "%d/%d matched (%d rows in table)" % (len(matched), KILL_TARGET, len(parsed))))

        rows.append(row("M2-02 false deaths == 0", "-", "0",
                        len(one_sided) == 0,
                        "%d%s" % (len(one_sided),
                                  "" if not one_sided else
                                  " -> row(s) " + ",".join(str(p["n"]) for p in one_sided)
                                  + " report a death on one side only"),
                        [p["raw"] for p in one_sided]))

        corpse_ok = [p for p in matched if p["ca"] is True and p["cb"] is True]
        corpses_missing = [p for p in matched if p not in corpse_ok]
        rows.append(row("M2-03 corpse on both", "-", "20/20 of matched",
                        len(matched) == KILL_TARGET and len(corpse_ok) == KILL_TARGET,
                        "%d/%d" % (len(corpse_ok), KILL_TARGET)
                        + ("" if not corpses_missing else
                           " -> row(s) " + ",".join(str(p["n"]) for p in corpses_missing)
                           + " missing a corpse"),
                        [p["raw"] for p in corpses_missing]))

        deltas = [abs(p["b_ms"] - p["a_ms"]) for p in matched]
        if not deltas:
            rows.append(absent_row("M2-04 p95 latency<=200ms", "-", "p95 <= 200 ms",
                                  "aDeadMs/bDeadMs pairs"))
        else:
            p = p95(deltas)
            rows.append(row("M2-04 p95 latency<=200ms", "-", "p95 <= 200 ms",
                            p <= LATENCY_LIMIT_MS,
                            "p95=%.1f ms over %d matched (max=%.1f, mean=%.1f)"
                            % (p, len(deltas), max(deltas), statistics.fmean(deltas))))
            rows.append(row("M2-i2 max latency (GATES #4)", "-", "reported, p95 decides",
                            True, "max=%.1f ms (n=%d)" % (max(deltas), len(deltas)),
                            info=True))

        # races: same victim killed more than once
        groups = {}
        for p_ in parsed:
            groups.setdefault(p_["victim"], []).append(p_)
        race_groups = {k: v for k, v in groups.items() if len(v) > 1}
        race_bad = []
        for victim, grp in race_groups.items():
            deaths = [x for x in grp if x["a_dead"] or x["b_dead"]]
            corpses_a = sum(1 for x in grp if x["ca"] is True)
            corpses_b = sum(1 for x in grp if x["cb"] is True)
            if len(deaths) != 1 or corpses_a > 1 or corpses_b > 1:
                race_bad.append((victim, len(deaths), corpses_a, corpses_b, grp))
        if not race_groups:
            rows.append(row("M2-05 race <=1 death", "-", "1 death per duplicated victim", True,
                            "0 race rows in table (no duplicated victimObjId)"))
        else:
            rows.append(row("M2-05 race <=1 death", "-", "1 death per duplicated victim",
                            not race_bad,
                            "%d duplicated victim(s), %d bad" % (len(race_groups), len(race_bad))
                            + ("" if not race_bad else " -> " + "; ".join(
                                "victim %s deaths=%d corpseA=%d corpseB=%d" % (v, d, ca, cb)
                                for v, d, ca, cb, _ in race_bad)),
                            [x["raw"] for _, _, _, _, grp in race_bad for x in grp]))

        # resurrection: a victim seen dead earlier comes back alive later
        by_victim = {}
        for p_ in parsed:
            by_victim.setdefault(p_["victim"], []).append(p_)
        resurrected = []
        for victim, grp in by_victim.items():
            ordered = sorted(grp, key=lambda x: (x["tick"] if x["tick"] is not None else -1, x["n"]))
            died_at = None
            for x in ordered:
                if x["a_dead"] or x["b_dead"]:
                    died_at = x["tick"]
                elif died_at is not None:
                    resurrected.append(x)
        rows.append(row("M2-06 no resurrected", "-", "0",
                        not resurrected,
                        "%d resurrected row(s)%s" % (len(resurrected),
                                                    "" if not resurrected else
                                                    " -> " + ",".join(str(x["n"]) for x in resurrected)),
                        [x["raw"] for x in resurrected]))
    else:
        for cid, exp in (("M2-01 matched kills == 20", "20/20"),
                         ("M2-02 false deaths == 0", "0"),
                         ("M2-03 corpse on both", "20/20 of matched"),
                         ("M2-04 p95 latency<=200ms", "p95 <= 200 ms"),
                         ("M2-05 race <=1 death", "1 death per duplicated victim"),
                         ("M2-06 no resurrected", "0")):
            rows.append(absent_row(cid, "-", exp, "kills.tsv"))

    # crash, both logs
    crashy = [(f, s) for (f, s) in ((a, "A"), (b, "B")) if f.crash]
    rows.append(row("M2-07 crash == 0", "both", "0 crash markers", not crashy,
                    "A=%s, B=%s" % ("none" if not a.crash else "line %d" % a.crash[0],
                                    "none" if not b.crash else "line %d" % b.crash[0]),
                    [f.crash[1] for f, _ in crashy]))

    if a.duration_s and b.duration_s:
        rows.append(row("M2-i1 duration (GATES #7)", "both", ">= %d s" % MIN_DURATION_M2, True,
                        "A=%.0f s, B=%.0f s (%s)" % (a.duration_s, b.duration_s, a.duration_source),
                        info=True))
    else:
        rows.append(row("M2-i1 duration (GATES #7)", "both", ">= %d s" % MIN_DURATION_M2, True,
                        "field absent: log duration", info=True))
    return rows


# ----------------------------------------------------------------------------
# Reporting
# ----------------------------------------------------------------------------
def clean(value):
    return str(value).replace("\t", " ").replace("\r", " ").replace("\n", " ")


def write_tsv(path, rows):
    with open(path, "w", encoding="utf-8", newline="") as f:
        w = csv.writer(f, delimiter="\t", lineterminator="\n")
        w.writerow(["check", "scope", "status", "expected", "actual", "source_line"])
        for r in rows:
            w.writerow([clean(r["check"]), clean(r["scope"]), clean(r["status"]),
                        clean(r["expected"]), clean(r["actual"]),
                        clean(r["lines"][0]) if r["lines"] else "-"])


def verdict_text(gate, rows, facts, extra=""):
    fails = [r for r in rows if r["status"] == "FAIL"]
    infos = [r for r in rows if r["status"] == "INFO"]
    out = []
    out.append("%s verdict: %s" % (gate, "PASS" if not fails else "FAIL"))
    out.append("generated: %s (gate_parse.py)" % dt.datetime.now().strftime("%Y-%m-%dT%H:%M:%S"))
    out.append("checks: %d, passing: %d, failing: %d, info: %d"
               % (len(rows), len(rows) - len(fails) - len(infos), len(fails), len(infos)))
    if facts:
        for side, f in facts:
            build = f.build_asi[:12] if f.build_asi else "field absent"
            out.append(
                "%s: %s pid=%s port=%s(%s) peer=%s tx=%s rx=%s apply=%d actions=%d "
                "maxD=%s drop=%s wipe=%s clamp=%s crash=%s duration=%s"
                % (side, f.name, f.pid if f.pid is not None else "?",
                   f.port if f.port is not None else "?",
                   f.port_source,
                   f.peer if f.peer else "field absent",
                   f.tx if f.tx is not None else "?",
                   f.rx if f.rx is not None else "?",
                   f.applies, f.actions,
                   fmt(f.smooth_max("maxD")),
                   fmt(f.smooth_max("drop"), 0), fmt(f.smooth_max("wipe"), 0),
                   fmt(f.smooth_max("clamp"), 0),
                   "none" if not f.crash else "line %d" % f.crash[0],
                   "%.0f s" % f.duration_s if f.duration_s is not None else "?"))
            out.append("%s: build asi=%s (%s)" % (side, build,
                                                 "from [Build] asi=" if f.build_asi else "field absent"))
    if extra:
        out.append(extra)
    if fails:
        out.append("")
        out.append("failing checks:")
        for r in fails:
            out.append("FAIL %s [%s]" % (r["check"], r["scope"]))
            out.append("  expected: %s" % r["expected"])
            out.append("  actual  : %s" % r["actual"])
            if r["lines"]:
                for ln in r["lines"]:
                    out.append("  line    : %s" % ln)
            elif str(r["actual"]).startswith("field absent"):
                out.append("  line    : (no line on disk carries this field)")
            else:
                out.append("  line    : (aggregate over the whole log: no single line decides it)")
    else:
        out.append("")
        out.append("all blocking checks passed (info rows do not decide the gate)")
    return "\n".join(out) + "\n"


def emit(gate, rows, facts, out_dir, tsv_name, verdict_name, extra=""):
    text = verdict_text(gate, rows, facts, extra)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
        write_tsv(os.path.join(out_dir, tsv_name), rows)
        with open(os.path.join(out_dir, verdict_name), "w", encoding="utf-8") as f:
            f.write(text)
    print(text, end="")
    return 0 if not any(r["status"] == "FAIL" for r in rows) else 1


# ----------------------------------------------------------------------------
# Selftest fixtures
# ----------------------------------------------------------------------------
def synth_log(pid, peer, local, peer_port, tx, rx, applies, actions,
              maxd=0.4, drop=0, wipe=0, clamp=0, crash=None, asi=None,
              duration=700.0, up=False):
    lines = ["[Phase1] DriveContext=0 ctxMask=0x440"]
    if asi:
        lines.append("[Build] asi=%s  exe=deadbeef  commit=selftest" % asi)
    lines += [
        "[IdRes] entity identity: scan only",
        "[Hook] GetUnusedPadID detour: installed",
        "[Net] init=1  (bound/peer line goes to OutputDebugString)",
        "[Net] cfg enabled=1 peer=%s local=%d peerPort=%d strictPeer=1"
        % (peer, local, peer_port),
    ]
    if up:
        lines.append("[Net] up: bound:%d -> %s:%d" % (local, peer, peer_port))
    for i in range(max(1, applies // 2)):
        lines.append("[Net] apply seq=%d pos=(1.00,-2.00,3.00) spd=1.2 dst=0.4 ctx=0 bundle=1"
                     % (i + 1))
    for i in range(actions):
        lines.append("[Net] action -> %d ch=0 ly=0 ratio=0.00 blendIn=0.20 (puppet, cur was 1)"
                     % (100 + i))
    lines += [
        "[Smooth] frames=228 hold=0 ext=0 clamp=%d eqT=0 drop=%d wipe=%d extra=76 avgD=0.0162 maxD=%.4f"
        % (clamp, drop, wipe, maxd),
        "[Net] stats tx=%d pkt, %d B, 4711 ent  rx=%d pkt, %d B  |  22.2 kB/s up, 22.2 kB/s down  |  "
        "mirror ok=0 nobod=0 steal=0" % (tx, tx * 900, rx, rx * 900),
    ]
    if crash:
        lines.append(crash)
    return "\n".join(lines) + "\n"


def write_fixture(folder, pid, duration=700.0, **kw):
    path = os.path.join(folder, "phase1_log_%d.txt" % pid)
    with open(path, "w", encoding="utf-8") as f:
        f.write(synth_log(pid=pid, **kw))
    st = os.stat(path)
    os.utime(path, (st.st_atime, st.st_mtime + duration))
    return path


def write_kills(path, rows):
    with open(path, "w", encoding="utf-8", newline="") as f:
        w = csv.writer(f, delimiter="\t", lineterminator="\n")
        w.writerow(["tick", "victimObjId", "killerSide", "aDeadMs", "bDeadMs",
                    "corpseA", "corpseB"])
        for r in rows:
            w.writerow(r)


def selftest():
    tmp = tempfile.mkdtemp(prefix="gate_parse_selftest_")
    results = []

    def run_case(name, expect, gate, pa, pb, kills=None, expect_ids=()):
        if gate == "M1":
            fa, fb = LogFacts(pa), LogFacts(pb)
            rows = eval_m1(fa, fb)
            facts = (("A", fa), ("B", fb))
        else:
            fa, fb = LogFacts(pa), LogFacts(pb)
            rows = eval_m2(fa, fb, kills)
            facts = (("A", fa), ("B", fb))
        got = "FAIL" if any(r["status"] == "FAIL" for r in rows) else "PASS"
        fails = [r["check"] for r in rows if r["status"] == "FAIL"]
        ok = got == expect
        ids_ok = all(any(i in c for c in fails) for i in expect_ids) if expect_ids else True
        results.append({
            "name": name, "expect": expect, "got": got,
            "fails": fails, "ok": ok and ids_ok, "ids_ok": ids_ok,
        })
        return rows

    try:
        # 1 — good pair
        d1 = os.path.join(tmp, "good")
        os.makedirs(d1)
        a1 = write_fixture(d1, 1001, peer="192.168.1.11", local=48001, peer_port=48002,
                           tx=420, rx=398, applies=380, actions=64, maxd=0.41,
                           asi="f474422f88c6aabb", duration=700.0)
        b1 = write_fixture(d1, 1002, peer="192.168.1.10", local=48002, peer_port=48001,
                           tx=398, rx=420, applies=404, actions=71, maxd=0.77,
                           asi="f474422f88c6aabb", duration=700.0)
        run_case("good pair", "PASS", "M1", a1, b1)

        # 2 — both bound the same port (E0-A)
        d2 = os.path.join(tmp, "sameport")
        os.makedirs(d2)
        a2 = write_fixture(d2, 2001, peer="192.168.1.11", local=47801, peer_port=47801,
                           tx=420, rx=398, applies=380, actions=64, maxd=0.41,
                           asi="f474422f88c6aabb")
        b2 = write_fixture(d2, 2002, peer="192.168.1.10", local=47801, peer_port=47801,
                           tx=398, rx=420, applies=404, actions=71, maxd=0.77,
                           asi="f474422f88c6aabb")
        run_case("same bound port", "FAIL", "M1", a2, b2, expect_ids=("E0-A",))

        # 3 — loopback with rx == tx
        d3 = os.path.join(tmp, "loopback")
        os.makedirs(d3)
        a3 = write_fixture(d3, 3001, peer="127.0.0.1", local=48001, peer_port=48002,
                           tx=400, rx=400, applies=390, actions=64, maxd=0.41,
                           asi="f474422f88c6aabb")
        b3 = write_fixture(d3, 3002, peer="127.0.0.1", local=48002, peer_port=48001,
                           tx=400, rx=400, applies=390, actions=71, maxd=0.77,
                           asi="f474422f88c6aabb")
        run_case("rx==tx on loopback", "FAIL", "M1", a3, b3,
                 expect_ids=("M1-03", "M1-06"))

        # 4 — crash marker
        d4 = os.path.join(tmp, "crash")
        os.makedirs(d4)
        a4 = write_fixture(d4, 4001, peer="192.168.1.11", local=48001, peer_port=48002,
                           tx=420, rx=398, applies=380, actions=64, maxd=0.41,
                           asi="f474422f88c6aabb",
                           crash="[Crash] unhandled exception 0xC0000005 at 0x00681817 write 0x0")
        b4 = write_fixture(d4, 4002, peer="192.168.1.10", local=48002, peer_port=48001,
                           tx=398, rx=420, applies=404, actions=71, maxd=0.77,
                           asi="f474422f88c6aabb")
        run_case("crash marker", "FAIL", "M1", a4, b4, expect_ids=("M1-10",))

        # 5 — M2 table, good
        d5 = os.path.join(tmp, "m2good")
        os.makedirs(d5)
        a5 = write_fixture(d5, 5001, peer="192.168.1.11", local=48001, peer_port=48002,
                           tx=420, rx=398, applies=380, actions=64, maxd=0.41,
                           asi="f474422f88c6aabb", duration=1000.0)
        b5 = write_fixture(d5, 5002, peer="192.168.1.10", local=48002, peer_port=48001,
                           tx=398, rx=420, applies=404, actions=71, maxd=0.77,
                           asi="f474422f88c6aabb", duration=1000.0)
        good_kills = []
        for i in range(20):
            t = 1000 + i * 40
            good_kills.append([t, 400000000 + i, "A" if i % 2 else "B",
                               t + 30 + (i % 7) * 10, t + 60 + (i % 5) * 12, 1, 1])
        k5 = os.path.join(d5, "kills.tsv")
        write_kills(k5, good_kills)
        run_case("M2 good table", "PASS", "M2", a5, b5, kills=k5)

        # 6 — M2 table with one late death (>200 ms) and one resurrection
        d6 = os.path.join(tmp, "m2bad")
        os.makedirs(d6)
        a6 = write_fixture(d6, 6001, peer="192.168.1.11", local=48001, peer_port=48002,
                           tx=420, rx=398, applies=380, actions=64, maxd=0.41,
                           asi="f474422f88c6aabb", duration=1000.0)
        b6 = write_fixture(d6, 6002, peer="192.168.1.10", local=48002, peer_port=48001,
                           tx=398, rx=420, applies=404, actions=71, maxd=0.77,
                           asi="f474422f88c6aabb", duration=1000.0)
        bad_kills = [list(r) for r in good_kills]
        late = 0
        for r in bad_kills:
            if late < 3:
                # 3 of 20 late by 900 ms -> p95 (19th of 20) is 900 ms
                r[4] = r[3] + 900
                late += 1
        bad_kills.append([3000, 400000000, "-", -1, -1, 0, 0])  # same victim, later, alive
        k6 = os.path.join(d6, "kills.tsv")
        write_kills(k6, bad_kills)
        run_case("M2 late death + resurrected", "FAIL", "M2", a6, b6, kills=k6,
                 expect_ids=("M2-04", "M2-06"))
    finally:
        pass

    width = max(len(r["name"]) for r in results)
    print("gate_parse.py selftest - synthetic fixtures in %s" % tmp)
    print("")
    print("%-*s  %-8s  %-8s  %s" % (width, "case", "expect", "got", "result"))
    print("%s  %s  %s  %s" % ("-" * width, "-" * 8, "-" * 8, "-" * 6))
    all_ok = True
    for r in results:
        mark = "ok" if r["ok"] else "MISMATCH"
        if not r["ok"]:
            all_ok = False
        print("%-*s  %-8s  %-8s  %s" % (width, r["name"], r["expect"], r["got"], mark))
        if r["fails"]:
            print("%-*s  failing: %s" % (width, "", "; ".join(r["fails"])))
        if not r["ids_ok"]:
            print("%-*s  expected failing check(s) not reported" % (width, ""))
    ok_count = sum(1 for r in results if r["ok"])
    print("")
    print("selftest: %d/%d expectations met -> %s"
          % (ok_count, len(results), "OK" if all_ok else "FAILED"))

    # keep the fixtures when something is wrong so it can be inspected
    if all_ok:
        shutil.rmtree(tmp, ignore_errors=True)
    else:
        print("fixtures kept in: %s" % tmp)
    return 0 if all_ok else 1


# ----------------------------------------------------------------------------
# CLI
# ----------------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Decide M1/M2 from the two plugin logs (no game required).")
    ap.add_argument("gate", nargs="?", choices=("M1", "M2"),
                    help="gate to evaluate")
    ap.add_argument("--a", dest="log_a", help="player A phase1_log_<pid>.txt")
    ap.add_argument("--b", dest="log_b", help="player B phase1_log_<pid>.txt")
    ap.add_argument("--kills", help="kills.tsv (M2)")
    ap.add_argument("--out", help="evidence folder to write the verdict into")
    ap.add_argument("--selftest", action="store_true",
                    help="run synthetic fixtures and check the verdicts")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()

    if not args.gate:
        ap.error("gate is required unless --selftest is given")
    for name, path in (("--a", args.log_a), ("--b", args.log_b)):
        if not path:
            ap.error("%s is required" % name)
        if not os.path.isfile(path):
            print("%s: FAIL — field absent: log %s (%s)" % (args.gate, name, path))
            return 2

    if args.gate == "M2" and not args.kills:
        ap.error("--kills is required for M2")

    fa, fb = LogFacts(args.log_a), LogFacts(args.log_b)
    for f in (fa, fb):
        if f.read_error:
            print("FAIL — cannot read %s: %s" % (f.name, f.read_error))
            return 2

    if args.gate == "M1":
        rows = eval_m1(fa, fb)
        return emit("M1", rows, (("A", fa), ("B", fb)), args.out,
                    "m1.tsv", "verdict.txt")

    rows = eval_m2(fa, fb, args.kills)
    extra = "kills.tsv: %s" % os.path.abspath(args.kills)
    return emit("M2", rows, (("A", fa), ("B", fb)), args.out,
                "m2.tsv", "m2-verdict.txt", extra)


if __name__ == "__main__":
    sys.exit(main())
