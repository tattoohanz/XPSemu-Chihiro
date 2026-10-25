# XPSemu Chihiro (PPSA97372)

XPSemu (xemu for the PS5) with the Sega Chihiro machine from the xemu
widescreen fork (tattoohanz/xemu, branch widescreen, 1c529e7): Wangan
Midnight Maximum Tune 1/2 and the other Chihiro netboot images, next to Xbox
games. A separate title with its own folder: XPSemu (PPSA97358, /data/xemu)
is not touched.

## Install

1. Copy `PPSA97372/` to `/data/homebrew/` (ShadowMountPlus installs it).
2. Add a line `PPSA97372` to `/data/whitelist.txt`; load XPSemu's
   `helper.elf` with kstuff as for XPSemu.
3. Copy into `/data/xemu-chihiro/chihiro/`:
   `chihiro_xbox_bios.bin`, `fpr21042_m29w160et.bin`, `ic10_g24lc64.bin`,
   `pc20_g24lc64.bin`, `ic11_24lc024.bin`.
4. Copy the netboot images (`.bin`) into `/data/xemu-chihiro/games/`
   (or `xemu/games/` on a USB drive).
5. Start XPSemu Chihiro, Games, pick the game. A Chihiro game restarts the
   app as a Chihiro with that image. Triangle on a game: Home screen
   shortcut makes a tile that starts it directly.

Xbox games work as in XPSemu (MCPX and BIOS in `/data/xemu-chihiro/`).

## The arcade front

The app opens on a game select in the style of SEGA ALL.Net P-ras MULTI
Ver.3, with the Chihiro games only (`XEMU_ARCADE_UI=0` in env.txt brings
back the dashboard with the Xbox games). The games are named from the
ID in their `boot.id`, whatever the file is called, with MAME's names.

- Left / right choose (L1 / R1 five at a time), Cross plays (or goes back
  to the game running), Circle goes back to the game running, Square ends
  it, Triangle opens the game's settings (its name, a home screen tile),
  Options the emulator's settings (L1 / R1 there: Settings, System).
- Tiles: a picture per game in `/data/xemu-chihiro/arcade/`, `.png` or
  `.jpg`, named after the game's MAME set (`wangmid2.jpg`) or its name
  (`Wangan Midnight Maximum Tune 2.png`); a flyer's shape fits best, any
  size. A game without one shows its name.
- Logos: a game's logo in `/data/xemu-chihiro/arcade/logos/`, named as the
  tiles (`wangmid2.png`; a transparent PNG suits the white), shown under
  the row instead of the selected game's name.
- The Chihiro logo at the top left: `/data/xemu-chihiro/arcade/logo.png`
  (or `.jpg`; a transparent PNG suits the blue bar), fitted into the bar.
- The settings pages (Options, Triangle) open over the same ground in its
  colours, the page's name in the banner and its buttons in the bottom bar.

## Switches: /data/xemu-chihiro/env.txt

Written the first time the app starts; NAME=VALUE per line, read at startup.

- `XEMU_WM1_*`, `XEMU_WM2_*`: the Maximum Tune export Gemballa patches
  (in memory; only on V307.xbe / V322.xbe export). On by default.
- `XEMU_WM1_WIDESCREEN=1`, `XEMU_WM2_WIDESCREEN=1`: Maximum Tune 1/2 export
  draws 16:9 itself (its camera's aspect and field of view, in memory), so
  the sides are drawn by the game. Give the game the 16:9 screen shape and
  `XEMU_WS_HACK=2` for its HUD and menus. Off by default.
- `XEMU_WS_HACK=1`: 16:9 for the Chihiro games; set the game's screen shape
  to 16:9 too. `XEMU_WS_COPY`, `XEMU_WS_HUD`, `XEMU_WS_HUD_EDGE`,
  `XEMU_WS_MENU43`, `XEMU_WS_MENUBG` tune it (as the desktop build's .bat).
  `XEMU_WS_HACK=2` does the same to the 2D only (the programmable shaders;
  these games draw their 3D with the fixed-function pipeline), for a game
  whose 3D is made 16:9 by its own patch: in env.txt's section for that
  game, e.g. `[wangmid2]` with `XEMU_WM2_WIDESCREEN=1` and `XEMU_WS_HACK=2`.
- `XEMU_DEPTH_READ_LAG`, `XEMU_ZETA_REUSE_LARGER`, `XEMU_CUBE_FROM_SURFACES`,
  `XEMU_GPU_BLIT`: PS5 speed-ups, on by default; 0 does it as XPSemu does.
- `XEMU_CHIHIRO_FFB=100`: the drive board's force feedback as DualSense
  rumble, in percent (0 = off, up to 200).
- `XEMU_CHIHIRO_LOG=boot,jvs,card,...`: Chihiro log categories in xemu.log.

## Controls (Chihiro)

| DualSense | Maximum Tune / OutRun 2 |
|---|---|
| Left stick | Steering |
| R2 / L2 | Gas / brake |
| R1 / L1 | Gear up / down |
| Triangle | View change |
| Square | Second wheel button (Maximum Tune) |
| Circle | Card in (Maximum Tune) |
| Options or Cross | Start |
| D-pad up | Coin (Create too, but it opens the PS5 capture menu) |
| D-pad right / left | Test / Service |
| Touchpad | XPSemu's menu |

## Logs

`/data/xemu-chihiro/xemu.log`, `/data/xemu-chihiro/xemu-game.log`.
