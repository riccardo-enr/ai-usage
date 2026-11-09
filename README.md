# AI Usage

AI Usage is a Linux desktop dashboard and CLI for Claude Code, Codex, and
Antigravity. The GTK dashboard shows provider quota, reset times, projected
usage at reset, and tokens recorded today. The Models, Daily, and Sessions tabs
show the underlying usage breakdowns.

## Build and run

Install Pixi and the following system dependencies. Package names vary by
distribution.

- A C11 compiler, CMake 3.20 or later, Ninja, and pkg-config
- Development libraries for SQLite3, GTK4, and Libadwaita

```text
pixi run gui
```

To install the dashboard, CLI, desktop launcher, and icons under `~/.local`,
run `pixi run install`. The desktop launcher expects `~/.local/bin` on your
desktop session's PATH. You can also run `~/.local/bin/ai-usage-gtk` directly.

## CLI and tests

| Command | Result |
| --- | --- |
| `pixi run usage` | Usage and quota summary |
| `pixi run models` | Token counts by model |
| `pixi run daily` | Daily usage totals |
| `pixi run sessions` | Recent sessions |
| `pixi run json` | Machine-readable report |
| `pixi run test` | Build and run the core tests |

## Quota readings

- Claude quota comes from the local Claude Code Monitor socket. With an updated
  monitor that supports the quota-only snapshot, the dashboard can read Claude
  Code's status-line quota even if the monitor's CLI poll fails. Older monitors
  still work through the regular snapshot. Saved readings from
  `~/.claude/usage-history.jsonl` are used when a live window is unavailable.
- Codex quota comes from local session logs and refreshes when Codex records a
  new quota snapshot.
- Antigravity provider quota comes from its CLI when available. The dashboard
  labels local rolling usage separately.

The pace estimate extrapolates the average usage rate in the current quota
window. A projection over 100% means that pace would exhaust quota before reset.
