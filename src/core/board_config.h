#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

/* Board-specific constants selected by the Makefile's BOARD variable
 * (BOARD_DEFINE -> -DBOARD_R1 / -DBOARD_R3PROII). Screen dimensions here are
 * real, device-confirmed values, not placeholders:
 *   - R1: 480x800, this codebase's own long-standing confirmed value (see
 *     HARDWARE_DRIVERS.md and main.c's own history).
 *   - R3PROII: 480x720, confirmed via the extracted stock firmware dump
 *     (usr/resource/config.json's own "type":"screen","hor":480,"ver":720
 *     entry, independently corroborated by etc/logo.jpeg/logo1.jpeg/
 *     logo2.jpeg all measuring exactly 480x720 -- the same evidentiary
 *     standard src/bootloader/fb_draw.h's own comment already relies on:
 *     the boot splash is authored to exactly match the real panel). */
#if defined(BOARD_R3PROII)
  #define BOARD_SCREEN_WIDTH  480
  #define BOARD_SCREEN_HEIGHT 720
#else
  #define BOARD_SCREEN_WIDTH  480
  #define BOARD_SCREEN_HEIGHT 800
#endif

/* Player (Now Playing) screen composition -- a full-bleed cover-art image,
 * top-aligned, plus a bottom-aligned gradient/controls overlay panel that
 * exactly fills the remaining screen height below it, with no gap or
 * overlap. Both pieces are real, fixed-size PNG assets shipped in each
 * device's OWN stock firmware (usr/resource/litegui/theme2/playing_plane/
 * default_cover_565.png and buttom.png -- see assets.c's THEME_ROOT), not
 * something this app generates, so these constants must track each
 * device's own real asset dimensions exactly, not an arbitrary split:
 *   - R1: default_cover_565.png is 480x480, buttom.png is 480x320
 *     (480+320=800, this codebase's own long-standing values).
 *   - R3PROII: confirmed directly from the extracted stock firmware's own
 *     theme2 assets -- default_cover_565.png measures 480x460, buttom.png
 *     measures 480x260 (460+260=720). HiBy's own stock UI already solved
 *     this exact layout problem for this panel; these numbers are read
 *     from their asset files, not derived. */
#if defined(BOARD_R3PROII)
  #define BOARD_PLAYER_COVER_HEIGHT 460
  #define BOARD_PLAYER_OVERLAY_HEIGHT 260
#else
  #define BOARD_PLAYER_COVER_HEIGHT 480
  #define BOARD_PLAYER_OVERLAY_HEIGHT 320
#endif

#endif /* BOARD_CONFIG_H */
