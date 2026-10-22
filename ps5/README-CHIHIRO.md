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

## Switches: /data/xemu-chihiro/env.txt

Written the first time the app starts; NAME=VALUE per line, read at startup.

- `XEMU_WM1_*`, `XEMU_WM2_*`: the Maximum Tune export Gemballa patches
  (in memory; only on V307.xbe / V322.xbe export). On by default.
- `XEMU_WS_HACK=1`: 16:9 for the Chihiro games; set the game's screen shape
  to 16:9 too. `XEMU_WS_COPY`, `XEMU_WS_HUD`, `XEMU_WS_HUD_EDGE`,
  `XEMU_WS_MENU43`, `XEMU_WS_MENUBG` tune it (as the desktop build's .bat).
- `XEMU_DEPTH_READ_LAG`, `XEMU_ZETA_REUSE_LARGER`, `XEMU_CUBE_FROM_SURFACES`,
  `XEMU_GPU_BLIT`: PS5 speed-ups, on by default; 0 does it as XPSemu does.
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
