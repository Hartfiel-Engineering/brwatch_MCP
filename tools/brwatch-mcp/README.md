# brwatch-mcp

MCP server that exposes [brwatch.exe](../../README.md)'s CLI subcommands to Claude Code
so a conversational "debug this machine" workflow drives real B&R PLC data
collection.

## Install

Requires Python 3.10+ and `brwatch.exe` built (see repo root).

```powershell
cd tools\brwatch-mcp
pip install mcp
```

## Register with Claude Code

Edit your Claude Code MCP config (`~/.claude.json` or similar — see `claude mcp` docs)
or your project-local `.mcp.json`:

```json
{
  "mcpServers": {
    "brwatch": {
      "command": "python",
      "args": ["C:/Projects/B_R/brwatch/tools/brwatch-mcp/brwatch_mcp.py"],
      "env": {
        "BRWATCH_EXE": "C:/Projects/B_R/brwatch/build/brwatch.exe",
        "BRWATCH_CWD": "C:/Projects/B_R/brwatch/build"
      }
    }
  }
}
```

| Env var | Default | Purpose |
| --- | --- | --- |
| `BRWATCH_CWD` | current CWD | Parent folder of `.brwatch/`. Change per machine project. |
| `BRWATCH_EXE` | `brwatch.exe` (PATH) | Absolute path to the brwatch binary. |

## Project layout

Each "machine project" is a folder containing a `.brwatch/` subdirectory:

```
your-machine-folder/
  brwatch.ini           # DEVICE1/CPU1 PVI config - normally symlinked
  .brwatch/
    pvs.json            # watched PVs (JSON array of "cpu->var" or "cpu->task:var")
    brwatch.db          # SQLite - runs/pvs/samples tables
    notes.md            # free-form notes about the machine
    .pid                # PID of running start-logging (if any)
    .pvs.txt            # pvs.json flattened to one spec per line (written by start_logging)
    .stop               # stop sentinel (transient)
```

`pvs.json` is written by `add_watch` and `remove_watch`. The brwatch.exe CLI
isn't aware of this file - when `start_logging` runs, the MCP server writes the
PVs to `.pvs.txt` and passes it as `--pv-file`, so there's no limit on how many
PVs a run can log.

## Tools

| Tool | What it does |
| --- | --- |
| `project_context()` | Current state: notes, watched PVs, recent runs, active logger PID. |
| `scan_network()` | Discover B&R CPUs on the LAN via SNMP + UDP broadcast. |
| `add_watch(pv)` | Append a PV to `.brwatch/pvs.json`. |
| `remove_watch(pv)` | Remove from `pvs.json`. |
| `start_logging(cycle_ms, notes)` | Spawn brwatch.exe in background. Returns PID. |
| `stop_logging()` | Write the stop sentinel; brwatch.exe exits cleanly. |
| `query_samples(pv, since, until, run_id, limit)` | SQL time-series for one PV. WAL-safe during live logging. |
| `list_runs(limit)` | Recent runs with PV and sample counts. |

## PV name formats

Two different formats appear:

- **Watch spec** (used by `add_watch`, `--pv`): `cpu_desc->var` or `cpu_desc->task:var`.
  `cpu_desc` is the `descriptor` from `brwatch.ini`'s `[CPUn]` section
  (e.g. `/RT=1000 /IP=127.0.0.1`).

- **Full path** (returned by `query_samples`): `cputype[arver]cpu_desc->[task:]var`
  (e.g. `4PPC80.0702-10B[V6.00]/RT=1000 /IP=127.0.0.1->Demo:Demo_EvtIntervalMs`).
  This is what the DB stores. Use `list_runs` or query result inspection to
  discover these.

## Example Claude Code session

```
User: "The stamper is dropping cycle time intermittently. Dig in."

Claude: calls project_context()  -> sees no prior runs, no watched PVs
        calls scan_network()      -> finds the CPU
        asks user: "Which PVs expose cycle timing? I'll start with..."
        calls add_watch("/RT=1000 /IP=.../Vis:gCycle_LastMs")
        calls add_watch(...)      -> a few more candidates
        calls start_logging(cycle_ms=250, notes="stamper cycle drop investigation")
        waits a minute
        calls stop_logging()
        calls query_samples("...Vis:gCycle_LastMs", since="now-2m")
        -> analyzes the time series, points at anomalies
```
