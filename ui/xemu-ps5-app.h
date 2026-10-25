/*
 * XPSemu Chihiro: which PS5 title this build is and where it keeps its files.
 *
 * XPSemu (PPSA97358, /data/xemu) is the Xbox emulator this is built on;
 * this build is a separate title with its own folder, so the two can be
 * installed side by side and neither touches the other's settings, disks
 * or saves.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#ifndef XEMU_PS5_APP_H
#define XEMU_PS5_APP_H

/* Must match ps5/sce_sys/param.json (titleId, conceptId, contentId). */
#define XEMU_PS5_TITLE_ID "PPSA97372"
#define XEMU_PS5_CONCEPT_ID "97372"
#define XEMU_PS5_CONTENT_SUFFIX "XPSCHIHIRO000001"
#define XEMU_PS5_TITLE_NAME "XPSemu Chihiro"

/* Everything the app writes, and the Xbox/Chihiro files the user copies. */
#define XEMU_PS5_DATA_DIR "/data/xemu-chihiro"
/* The Chihiro BIOS and the media board files (chihiro_xbox_bios.bin,
 * fpr21042_m29w160et.bin, ic10_g24lc64.bin, pc20_g24lc64.bin,
 * ic11_24lc024.bin). */
#define XEMU_PS5_CHIHIRO_DIR XEMU_PS5_DATA_DIR "/chihiro"
/* Environment switches read at startup (NAME=VALUE per line): the
 * XEMU_WM1_*, XEMU_WM2_* and XEMU_WS_HACK of the desktop build's .bat. */
#define XEMU_PS5_ENV_FILE XEMU_PS5_DATA_DIR "/env.txt"
/* The arcade front's tiles (ui/xui/dashboard.cc, ListArcadeTiles). */
#define XEMU_PS5_CHIHIRO_ARCADE_DIR XEMU_PS5_DATA_DIR "/arcade"
/* One-shot: the dashboard writes the Chihiro image to boot here, then
 * restarts the app; startup takes it and deletes the file. */
#define XEMU_PS5_CHIHIRO_BOOT_FILE XEMU_PS5_DATA_DIR "/chihiro-boot.txt"

#endif
