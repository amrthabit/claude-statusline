# Width stability

The statusline refreshes once a second. Any field that changes *width* between
renders shoves everything to its right, so the whole line jitters. The fix is to
give the volatile fields a fixed width. The quiet fields are deliberately left
alone: a value that almost never crosses a digit boundary would only pay padding
for a reflow that never happens, and padding is chrome this statusline avoids.

So the rule is **pad in proportion to volatility**, not uniformly.

## What gets a fixed width, and why

| field | volatility | treatment |
|---|---|---|
| **Disk IO, network** (`R`/`W`, `↓`/`↑`) | changes every render, across many orders of magnitude | constant 4 chars (see below) |
| **CPU %** | high: load crosses the 9/10 boundary constantly | leading zero, 2 digits (`09%`) |
| **RAM %** | low: drifts slowly, effectively never single digit | left alone |
| **Disk %** | low: moves over minutes | left alone |
| **Context %, 5h / 7d** | low: grow monotonically within a session | left alone |
| **Battery %** | low | left alone |
| **CPU temp** | value moves, width doesn't: the package sensor sits well above 10°C in normal use, so it is always 2 digits | left alone (padding would only ever produce an `07°C` that never occurs) |

`100%` and `100°C` are still 3 wide on the rare occasions they happen, but they
are brief. The flicker worth killing is the single/double-digit boundary that a
volatile field trips many times a minute.

## The IO / network rate format

Rates are the worst case: they change every render and span idle bytes/s to
burst MiB/s. They are formatted to a **constant 4 characters**, a 3-char number
plus a 1-char unit:

- **< 10**: one decimal, e.g. `1.5K`, `0.0B`
- **100-999**: an integer, e.g. `120K`, `823B`
- **10-99**: the hard band. Two significant digits is one char short of the
  others, so instead of padding with a space it rolls up to a leading-dot
  fraction of the next unit: `12K` becomes `.01M`.

Why 10-99 is the odd one out: a number in a 3-char field can hold three
significant digits only when no decimal point takes a slot (`120`), or two
digits with a point (`1.5`). The 10-99 range wants exactly two digits and no
point, which fills only two chars; the third slot has to be either a space or a
rolled fraction. The space was rejected for the dense look, so it rolls.

Why not roll *every* value to a leading-dot fraction: base-1024 units are a
factor of ~1000 apart, and a two-decimal fraction (`.dd`) only reaches down to
1/100 of a unit. The 1-9 decade of any unit is smaller than that, so `1.5K`
would collapse to `.00M` and read as zero. That decade is exactly where idle
network traffic lives. So only 10-99 rolls; 1-9 and 100-999 keep their natural,
readable form.

This trades a little precision in the 10-99 band (`12K` and `15K` both land near
`.01M`) for a width that never moves. That precision is cheap for a live rate,
and the reflow it buys off is not.

Both `statusline.c` and `statusline.py` implement this identically; `make test`
checks they stay byte-for-byte equal.

## Rules of thumb

- **Leading zero, not leading space.** `09%`, not ` 9%`: same width, no
  whitespace inside a segment, consistent with the rest of the line.
- **Never hide magnitude to win width.** A fixed width that turns `1.5K` into
  `.00M` is worse than the reflow it removes. Width is a display convenience; the
  number is the point.
