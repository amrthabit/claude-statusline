#!/usr/bin/env bash
# Byte-for-byte parity: python reference vs C binary, on freshly generated
# inputs, with cold per-case state (fresh session ids ensure no rate segments,
# keeping output deterministic modulo the /proc/loadavg 5s tick).
set -u
cd "$(dirname "$0")/.."

PY="python3 -SE statusline.py"
BIN="./statusline-bin"
FAIL=0

python3 test/gen_inputs.py test/inputs >/dev/null

# cold state for every case, both formats
rm -f ~/.claude/cache/statusline-state-parity-*.json ~/.claude/cache/statusline-state-parity-*.dat \
      ~/.claude/cache/statusline-state-default.json ~/.claude/cache/statusline-state-default.dat

for f in test/inputs/*.json; do
  name=$(basename "$f")
  # run back-to-back to minimize clock skew; delete state between so BOTH are cold
  rm -f ~/.claude/cache/statusline-state-parity-*.json ~/.claude/cache/statusline-state-parity-*.dat \
      ~/.claude/cache/statusline-state-default.json ~/.claude/cache/statusline-state-default.dat
  out_py=$($PY < "$f")
  rm -f ~/.claude/cache/statusline-state-parity-*.json ~/.claude/cache/statusline-state-parity-*.dat \
      ~/.claude/cache/statusline-state-default.json ~/.claude/cache/statusline-state-default.dat
  out_c=$($BIN < "$f")
  if [ "$out_py" == "$out_c" ]; then
    echo "PASS  $name"
  else
    echo "FAIL  $name"
    echo "  py: $(printf '%s' "$out_py" | cat -v)"
    echo "   c: $(printf '%s' "$out_c" | cat -v)"
    diff <(printf '%s' "$out_py" | xxd) <(printf '%s' "$out_c" | xxd) | head -8 | sed 's/^/  /'
    FAIL=1
  fi
done

rm -f ~/.claude/cache/statusline-state-parity-*.json ~/.claude/cache/statusline-state-parity-*.dat \
      ~/.claude/cache/statusline-state-default.json ~/.claude/cache/statusline-state-default.dat
exit $FAIL
