# Native source provenance

Historical evidence belongs here; the native APIs describe their value and ownership contracts without console implementation details. The purity checker still checks every source line, including comments.

- `runtime/lift/rwim/immediate.h`: the historical caller is the RenderWare GameCube driver scaffold in `runtime/scaffold/rw_driver`, which decodes the game's vertex records. NE-RW5/R4 is intended to retire that caller; the same immediate-mode API accepts native callers. This is provenance, not evidence that the scaffold has already been removed.
- `runtime/lift/ui/hud_layout.cpp`: fixed gameplay glyph alignment points were recovered from the layout table at `0x805F7828..0x805F78E8`.
- `runtime/lift/ui/hud_layout.h`: the mission objective window is `fn_800B8ACC`, return `0x800B8AFC`. Its two counters use the table at `0x80523610`, record stride 104, with X positions 24 and 545. Per-cast anchoring keeps each panel with its corresponding counter.
