# statusline

A fast, compact statusline for [Claude Code](https://claude.com/claude-code),
in one C file. Reads Claude's status JSON on stdin, samples `/proc` + `/sys`,
prints one color-coded line in ~**0.7 ms**.

![statusline](docs/statusline-full.png)

```
 launch ›  current │ Model ctx% 5h%reset 7d%reset │ BAT RAM CPU DISK IO NET
```

## Features

- **Directories**: launch and current dir, `~`-shortened and elided; collapsed when equal
- **Model + context**: active model name and context-window usage %
- **Plan usage**: 5h and 7d windows with reset countdowns (`20%4h9m`); subscribers only
- **Battery**: charge % with a level glyph; distinct glyph while charging
- **RAM & disk**: used % plus free space (`53%3.7G`)
- **CPU, disk IO, network**: true rates from `/proc` deltas, tracked per session without collisions
- **Zero disk writes**: state (~100 bytes/session) lives in tmpfs (`$XDG_RUNTIME_DIR`), vanishes at logout
- **Color as the signal**: green/amber/red at 70/80%; battery inverted; IO/net at 10/30MiB/s
- **Never breaks a render**: malformed stdin, missing fields, unreadable `/proc`: segments fail soft
- **Self-contained**: one `.c` file, nothing beyond libc; the JSON reader is ~180 lines of it
- **Lean by measurement**: 19 syscalls, zero allocations, zero writes per render

Built for a **Linux laptop** on a **Claude subscription**. Desktops, VMs, and
API-key billing still work: the missing segments just disappear.

## Prerequisites

- Linux with `/proc` and `/sys`; battery needs `/sys/class/power_supply/BAT*`
- `gcc`, `make`, libc headers (`sudo apt install gcc make libc6-dev`); optional `musl-tools` for ~10× faster init
- A Nerd Font (v3) for glyphs, or `CLAUDE_STATUSLINE_NERD=0` for plain text
- `make test` also needs `python3` and `bash`

## Build & deploy

```
make            # single .c, static; prefers musl-gcc, falls back to gcc
make test       # byte-for-byte parity vs the Python reference
```

Then point `statusLine` at the binary in `~/.claude/settings.json`:

```json
"statusLine": {
  "type": "command",
  "command": "/path/to/statusline/statusline-bin",
  "padding": 0,
  "refreshInterval": 1
}
```

## Tuning

Thresholds are `#define`s in the CONFIG block atop `statusline.c`. Change, `make`, done.

## Files

- `statusline.c`: the entire implementation
- `statusline.py`: Python reference; `test/parity.sh` diffs the two byte-for-byte
