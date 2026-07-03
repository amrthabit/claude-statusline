# Fonts

The statusline draws its icons from **Nerd Font v3** glyphs. For them to line
up in a terminal you need the **Mono** (`NFM`) variant of a Nerd Font, not the
regular one.

## Why the Mono variant

Nerd Fonts ship in three flavours:

- **NF** (regular / variable width) - many glyphs are drawn 1.5-2 cells wide.
  In a terminal (a fixed cell grid) they overflow their cell by up to ~39% and
  bleed into the next column, so icons look oversized, clipped, or misaligned -
  "wonky".
- **NFP** (proportional) - same problem, worse.
- **NFM** (Mono) - every glyph is squeezed to a single cell. This is the one a
  terminal needs; icons render at the right size and stay on the grid.

If you can't install a Mono variant, run with `CLAUDE_STATUSLINE_NERD=0` to fall
back to plain-text labels (`RAM`/`CPU`/...) instead of glyphs.

## Recommended font

**JetBrainsMono Nerd Font Mono** renders the full glyph set used here cleanly.
Grab it from the [Nerd Fonts](https://www.nerdfonts.com/font-downloads)
downloads (the archive's `*NerdFontMono-*.ttf` files), or on Linux:

```
# Debian/Ubuntu
sudo apt install fonts-jetbrains-mono   # base family
# then add the Nerd Font Mono patched build from nerdfonts.com for the glyphs
```

Any Nerd Font **Mono** variant works (FiraCode NFM, Hack NFM, CaskaydiaCove NFM,
...); JetBrainsMono NFM is just the one this was tuned against.

## Windows Terminal

Set the profile's `font.face` to the **NFM** face name (note the `NFM`, not
plain `NF`):

```json
"font":
{
    "face": "JetBrainsMono NFM"
}
```

Put that inside the relevant `profiles` entry (or `profiles.defaults` to apply
it everywhere) in Windows Terminal's `settings.json`. Using `"JetBrainsMono NF"`
here is the usual cause of oversized / misaligned icons - switch it to `NFM`.
