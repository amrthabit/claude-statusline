#!/usr/bin/env bash
# Local, no-CI equivalent of "regenerate the preview and show profiling
# numbers": build, test, print syscall/timing profiling, capture
# docs/statusline-preview.txt (real ANSI text - cat it in a terminal with
# the same Nerd Font to see it exactly as rendered, or paste it directly),
# and render docs/statusline-preview.png with VHS (GitHub doesn't render
# ANSI in a code block, so the README needs the image). Run manually with
# `make preview`, or automatically via the pre-commit hook - see
# githooks/pre-commit and the README for how to enable it.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

command -v vhs >/dev/null || {
    echo "preview.sh: vhs not found - see README prerequisites" >&2
    exit 1
}

make
make test

IN=$(python3 -c "
import json, time
now = int(time.time())
print(json.dumps({
    'session_id': 'preview',
    'model': {'display_name': 'Opus 4.8', 'id': 'claude-opus-4-8'},
    'workspace': {'current_dir': '$PWD', 'project_dir': '$HOME'},
    'context_window': {'used_percentage': 34, 'context_window_size': 200000},
    'effort': {'level': 'high'},
    'rate_limits': {
        'five_hour': {'used_percentage': 22, 'resets_at': now + 4*3600 + 9*60},
        'seven_day': {'used_percentage': 41, 'resets_at': now + 3*86400 + 2*3600},
    },
}))
")
printf '%s' "$IN" > docs/preview-input.json

echo "== profiling =="
STATE_DIR="${XDG_RUNTIME_DIR:-/dev/shm}"
rm -f "$STATE_DIR/claude-statusline-state-preview.dat" /dev/shm/claude-statusline-state-preview.dat
if command -v strace >/dev/null; then
    tmp=$(mktemp)
    echo "-- cold render (first ever) --"
    printf '%s' "$IN" | strace -c ./statusline-bin > /dev/null 2> "$tmp"; tail -1 "$tmp"
    echo "-- warm burst render (steady state) --"
    printf '%s' "$IN" | strace -c ./statusline-bin > /dev/null 2> "$tmp"; tail -1 "$tmp"
    rm -f "$tmp"
else
    echo "(strace not installed - skipping syscall counts)"
fi
echo "-- wall-clock, 10 renders --"
time (for _ in $(seq 1 10); do printf '%s' "$IN" | ./statusline-bin > /dev/null; done)

echo "== capturing docs/statusline-preview.txt =="
scripts/preview-frame.sh > docs/statusline-preview.txt
cat docs/statusline-preview.txt

echo "== rendering docs/statusline-preview.png =="
VHS_NO_SANDBOX=true vhs docs/preview.tape
rm -f ignored.gif
# Autocrop: VHS's canvas math leaves uneven margins (plus a dark window-edge
# band at the bottom); trim to the content with a small uniform border.
python3 - <<'PYEOF'
from PIL import Image, ImageChops
im = Image.open('docs/statusline-preview.png').convert('RGB')
bg = im.getpixel((5, 5))
diff = ImageChops.difference(im, Image.new('RGB', im.size, bg))
bbox = diff.convert('L').point(lambda v: 255 if v > 24 else 0).getbbox()
if bbox:
    m = 14
    l, t, r, b = bbox
    im.crop((max(0, l - m), max(0, t - m),
             min(im.width, r + m), min(im.height, b + m))
      ).save('docs/statusline-preview.png')
PYEOF
echo "done"
