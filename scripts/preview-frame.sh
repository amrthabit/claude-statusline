#!/usr/bin/env bash
# Mocks the bit of the real Claude Code TUI immediately around the
# statusline: an input line (prompt + cursor) between two full-width rules,
# then the statusline and the "accept edits" permission-mode footer, both
# indented 2 spaces. Layout and colors matched against a real screenshot.
# Not pixel-perfect - Claude Code's own chrome can change.
#
# --hold: after printing, hide the cursor and sleep, so a VHS screenshot
# captures the frame without a fresh shell prompt below it. The plain run
# (for the .txt capture) skips that.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

DIM=$'\033[2m'
VIO=$'\033[38;5;141m'   # #af87ff - sampled from Claude Code's real footer
REV=$'\033[7m'
RST=$'\033[0m'
W=$(stty size 2>/dev/null < /dev/tty | awk '{print $2}' || true)
W=${W:-${COLUMNS:-120}}

# Heavy box-drawing (U+2501/U+2503) instead of light: gnome-terminal's VTE
# draws light box lines at ~2x the thickness xterm/VHS does, so the light
# glyphs that look right in a real terminal render as hairlines in the
# screenshot. Heavy in the preview matches VTE's rendered weight.
rule=$(printf '\xe2\x94\x81%.0s' $(seq 1 "$W"))

printf '%s%s%s\n' "$DIM" "$rule" "$RST"
printf '\xe2\x9d\xaf %s %s\n' "$REV" "$RST"   # prompt + drawn block cursor
printf '%s%s%s\n' "$DIM" "$rule" "$RST"
printf '  '
# Claude Code's UI layer renders the statusline's bold, dim, AND
# default-color text at the same muted gray (#999999, sampled) - bold only
# differs by glyph weight; explicit colors pass through untouched.
# Replicate that: every reset re-establishes the gray as the ambient
# foreground (so default-color fragments like the IO/net rate values render
# gray, not bright white), and dim becomes the exact gray instead of the
# renderer's own darker 50%.
cat docs/preview-input.json | ./statusline-bin \
    | sed $'s/\033\\[2m/\033[38;2;153;153;153m/g; s/\033\\[0m/\033[0m\033[38;2;153;153;153m/g; s/\xe2\x94\x82/\xe2\x94\x83/g'
GRY=$'\033[38;2;153;153;153m'
printf '\n  %s\xe2\x8f\xb5\xe2\x8f\xb5 accept edits on%s %s(shift+tab to cycle)%s' \
    "$VIO" "$RST" "$GRY" "$RST"

if [ "${1:-}" = "--hold" ]; then
    printf '\033[?25l'   # hide the real cursor; the drawn one stands in
    sleep 3
else
    printf '\n'
fi
