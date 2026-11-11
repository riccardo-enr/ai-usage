# AI Usage

AI Usage is a Linux desktop dashboard and CLI for Claude Code, Codex, and
Antigravity. The GTK dashboard shows provider quota, reset times, projected
usage at reset, and tokens recorded today. The Models, Daily, and Sessions tabs
show the underlying usage breakdowns.

## Screenshots

Native GTK dashboard with sample data.

![Dark dashboard showing provider quota and today's token usage](docs/img/dashboard-dark.png)

Quota windows, reset times, and today's tokens in the dark theme.

![Light dashboard showing usage charts and expanded provider details](docs/img/dashboard-light.png)

Token composition, daily usage, and expanded provider details in the light theme.

## Build and run

Install Pixi and the following system dependencies. Package names vary by
distribution.

- A C11 compiler, CMake 3.20 or later, Ninja, and pkg-config
- Development libraries for SQLite3, GTK4, and Libadwaita

```text
pixi run gui
```

To install the dashboard, CLI, desktop launcher, and icons under `~/.local`,
run `pixi run install`, then open the AI Usage launcher entry. The generated
launcher uses the absolute installed executable path. You can also run
`~/.local/bin/ai-usage-gtk` directly.

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
  Code's status-line quota even if the monitor's CLI poll fails. The updated
  monitor also polls quota independently through its installed OAuth API reader,
  avoiding the local conversation scan. It polls every two minutes, and the API
  reader caches responses for three minutes. Older monitors
  still work through the regular snapshot. Saved readings from
  `~/.claude/usage-history.jsonl` are used when a live window is unavailable.
- Codex quota comes from local session logs and refreshes when Codex records a
  new quota snapshot.
- Antigravity provider quota comes from its CLI when available. The dashboard
  labels local rolling usage separately.

The pace estimate extrapolates the average usage rate in the current quota
window. A projection over 100% means that pace would exhaust quota before reset.
