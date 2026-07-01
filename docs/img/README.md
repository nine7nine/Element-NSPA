# Screenshots

The generated docs currently illustrate each subsystem with hand-authored SVG
diagrams, which render without any external assets. Screenshots are optional and
**not referenced yet** (so nothing shows a broken image). Drop PNGs here when you
want to add them, then paste a `<figure class="screenshot">` block into the
matching `.md` and re-run `./md2html.sh <page>.md`.

Figure block to paste (the theme already styles `figure.screenshot`):

```html
<figure class="screenshot">
  <img src="img/session-view-grid.png" alt="Session View clip grid with a scene launched">
  <figcaption>The Session View clip grid &mdash; columns are source nodes, rows are scenes.</figcaption>
</figure>
```

Suggested shots, one per page (capture the winelib app running under Wine-NSPA):

| Suggested file | Page | Shot |
| --- | --- | --- |
| `architecture-overview.png` | architecture | The main window with the graph editor and a couple of embedded plugin editors |
| `plugin-embed.png` | wayland-embedding | A Win32 VST/CLAP editor embedded inside the host window on Wayland |
| `graph-editor.png` | graph-engine | The modular graph: internal nodes + hosted plugins wired together |
| `tracker-grid.png` | tracker | The tracker pattern grid with FX columns and MUTE/SOLO |
| `pianoroll-editor.png` | pianoroll | The piano-roll with notes, the velocity lane, and a CC lane |
| `session-view-grid.png` | session-view | The clip grid with a scene launched and clips in their queued/playing states |
| `arrangement-timeline.png` | arrangement-timeline | Audio + MIDI lanes with regions, fades, and a volume envelope |
| `automation-overlay.png` | automation | The automation overlay under a lane with the chip rail and a bent curve |
| `sampler-editor.png` | sampler | The paged sampler editor (Bank / Inst / Sample / FX) with an FT2 envelope |

Filenames are referenced from the `.md` sources once you add a figure block; if you
rename a file, update the matching `<img src="img/...">` and re-run `md2html.sh`.
