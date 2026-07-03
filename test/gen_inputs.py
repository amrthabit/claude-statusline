#!/usr/bin/env python3
"""Generate parity-test inputs. Time-sensitive resets_at values are computed
relative to now with +30s margins so both implementations (run ms apart)
format the same minute."""
import json, sys, time, datetime as dt

out = sys.argv[1] if len(sys.argv) > 1 else "test/inputs"
now = time.time()

def w(name, obj):
    with open(f"{out}/{name}", "w") as f:
        if isinstance(obj, str):
            f.write(obj)
        else:
            json.dump(obj, f)

# (a) captured real-schema payload shape
w("a_real.json", {
    "session_id": "parity-a",
    "model": {"display_name": "Opus 4.8", "id": "claude-opus-4-8"},
    "workspace": {"current_dir": "/home/amr/.claude", "project_dir": "/home/amr"},
    "cwd": "/home/amr/.claude",
    "context_window": {"used_percentage": 18, "context_window_size": 200000},
    "effort": {"level": "high"},
    "rate_limits": {
        "five_hour": {"used_percentage": 2, "resets_at": int(now) + 2*3600 + 42*60 + 30},
        "seven_day": {"used_percentage": 3, "resets_at": int(now) + 31*3600 + 30},
    },
})

# (b) launch != current + long path needing middle elision
w("b_dirs.json", {
    "session_id": "parity-b",
    "model": {"display_name": "Fable 5"},
    "workspace": {
        "current_dir": "/home/amr/tickets/PHPG1138-305/references/deep/nest",
        "project_dir": "/home/amr/tickets/PHPG1138-305",
    },
    "context_window": {"used_percentage": 55},
    "effort": {"level": "low"},
})

# (c) free tier: no rate_limits, no context_window
w("c_free.json", {
    "session_id": "parity-c",
    "model": {"display_name": "Haiku 4.5"},
    "workspace": {"current_dir": "/home/amr", "project_dir": "/home/amr"},
})

# (d) empty stdin
w("d_empty.json", "")

# (e) garbage stdin
w("e_garbage.json", "this is { not json ]][")

# (f) ISO8601 + epoch-ms resets_at
iso = (dt.datetime.now(dt.timezone.utc)
       + dt.timedelta(hours=3, minutes=8, seconds=30)).strftime("%Y-%m-%dT%H:%M:%SZ")
w("f_iso.json", {
    "session_id": "parity-f",
    "model": {"display_name": "Opus 4.8"},
    "workspace": {"current_dir": "/home/amr", "project_dir": "/home/amr"},
    "rate_limits": {
        "five_hour": {"used_percentage": 42, "resets_at": iso},
        "seven_day": {"used_percentage": 7, "resets_at": int((now + 5*86400 + 30) * 1000)},
    },
})

# (g) threshold edges: ctx 80 red, 5h 79 amber, 7d 80 red; extended 1M context
w("g_edges.json", {
    "session_id": "parity-g",
    "model": {"id": "claude-fable-5"},   # no display_name -> id fallback
    "workspace": {"current_dir": "/home/amr", "project_dir": "/home/amr"},
    "context_window": {"used_percentage": 80, "context_window_size": 1000000},
    "effort": {"level": "xhigh"},
    "rate_limits": {
        "five_hour": {"used_percentage": 79, "resets_at": int(now) + 8*60 + 30},
        "seven_day": {"used_percentage": 80},   # no resets_at -> "?"
    },
})

# (h) ctx 70 amber boundary + missing model entirely
w("h_edge2.json", {
    "session_id": "parity-h",
    "workspace": {"current_dir": "/home/amr/x", "project_dir": "/home/amr/x"},
    "context_window": {"used_percentage": 70},
    "effort": {"level": "medium"},
})

# (i) JSON torture: escapes incl. \uXXXX + surrogate pair, arrays and unknown
# nesting to skip over, exponent-form numbers. json.dumps escapes non-ASCII
# by default, exercising the C decoder's \u path.
w("i_torture.json", {
    "session_id": "parity-i",
    "model": {"display_name": "Opüs \"4.8\" ⚡", "extra": [1, [2, {"x": []}], None, True]},
    "workspace": {"current_dir": "/home/amr/café 🚀/dir",
                  "project_dir": "/home/amr/café 🚀/dir"},
    "context_window": {"used_percentage": 1.8e1},
    "effort": {"level": "max"},
    "rate_limits": {
        "five_hour": {"used_percentage": 7.9e1, "resets_at": (int(now) + 3600 + 30) * 1000.0},
        "seven_day": {"used_percentage": 3, "resets_at": int(now) + 2*86400 + 30},
    },
    "unknown_deeply": {"a": {"b": [[[{"c": "d\\e"}]]]}},
})

# (j) truncated JSON must be rejected whole (renders like empty input)
w("j_truncated.json", '{"model":{"display_name":"Half')

# (k) unrecognized effort level (pass-through) + sub-1000 and mid-K token counts
w("k_effort.json", {
    "session_id": "parity-k",
    "model": {"display_name": "Sonnet 5"},
    "workspace": {"current_dir": "/home/amr", "project_dir": "/home/amr"},
    "context_window": {"used_percentage": 5, "context_window_size": 999},
    "effort": {"level": "turbo"},
})
print("inputs generated")
