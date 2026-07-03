# Services segment

An optional, opt-in segment that shows one colored dot per service - **green**
when its systemd unit is active, **red** otherwise - so you can watch a local
stack (database, app server, search, ...) from the statusline.

It is **off unless you create a config file**. With no config, the segment
(and every `systemctl` call and cache write behind it) is skipped entirely, so
anyone who doesn't want it pays nothing.

## Config

Point `CLAUDE_STATUSLINE_SERVICES` at a file, or drop a `services.conf` next to
the binary. One service per line:

```
# label  systemd-unit        icon
db       postgresql          U+F1C0
web      nginx               U+F0AC
cache    redis-server        U+F1C0
```

- **label** - shown when no Nerd Font is in use (`CLAUDE_STATUSLINE_NERD=0`), or
  when no icon is given.
- **unit** - the systemd unit name passed to `systemctl is-active` (the
  `.service` suffix is optional).
- **icon** - optional glyph, either `U+XXXX` / `0xXXXX` hex or a literal glyph.
  Shown (colored by state) when a Nerd Font is active. Pick per-service icons
  from [nerdfonts.com/cheat-sheet](https://www.nerdfonts.com/cheat-sheet).

Lines starting with `#` and blank lines are ignored. Delete the file to remove
the segment.

Keep `services.conf` out of version control if its unit names are sensitive -
this repo's `.gitignore` already ignores it.

## Cost

Liveness is a single `systemctl is-active <all units>` call, cached for
`SVC_MIN_INTERVAL` (15 s) in a small tmpfs file shared between the C and Python
builds. A burst of renders reuses the cache instead of forking, and a stale
cache is still correct because liveness changes slowly. Any failure (no
systemd, exec error, timeout) reads as all-down rather than breaking the line.
