# Scope: Game Boy emulator as a separate CardputerZero app

**Status: scoping only, not started.** This is deliberately not part of the
MonsterMesh app. It would be its own CardputerZero app, its own repo directory
and its own .deb, launched from APPLaunch next to MonsterMesh.

## Goal

Play the user's own Game Boy / Game Boy Color cartridge dumps (`.gb` / `.gbc`)
with their own battery saves (`.sav`) on the CardputerZero. Those saves are the
same files MonsterMesh's daemon reads and patches.

**No ROMs or BIOS images are shipped, bundled, downloaded or linked.** The app
opens with a file picker over a user directory such as `~/roms`. If the
directory is empty it explains how to copy your own dumps there.

## Why separate

* **Different runtime shape.** It needs a 60 fps frame loop, audio, and a
  160x144 → 320x170 scaler. The MonsterMesh app is an event-driven UI.
* **Licensing and distribution.** An emulator core has its own licence and
  release cadence. Keeping it out of the MonsterMesh .deb keeps that package
  simple.
* **Save safety.** The emulator writes `.sav` files while mmd may be watching
  them. Keeping that policy in one place, with documented hand-off rules, is
  easier than mixing it into the game UI.

## Sketch

| Area | Choice |
|---|---|
| Core | An existing permissively-licensed C core (e.g. **Peanut-GB**, MIT, single header; or **SameBoy** core, MIT, for GBC). Needs evaluating for CM0 performance at 512 MB / Cortex-A53. |
| App shell | Same CardputerZero Template (LVGL 9.5, presets, cp0-cross deb). LVGL draws the picker and menus only; the game frame is blitted straight to the fb with integer 2x vertical crop or a nearest-neighbour 160x144 → 188x170 scale. |
| Input | evdev keyboard: arrows / `;.,/` = D-pad, `z`/`x` or Enter/Backspace = A/B, Tab = Select, Space = Start, Esc = menu. |
| Audio | ALSA if the CardputerZero speaker path supports it; otherwise muted in v1. |
| Saves | `.sav` written on exit, on menu open, and every N minutes, atomically (temp file + rename). Use the same `save-backups/` convention as the Pi tooling. |
| MonsterMesh interop | Optional: after saving, notify mmd (it already watches the save directory). No direct coupling to the MonsterMesh app. |

## Open questions for the user

1. GB only, or GB + GBC? This decides between Peanut-GB and SameBoy.
2. Where do ROMs and saves live on the device? Is there a shared directory with mmd's watch dir?
3. Should mmd pause its save patching while the emulator is running?
4. Is audio needed in v1?
