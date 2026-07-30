# Profile accent

Claude Code keeps its account, settings and session history in the directory
named by `CLAUDE_CONFIG_DIR` (default `~/.claude`). Point separate launchers at
separate config dirs and you can run several instances side by side as the same
Linux user - different accounts, independent session history, and, until now, an
identical statusline in both terminals.

This segment colors the `user@host:` prefix per config dir, so a glance at the
left of the line tells you which instance the terminal is talking to.

It is **off unless you create a config file**: with no config, every prefix
renders at the default intensity exactly as before.

## Config

Point `CLAUDE_STATUSLINE_PROFILE` at a file, or drop a `profile.conf` next to
the binary. One rule per line:

```
# config-dir      color
.claude-alt       blue
/home/me/.claude  green
-                 magenta
```

- **config-dir** - matched against `CLAUDE_CONFIG_DIR`, either in full or by
  basename (a trailing slash is ignored). The literal `-` matches a session
  running with `CLAUDE_CONFIG_DIR` unset, i.e. the default `~/.claude`.
- **color** - one of `red green yellow blue magenta cyan`. An unrecognized
  color falls back to the default intensity rather than breaking the line.

First matching rule wins. Lines starting with `#` and blank lines are ignored.
Delete the file to remove the accent entirely.

Because the accent is keyed on the environment rather than on settings, it works
even when the instances share one `settings.json` - a symlinked or otherwise
common settings file cannot express "blue here, plain there", but this can.

Keep `profile.conf` out of version control if its directory names are
sensitive - this repo's `.gitignore` already ignores it.

## Cost

One `getenv` and one read of a small file per render, on the same code path that
already reads the services config. No extra syscalls when the file is absent.
