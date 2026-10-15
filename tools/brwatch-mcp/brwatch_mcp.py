"""
brwatch MCP server.

Exposes brwatch.exe's CLI subcommands as MCP tools so Claude Code can drive
a B&R PLC diagnostic workflow end to end.

Tools:
    project_context       Snapshot of .brwatch/ (notes, pvs, recent runs)
    scan_network          Spawns `brwatch.exe scan --json`
    add_watch(pv)         Appends to .brwatch/pvs.json
    start_logging(...)    Spawns `brwatch.exe start-logging` in background
    stop_logging()        Spawns `brwatch.exe stop-logging`
    query_samples(pv,...) Direct SQL on .brwatch/brwatch.db (WAL-safe)
    list_runs(limit)      Direct SQL on .brwatch/brwatch.db

Project folder (.brwatch/) is resolved from the BRWATCH_CWD env var, or CWD
if unset. brwatch.exe path is resolved from BRWATCH_EXE env var, or "brwatch.exe"
(PATH lookup) if unset.
"""

from __future__ import annotations

import datetime
import json
import os
import pathlib
import re
import sqlite3
import subprocess
import time
from typing import Any, Optional

from mcp.server.mcpserver import MCPServer


BRWATCH_DIR_NAME = ".brwatch"
PVS_FILE = "pvs.json"
DB_FILE = "brwatch.db"
NOTES_FILE = "notes.md"
PID_FILE = ".pid"
PV_SPEC_FILE = ".pvs.txt"  # pvs.json flattened for `start-logging --pv-file`

CREATE_NO_WINDOW = 0x08000000  # Windows CreateProcess flag


def project_dir() -> pathlib.Path:
    base = os.environ.get("BRWATCH_CWD") or os.getcwd()
    return pathlib.Path(base) / BRWATCH_DIR_NAME


def brwatch_exe() -> str:
    return os.environ.get("BRWATCH_EXE") or "brwatch.exe"


def ensure_dir() -> pathlib.Path:
    d = project_dir()
    d.mkdir(parents=True, exist_ok=True)
    return d


def _parse_time(s: Optional[str]) -> Optional[int]:
    """Accepts 'now', 'now-5m', Unix epoch sec/ms, or ISO-8601 UTC. Returns ms."""
    if not s:
        return None
    s = s.strip()
    if s == "now":
        return int(time.time() * 1000)
    m = re.match(r"now-(\d+)([smhd])$", s)
    if m:
        n, unit = int(m.group(1)), m.group(2)
        mult = {"s": 1000, "m": 60_000, "h": 3_600_000, "d": 86_400_000}[unit]
        return int(time.time() * 1000) - n * mult
    if s.isdigit():
        v = int(s)
        return v if v > 10**12 else v * 1000
    try:
        dt = datetime.datetime.fromisoformat(s.rstrip("Z"))
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=datetime.timezone.utc)
        return int(dt.timestamp() * 1000)
    except ValueError as e:
        raise ValueError(f"Could not parse time '{s}'") from e


def _ms_to_iso(ms: Optional[int]) -> Optional[str]:
    if ms is None:
        return None
    return datetime.datetime.fromtimestamp(ms / 1000, tz=datetime.timezone.utc).isoformat()


def _db_connect_ro(db: pathlib.Path) -> Optional[sqlite3.Connection]:
    if not db.exists():
        return None
    uri = f"file:{db.as_posix()}?mode=ro"
    conn = sqlite3.connect(uri, uri=True)
    conn.row_factory = sqlite3.Row
    return conn


mcp = MCPServer("brwatch")


@mcp.tool()
def project_context() -> dict:
    """
    Return the current .brwatch/ project context. Call this first to see what
    PVs are already configured, what runs have happened, and read any user
    notes about the machine.
    """
    pdir = ensure_dir()

    notes = ""
    notes_path = pdir / NOTES_FILE
    if notes_path.exists():
        notes = notes_path.read_text(encoding="utf-8", errors="replace")

    pvs: list[str] = []
    pvs_path = pdir / PVS_FILE
    if pvs_path.exists():
        try:
            pvs = json.loads(pvs_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            pvs = []

    recent_runs: list[dict] = []
    conn = _db_connect_ro(pdir / DB_FILE)
    if conn is not None:
        try:
            for r in conn.execute(
                "SELECT run_id, started_utc, stopped_utc, cycle_ms, notes "
                "FROM runs ORDER BY started_utc DESC LIMIT 20"
            ):
                recent_runs.append({
                    "run_id": r["run_id"],
                    "started_iso": _ms_to_iso(r["started_utc"] * 1000),
                    "stopped_iso": _ms_to_iso((r["stopped_utc"] or 0) * 1000) if r["stopped_utc"] else None,
                    "cycle_ms": r["cycle_ms"],
                    "notes": r["notes"],
                    "status": "complete" if r["stopped_utc"] else "running_or_crashed",
                })
        finally:
            conn.close()

    pid_path = pdir / PID_FILE
    active_pid: Optional[int] = None
    if pid_path.exists():
        try:
            active_pid = int(pid_path.read_text().strip())
        except ValueError:
            pass

    return {
        "project_dir": str(pdir),
        "brwatch_exe": brwatch_exe(),
        "notes": notes,
        "watched_pvs": pvs,
        "watched_pv_count": len(pvs),
        "recent_runs": recent_runs,
        "logging_active_pid": active_pid,
    }


@mcp.tool()
def add_watch(pv: str) -> dict:
    """
    Add a PV to the watch list for the next start_logging call.

    Format:
        'cpu_desc->var_name'          for global PVs
        'cpu_desc->task_name:var_name' for task-local PVs

    cpu_desc must match the `descriptor` field of a [CPUn] section in
    brwatch.ini (e.g. '/RT=1000 /IP=127.0.0.1'). Idempotent: adding the
    same PV twice is a no-op.
    """
    if "->" not in pv:
        raise ValueError(
            "PV spec must contain '->'. Use 'cpu_desc->var' for global "
            "or 'cpu_desc->task:var' for local."
        )

    pdir = ensure_dir()
    pvs_path = pdir / PVS_FILE
    pvs: list[str] = []
    if pvs_path.exists():
        try:
            pvs = json.loads(pvs_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            pvs = []

    if pv not in pvs:
        pvs.append(pv)
        pvs_path.write_text(json.dumps(pvs, indent=2), encoding="utf-8")

    return {"pv": pv, "already_present": pv in pvs[:-1], "total": len(pvs), "all_pvs": pvs}


@mcp.tool()
def remove_watch(pv: str) -> dict:
    """Remove a previously added PV from the watch set."""
    pdir = ensure_dir()
    pvs_path = pdir / PVS_FILE
    if not pvs_path.exists():
        return {"removed": False, "reason": "no pvs.json", "total": 0}

    pvs: list[str] = json.loads(pvs_path.read_text(encoding="utf-8"))
    before = len(pvs)
    pvs = [p for p in pvs if p != pv]
    pvs_path.write_text(json.dumps(pvs, indent=2), encoding="utf-8")
    return {"removed": before > len(pvs), "total": len(pvs), "all_pvs": pvs}


@mcp.tool()
def scan_network() -> list[dict]:
    """Discover B&R CPUs on the network via SNMP + UDP broadcast."""
    pdir = ensure_dir()
    cwd = pdir.parent  # brwatch.exe reads brwatch.ini from CWD
    result = subprocess.run(
        [brwatch_exe(), "scan", "--json"],
        capture_output=True, text=True, timeout=30, cwd=str(cwd),
        creationflags=CREATE_NO_WINDOW,
    )
    if result.returncode != 0:
        raise RuntimeError(f"scan failed (rc={result.returncode}): {result.stderr.strip()}")
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError:
        return []


@mcp.tool()
def start_logging(cycle_ms: int = 500, notes: Optional[str] = None) -> dict:
    """
    Start a logging run. Reads the watched PVs from .brwatch/pvs.json (populated
    by prior add_watch calls), writes them to .brwatch/.pvs.txt, and spawns
    brwatch.exe start-logging --pv-file in the background. No PV count limit.

    Returns immediately. Use stop_logging() to end the run cleanly. query_samples
    and list_runs will see live data while the logger is running (WAL mode).
    """
    pdir = ensure_dir()
    pvs_path = pdir / PVS_FILE
    if not pvs_path.exists():
        raise RuntimeError("No watched PVs. Call add_watch() first.")
    pvs = json.loads(pvs_path.read_text(encoding="utf-8"))
    if not pvs:
        raise RuntimeError("No watched PVs. Call add_watch() first.")

    pid_path = pdir / PID_FILE
    if pid_path.exists():
        try:
            existing = int(pid_path.read_text().strip())
            raise RuntimeError(
                f"Logging already running (PID {existing} in {pid_path}). "
                "Call stop_logging() first."
            )
        except ValueError:
            pid_path.unlink(missing_ok=True)

    # Pass PVs via a spec file rather than one --pv per PV: the command line
    # has a bounded argv/length, the file doesn't.
    spec_path = pdir / PV_SPEC_FILE
    spec_path.write_text("".join(f"{pv}\n" for pv in pvs), encoding="utf-8")

    args: list[str] = [
        brwatch_exe(), "start-logging",
        "--cycle", str(cycle_ms),
        "--db", str(pdir / DB_FILE),
        "--pv-file", str(spec_path),
    ]
    if notes:
        args.extend(["--notes", notes])

    cwd = pdir.parent
    proc = subprocess.Popen(
        args, cwd=str(cwd),
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL,
        creationflags=CREATE_NO_WINDOW,
    )
    pid_path.write_text(str(proc.pid))

    return {
        "pid": proc.pid,
        "pv_count": len(pvs),
        "cycle_ms": cycle_ms,
        "db": str(pdir / DB_FILE),
        "notes": notes,
    }


@mcp.tool()
def stop_logging() -> dict:
    """Signal the running logger to exit cleanly via the stop sentinel."""
    pdir = ensure_dir()
    cwd = pdir.parent

    result = subprocess.run(
        [brwatch_exe(), "stop-logging"],
        capture_output=True, text=True, timeout=10, cwd=str(cwd),
        creationflags=CREATE_NO_WINDOW,
    )

    pid_path = pdir / PID_FILE
    stopped_pid = None
    if pid_path.exists():
        try:
            stopped_pid = int(pid_path.read_text().strip())
        except ValueError:
            pass
        pid_path.unlink(missing_ok=True)

    if result.returncode != 0:
        raise RuntimeError(f"stop-logging failed (rc={result.returncode}): {result.stderr.strip()}")

    return {"stopped_pid": stopped_pid, "stderr": result.stderr.strip()}


@mcp.tool()
def query_samples(
    pv: str,
    since: Optional[str] = None,
    until: Optional[str] = None,
    run_id: Optional[int] = None,
    limit: int = 1000,
) -> list[dict]:
    """
    Return time-series samples for a PV. The pv argument matches the full_path
    column in the pvs table (format: 'cputype[arver]cpu_desc->[task:]var').
    Use list_runs or project_context to see which full paths exist.

    Time args accept 'now', 'now-5m' / 'now-1h' / 'now-30s', Unix epoch
    seconds or ms, or ISO-8601 (e.g. '2026-10-14T15:30:00Z').
    """
    pdir = project_dir()
    db = pdir / DB_FILE
    conn = _db_connect_ro(db)
    if conn is None:
        return []

    t_since = _parse_time(since)
    t_until = _parse_time(until)

    sql = (
        "SELECT s.ts_utc_ms, s.v_int, s.v_real, s.v_text, s.run_id "
        "FROM samples s JOIN pvs p ON p.pv_id = s.pv_id "
        "WHERE p.full_path = ?"
    )
    params: list[Any] = [pv]
    if t_since is not None:
        sql += " AND s.ts_utc_ms >= ?"
        params.append(t_since)
    if t_until is not None:
        sql += " AND s.ts_utc_ms <= ?"
        params.append(t_until)
    if run_id is not None:
        sql += " AND s.run_id = ?"
        params.append(run_id)
    sql += " ORDER BY s.ts_utc_ms LIMIT ?"
    params.append(limit)

    rows: list[dict] = []
    try:
        for r in conn.execute(sql, params):
            value: Any
            if r["v_int"] is not None:
                value = r["v_int"]
            elif r["v_real"] is not None:
                value = r["v_real"]
            else:
                value = r["v_text"]
            rows.append({
                "ts_utc_ms": r["ts_utc_ms"],
                "ts_iso": _ms_to_iso(r["ts_utc_ms"]),
                "value": value,
                "run_id": r["run_id"],
            })
    finally:
        conn.close()
    return rows


@mcp.tool()
def list_runs(limit: int = 20) -> list[dict]:
    """List logging runs with metadata. Marks still-running runs."""
    pdir = project_dir()
    db = pdir / DB_FILE
    conn = _db_connect_ro(db)
    if conn is None:
        return []

    rows: list[dict] = []
    try:
        for r in conn.execute(
            "SELECT r.run_id, r.started_utc, r.stopped_utc, r.cycle_ms, "
            "       r.notes, r.watch_file, r.pvi_version, r.hostname,"
            "       (SELECT COUNT(DISTINCT pv_id) FROM samples WHERE run_id = r.run_id) AS pv_count,"
            "       (SELECT COUNT(*) FROM samples WHERE run_id = r.run_id) AS sample_count "
            "FROM runs r ORDER BY r.started_utc DESC LIMIT ?",
            (limit,)
        ):
            rows.append({
                "run_id": r["run_id"],
                "started_iso": _ms_to_iso(r["started_utc"] * 1000),
                "stopped_iso": _ms_to_iso(r["stopped_utc"] * 1000) if r["stopped_utc"] else None,
                "status": "complete" if r["stopped_utc"] else "running_or_crashed",
                "cycle_ms": r["cycle_ms"],
                "pv_count": r["pv_count"],
                "sample_count": r["sample_count"],
                "notes": r["notes"],
                "watch_file": r["watch_file"],
                "pvi_version": (r["pvi_version"] or "").strip(),
                "hostname": r["hostname"],
            })
    finally:
        conn.close()
    return rows


if __name__ == "__main__":
    mcp.run()
