# statusline

Claude Code statusline in C. Reads the status JSON on stdin, reads system
metrics from `/proc` + `/sys`, prints one compact color-coded line:

![statusline](docs/statusline-full.png)

```
 launch ›  current │ Model ctx% 5h%reset 7d%reset │ BAT RAM CPU DISK IO NET
```

Designed for a **Linux laptop**: it assumes `/proc` + `/sys/class/power_supply`
(Linux-only — no macOS/BSD), shows a battery segment, and skips virtual
NICs/loop devices when summing net and disk IO. On a desktop or VM it still
works — the battery segment just disappears (every segment fails soft).

## Prerequisites

- **Linux** with `/proc` and `/sys` (any modern distro; battery segment needs
  `/sys/class/power_supply/BAT*`)
- **Build**: `gcc`, `make`, libc headers — Debian/Ubuntu:
  `sudo apt install gcc make libc6-dev`
- **Runtime**: none — plain libc binary, cJSON is vendored and compiled in
- **A Nerd Font (v3)** in the terminal for glyphs (e.g. JetBrainsMono Nerd
  Font); without one set `CLAUDE_STATUSLINE_NERD=0` for plain-text labels
- `make test` (optional) additionally needs `python3` and `bash` for the
  parity harness against the Python reference

- **~1.8 ms/render** (26× the Python reference), fine for `refreshInterval: 1`.
- Green / amber ≥70% / red ≥80% for RAM, CPU, disk, context, and plan usage.
  Battery inverted (amber ≤30%, red ≤15%, green while charging).
  IO/NET default color, amber ≥10 MiB/s, red ≥30 MiB/s.
- Plan usage (5h/7d windows + reset countdown) comes from `rate_limits` in the
  stdin JSON — official data, no transcript scraping.
- CPU/NET/IO are true rates from `/proc` deltas, persisted per-session in
  `~/.claude/cache/statusline-state-<session_id>.dat` (atomic tmp+rename).
- Never crashes the render: malformed stdin, missing fields, unreadable /proc,
  corrupt state — every segment fails soft and is simply omitted.
- Nerd Font v3 glyphs; set `CLAUDE_STATUSLINE_NERD=0` for plain-text labels.

## Build

```
make            # gcc -O2 -Wall -Wextra, links vendor/cJSON
make test       # byte-for-byte parity vs statusline.py reference
```

## Deploy

`~/.claude/settings.json`:

```json
"statusLine": {
  "type": "command",
  "command": "/home/amr/statusline/statusline-bin",
  "padding": 0,
  "refreshInterval": 1
}
```

## Tuning

Thresholds are `#define`s in the CONFIG block at the top of `statusline.c`
(SYS_WARN/SYS_BAD, USE_WARN/USE_BAD, RATE_WARN/RATE_BAD, RATE_MIN_INTERVAL).
Rebuild with `make` after changing.

## Files

- `statusline.c` — the implementation
- `statusline.py` — original Python implementation, kept as the reference spec;
  `test/parity.sh` diffs the two byte-for-byte on generated inputs
- `vendor/cJSON.{c,h}` — vendored [cJSON](https://github.com/DaveGamble/cJSON) v1.7.18 (MIT)
