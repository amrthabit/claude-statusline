Output ignored.gif
Set Shell "bash"
Set FontFamily "JetBrainsMono Nerd Font Mono"
Set FontSize 32
Set Width 3840
Set Height 330
Set Padding 10
# Colors sampled from a real gnome-terminal screenshot (GNOME stock palette
# on the Ubuntu aubergine background), so the preview matches the actual
# terminal rather than a stock VHS theme.
Set Theme { "name": "gnome-aubergine", "background": "#300a24", "foreground": "#ffffff", "cursor": "#ffffff", "black": "#171421", "red": "#c01c28", "green": "#26a269", "yellow": "#a2734c", "blue": "#12488b", "magenta": "#a347ba", "cyan": "#2aa1b3", "white": "#d0cfcc", "brightBlack": "#5e5c64", "brightRed": "#f66151", "brightGreen": "#33da7a", "brightYellow": "#e9ad0c", "brightBlue": "#2a7bde", "brightMagenta": "#c061cb", "brightCyan": "#33c7de", "brightWhite": "#ffffff" }
Hide
Type "clear && scripts/preview-frame.sh --hold"
Enter
Sleep 500ms
Show
Sleep 500ms
Screenshot "docs/statusline-preview.png"
