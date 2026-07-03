#!/usr/bin/env python3
"""
Claude Code statusline.
Reads the status JSON on stdin, reads cheap system metrics from /proc,
prints one compact, color-coded line.

Segments:  launch>current dir | model ctx% 5h 7d | RAM CPU DISK IO NET

Everything is configurable in the CONFIG block below.
"""
import sys, os, json, time, re

HOME = os.path.expanduser("~")
CACHE_DIR = os.path.join(HOME, ".claude", "cache")

def state_path(session_id):
    """Per-session state file so concurrent Claude sessions don't stomp each
    other's cpu/net/io delta snapshots."""
    sid = re.sub(r"[^A-Za-z0-9_-]", "_", str(session_id or "default"))[:64]
    return os.path.join(CACHE_DIR, f"statusline-state-{sid}.json")

# ---------------------------------------------------------------- CONFIG ----
# Set NERD=False (or env CLAUDE_STATUSLINE_NERD=0) if you have no Nerd Font.
NERD = os.environ.get("CLAUDE_STATUSLINE_NERD", "1") != "0"

# Color thresholds (percent). >=WARN -> amber, >=BAD -> red, else green.
SYS_WARN, SYS_BAD = 70, 80        # RAM / CPU / disk / context
USE_WARN, USE_BAD = 70, 80        # 5h / 7d subscription usage

# Minimum seconds between recomputing rate metrics (cpu/net/io). Between
# recomputes the last computed rate is reused, so the line stays stable and
# the script stays cheap even when Claude re-renders many times a second.
RATE_MIN_INTERVAL = 1.0

# Throughput thresholds for IO / network (bytes per second). Below WARN uses the
# terminal's standard color; >=WARN amber; >=BAD red.
RATE_WARN = 10 * 1024 * 1024      # 10 MiB/s
RATE_BAD  = 30 * 1024 * 1024      # 30 MiB/s

SEP = "  "                        # segment separator
# ----------------------------------------------------------------------------

# ANSI
R   = "\033[0m"
DIM = "\033[2m"
BOLD= "\033[1m"
GRN = "\033[32m"
YLW = "\033[33m"
RED = "\033[31m"
CYN = "\033[36m"
BLU = "\033[34m"
MAG = "\033[35m"

def c(s, color):
    return f"{color}{s}{R}"

def pct_color(p):
    if p is None: return DIM
    if p >= SYS_BAD: return RED
    if p >= SYS_WARN: return YLW
    return GRN

def use_color(p):
    if p is None: return DIM
    if p >= USE_BAD: return RED
    if p >= USE_WARN: return YLW
    return GRN

def bat_color(pct, charging):
    # Inverted vs usage: a LOW battery is bad.
    if charging: return GRN
    if pct <= 15: return RED
    if pct <= 30: return YLW
    return GRN

def rate_color(bps):
    # Standard color until throughput crosses the amber/red thresholds.
    if bps is None: return ""
    if bps >= RATE_BAD:  return RED
    if bps >= RATE_WARN: return YLW
    return ""

# Glyphs: Nerd Font v3 (Font Awesome core + Material 8-hex), with plain fallbacks.
if NERD:
    G = {
        "launch": "",          # fa home
        "cur":    "",          # fa folder-open
        "model":  "",                # (no icon)
        "ctx":    "",                # (no icon)
        "t5h":    "",                # (no icon)
        "t7d":    "",                # (no icon)
        "bat":    "",                # (chosen by level, see bat_glyph)
        "ram":    "\U000F0F58",      # md memory
        "cpu":    "",          # fa microchip
        "disk":   "\U000F02CA",      # md harddisk
        "io":     "",          # fa exchange
        "net":    "\U000F0200",      # md ethernet
        "arrow":  "",          # fa angle-right
    }
else:
    G = {
        "launch": "L", "cur": "D", "model": "M", "ctx": "ctx",
        "t5h": "5h", "t7d": "7d", "bat": "BAT", "ram": "RAM", "cpu": "CPU",
        "disk": "DSK", "io": "IO", "net": "NET", "arrow": ">",
    }

# -------------------------------------------------------------- helpers ----
def human(n):
    """Bytes -> compact human string (no decimals for big units)."""
    n = float(n)
    for unit in ("B", "K", "M", "G", "T"):
        if n < 1024 or unit == "T":
            if unit == "B":
                return f"{int(n)}{unit}"
            if n < 10:
                return f"{n:.1f}{unit}"
            return f"{int(round(n))}{unit}"
        n /= 1024.0

def gb(n):
    return f"{n/1e9:.1f}G"

def shorten(path, maxlen=30):
    if not path:
        return ""
    if path == HOME:
        return "~"
    if path.startswith(HOME + "/"):
        path = "~" + path[len(HOME):]
    if len(path) <= maxlen:
        return path
    parts = path.split("/")
    if len(parts) <= 3:
        return path
    # keep leading ~ or first element + last two
    head = parts[0] if parts[0] in ("~", "") else parts[0]
    tail = "/".join(parts[-2:])
    return f"{head}/…/{tail}"

def fmt_delta(seconds):
    """Seconds -> 2d13h / 3h8m / 8m / <1m / now."""
    if seconds is None:
        return "?"
    s = int(seconds)
    if s <= 0:
        return "now"
    d, s = divmod(s, 86400)
    h, s = divmod(s, 3600)
    m, s = divmod(s, 60)
    if d:  return f"{d}d{h}h"
    if h:  return f"{h}h{m}m"
    if m:  return f"{m}m"
    return "<1m"

def parse_reset(v):
    """resets_at -> seconds until reset. Accepts epoch (s or ms) or ISO8601."""
    if v is None:
        return None
    now = time.time()
    try:
        n = float(v)
        if n > 1e12:      # ms
            n /= 1000.0
        return n - now
    except (TypeError, ValueError):
        pass
    try:
        s = str(v).replace("Z", "+00:00")
        import datetime as dt
        t = dt.datetime.fromisoformat(s)
        if t.tzinfo is None:
            t = t.replace(tzinfo=dt.timezone.utc)
        return t.timestamp() - now
    except Exception:
        return None

def dig(d, *path, default=None):
    cur = d
    for k in path:
        if isinstance(cur, dict) and k in cur:
            cur = cur[k]
        else:
            return default
    return cur

# ------------------------------------------------------------ /proc read ----
def read_battery():
    """Return (percent, charging) or (None, None) if no battery."""
    base = None
    psdir = "/sys/class/power_supply"
    try:
        for name in sorted(os.listdir(psdir)):
            if name.startswith("BAT") and os.path.exists(f"{psdir}/{name}/capacity"):
                base = f"{psdir}/{name}"
                break
    except OSError:
        return None, None
    if not base:
        return None, None
    try:
        with open(base + "/capacity") as f:
            pct = int(f.read().strip())
    except (OSError, ValueError):
        return None, None
    status = ""
    try:
        with open(base + "/status") as f:
            status = f.read().strip()
    except OSError:
        pass
    return pct, status in ("Charging", "Full")

def bat_glyph(pct, charging):
    if not NERD:
        return G["bat"]                      # "BAT"
    if charging:
        return chr(0xF0084)                  # md battery-charging
    # Font Awesome battery ramp: U+F240 full .. U+F244 empty (safe FA core range)
    idx = 0 if pct >= 88 else 1 if pct >= 63 else 2 if pct >= 38 else 3 if pct >= 13 else 4
    return chr(0xF240 + idx)

def read_mem():
    total = avail = None
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemTotal:"):
                    total = int(line.split()[1]) * 1024
                elif line.startswith("MemAvailable:"):
                    avail = int(line.split()[1]) * 1024
                if total and avail:
                    break
    except OSError:
        return None, None
    if not total:
        return None, None
    used_pct = (total - avail) / total * 100.0
    return used_pct, avail  # avail bytes = "free"

def read_cpu_snapshot():
    try:
        with open("/proc/stat") as f:
            parts = f.readline().split()
        vals = list(map(int, parts[1:]))
        idle = vals[3] + (vals[4] if len(vals) > 4 else 0)  # idle + iowait
        total = sum(vals)
        return total, idle
    except (OSError, ValueError, IndexError):
        return None

def read_net_snapshot():
    rx = tx = 0
    try:
        with open("/proc/net/dev") as f:
            for line in f.readlines()[2:]:
                iface, _, rest = line.partition(":")
                iface = iface.strip()
                if iface == "lo" or iface.startswith(("veth", "docker", "br-", "vnet")):
                    continue
                fields = rest.split()
                if len(fields) >= 9:
                    rx += int(fields[0])
                    tx += int(fields[8])
    except (OSError, ValueError):
        return None
    return rx, tx

DISK_RE = re.compile(r"^(nvme\d+n\d+|sd[a-z]+|vd[a-z]+|mmcblk\d+|xvd[a-z]+)$")
def read_io_snapshot():
    rd = wr = 0
    try:
        with open("/proc/diskstats") as f:
            for line in f:
                p = line.split()
                if len(p) < 10:
                    continue
                name = p[2]
                if not DISK_RE.match(name):
                    continue
                rd += int(p[5]) * 512    # sectors read
                wr += int(p[9]) * 512    # sectors written
    except (OSError, ValueError):
        return None
    return rd, wr

def read_disk():
    try:
        st = os.statvfs("/")
    except OSError:
        return None, None
    total = st.f_blocks * st.f_frsize
    free  = st.f_bfree  * st.f_frsize
    avail = st.f_bavail * st.f_frsize
    used  = total - free
    denom = used + avail
    pct = used / denom * 100.0 if denom else 0.0
    return pct, avail

def loadavg_pct():
    try:
        with open("/proc/loadavg") as f:
            one = float(f.read().split()[0])
        n = os.cpu_count() or 1
        return one / n * 100.0
    except (OSError, ValueError):
        return None

# ---------------------------------------------------------------- state ----
def load_state(path):
    try:
        with open(path) as f:
            return json.load(f)
    except Exception:
        return {}

def save_state(path, st):
    try:
        tmp = path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(st, f)
        os.replace(tmp, path)
    except OSError:
        pass

def rate_metrics(state):
    """Compute cpu%, net rx/tx B/s, io r/w B/s using deltas vs saved state.
    Reuses cached results if the last sample is younger than RATE_MIN_INTERVAL."""
    now = time.time()
    prev_ts = state.get("ts", 0)
    dt = now - prev_ts
    cached = state.get("rates", {})

    cpu_now = read_cpu_snapshot()
    net_now = read_net_snapshot()
    io_now  = read_io_snapshot()

    # Not enough time elapsed -> reuse last computed rates, keep old snapshot.
    if cached and dt < RATE_MIN_INTERVAL and dt >= 0:
        return cached, state  # unchanged state

    rates = dict(cached)  # start from last known so we never regress to blank

    # CPU
    pc = state.get("cpu")
    if cpu_now and pc and dt > 0:
        dtot = cpu_now[0] - pc[0]
        didle = cpu_now[1] - pc[1]
        if dtot > 0:
            rates["cpu"] = max(0.0, min(100.0, (1 - didle / dtot) * 100.0))

    # NET
    pn = state.get("net")
    if net_now and pn and dt > 0:
        rates["rx"] = max(0.0, (net_now[0] - pn[0]) / dt)
        rates["tx"] = max(0.0, (net_now[1] - pn[1]) / dt)

    # IO
    pio = state.get("io")
    if io_now and pio and dt > 0:
        rates["ior"] = max(0.0, (io_now[0] - pio[0]) / dt)
        rates["iow"] = max(0.0, (io_now[1] - pio[1]) / dt)

    new_state = {
        "ts": now,
        "cpu": cpu_now or state.get("cpu"),
        "net": net_now or state.get("net"),
        "io":  io_now  or state.get("io"),
        "rates": rates,
    }
    return rates, new_state

# ----------------------------------------------------------------- main ----
def main():
    raw = sys.stdin.read()
    try:
        data = json.loads(raw)
    except Exception:
        data = {}

    segs = []

    # --- dirs ---
    current = dig(data, "workspace", "current_dir") or data.get("cwd") or os.getcwd()
    launch  = dig(data, "workspace", "project_dir") or current
    cur_s = shorten(current)
    if os.path.normpath(current) != os.path.normpath(launch):
        dirs = (c(G["launch"], DIM) + " " + c(shorten(launch), DIM)
                + " " + c(G["arrow"], DIM) + " "
                + c(G["cur"], CYN) + " " + c(cur_s, BOLD))
    else:
        dirs = c(G["cur"], CYN) + " " + c(cur_s, BOLD)
    segs.append(dirs)

    # --- model + context + usage ---
    model = dig(data, "model", "display_name") or dig(data, "model", "id") or "?"
    mstr = c(model, BOLD)
    if G["model"]:
        mstr = c(G["model"], MAG) + " " + mstr
    mid = [mstr]

    ctx = dig(data, "context_window", "used_percentage")
    if ctx is None:
        ctx = dig(data, "context_window", "usedPercentage")
    if isinstance(ctx, (int, float)):
        cstr = c(f"{int(round(ctx))}%", pct_color(ctx))
        if G["ctx"]:
            cstr = c(G["ctx"], DIM) + cstr
        mid.append(cstr)

    def usage_seg(node, glyph):
        used = dig(data, "rate_limits", node, "used_percentage")
        if used is None:
            used = dig(data, "rate_limits", node, "usedPercentage")
        if not isinstance(used, (int, float)):
            return None
        reset = dig(data, "rate_limits", node, "resets_at")
        if reset is None:
            reset = dig(data, "rate_limits", node, "resetsAt")
        delta = parse_reset(reset)
        col = use_color(used)
        label = fmt_delta(delta)
        out = c(f"{int(round(used))}%", col) + c(f"{label}", DIM)
        if glyph:
            out = c(glyph, DIM) + out
        return out

    for node, glyph in (("five_hour", G["t5h"]), ("seven_day", G["t7d"])):
        s = usage_seg(node, glyph)
        if s:
            mid.append(s)
    segs.append(" ".join(mid))

    # --- system metrics ---
    sfile = state_path(data.get("session_id"))
    state = load_state(sfile)
    rates, new_state = rate_metrics(state)
    if new_state is not state:
        save_state(sfile, new_state)

    sysparts = []

    bat_pct, bat_chg = read_battery()
    if bat_pct is not None:
        bcol = bat_color(bat_pct, bat_chg)
        sysparts.append(c(bat_glyph(bat_pct, bat_chg), bcol) + " " +
                        c(f"{bat_pct}%", bcol))

    ram_pct, ram_free = read_mem()
    if ram_pct is not None:
        sysparts.append(c(G["ram"], DIM) + " " +
                        c(f"{int(round(ram_pct))}%", pct_color(ram_pct)) +
                        c(f"{gb(ram_free)}", DIM))

    cpu = rates.get("cpu")
    if cpu is None:
        cpu = loadavg_pct()   # first-render fallback until a delta exists
    if cpu is not None:
        sysparts.append(c(G["cpu"], DIM) + " " +
                        c(f"{int(round(cpu))}%", pct_color(cpu)))

    disk_pct, disk_free = read_disk()
    if disk_pct is not None:
        sysparts.append(c(G["disk"], DIM) + " " +
                        c(f"{int(round(disk_pct))}%", pct_color(disk_pct)) +
                        c(f"{gb(disk_free)}", DIM))

    if "ior" in rates or "iow" in rates:
        iorb = rates.get("ior", 0); iowb = rates.get("iow", 0)
        sysparts.append(c(G["io"], DIM) + " " +
                        c("R", DIM) + c(human(iorb), rate_color(iorb)) +
                        c("W", DIM) + c(human(iowb), rate_color(iowb)))

    if "rx" in rates or "tx" in rates:
        rxb = rates.get("rx", 0); txb = rates.get("tx", 0)
        sysparts.append(c(G["net"], DIM) + " " +
                        c(f"↓{human(rxb)}", rate_color(rxb)) +
                        c(f"↑{human(txb)}", rate_color(txb)))

    if sysparts:
        segs.append(" ".join(sysparts))

    dsep = c("│", DIM)
    sys.stdout.write((f" {dsep} ").join(segs))

if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        # Never let the statusline crash Claude's render.
        sys.stdout.write(f"\033[2mstatusline error: {e}\033[0m")
