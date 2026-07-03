#!/usr/bin/env bash
# Byte-for-byte parity: python reference vs C binary, on freshly generated
# inputs, with cold per-case state (fresh session ids ensure no rate segments,
# keeping output deterministic modulo the /proc/loadavg 5s tick).
#
# Two fields are normalized before comparing (see `normalize` below), not
# byte-for-byte by nature rather than by bug: CPU package temp is read live
# with no state gating, so it can visibly shift between the two back-to-back
# calls; the active-session count depends on each implementation's own state
# directory (tmpfs for the C binary, ~/.claude/cache for the Python
# reference), whose real current contents differ regardless of the fixture.
set -u
cd "$(dirname "$0")/.."

PY="python3 -SE statusline.py"
BIN="./statusline-bin"
FAIL=0
ESC=$'\x1b'

# Trailing session count is the last field on the line (...<count>\033[0m); it
# differs between the two implementations' state dirs, so mask it. (Intensity
# is flattened, so it is no longer wrapped in a \033[2m marker to anchor on.)
normalize() {
  sed -E "s/[0-9]+°C/N°C/g; s/[0-9]+(${ESC}\[0m)\$/N\1/"
}

python3 test/gen_inputs.py test/inputs >/dev/null

# cold state for every case, both formats
rm -f ~/.claude/cache/statusline-state-parity-*.json ~/.claude/cache/statusline-state-default.json \
      "${XDG_RUNTIME_DIR:-/dev/shm}"/claude-statusline-state-parity-*.dat \
      "${XDG_RUNTIME_DIR:-/dev/shm}"/claude-statusline-state-default.dat \
      /dev/shm/claude-statusline-state-parity-*.dat /dev/shm/claude-statusline-state-default.dat

for f in test/inputs/*.json; do
  name=$(basename "$f")
  # run back-to-back to minimize clock skew; delete state between so BOTH are cold
  rm -f ~/.claude/cache/statusline-state-parity-*.json ~/.claude/cache/statusline-state-default.json \
      "${XDG_RUNTIME_DIR:-/dev/shm}"/claude-statusline-state-parity-*.dat \
      "${XDG_RUNTIME_DIR:-/dev/shm}"/claude-statusline-state-default.dat \
      /dev/shm/claude-statusline-state-parity-*.dat /dev/shm/claude-statusline-state-default.dat
  out_py=$($PY < "$f")
  rm -f ~/.claude/cache/statusline-state-parity-*.json ~/.claude/cache/statusline-state-default.json \
      "${XDG_RUNTIME_DIR:-/dev/shm}"/claude-statusline-state-parity-*.dat \
      "${XDG_RUNTIME_DIR:-/dev/shm}"/claude-statusline-state-default.dat \
      /dev/shm/claude-statusline-state-parity-*.dat /dev/shm/claude-statusline-state-default.dat
  out_c=$($BIN < "$f")
  norm_py=$(printf '%s' "$out_py" | normalize)
  norm_c=$(printf '%s' "$out_c" | normalize)
  if [ "$norm_py" == "$norm_c" ]; then
    echo "PASS  $name"
  else
    echo "FAIL  $name"
    echo "  py: $(printf '%s' "$out_py" | cat -v)"
    echo "   c: $(printf '%s' "$out_c" | cat -v)"
    diff <(printf '%s' "$out_py" | xxd) <(printf '%s' "$out_c" | xxd) | head -8 | sed 's/^/  /'
    FAIL=1
  fi
done

rm -f ~/.claude/cache/statusline-state-parity-*.json ~/.claude/cache/statusline-state-default.json \
      "${XDG_RUNTIME_DIR:-/dev/shm}"/claude-statusline-state-parity-*.dat \
      "${XDG_RUNTIME_DIR:-/dev/shm}"/claude-statusline-state-default.dat \
      /dev/shm/claude-statusline-state-parity-*.dat /dev/shm/claude-statusline-state-default.dat
exit $FAIL
