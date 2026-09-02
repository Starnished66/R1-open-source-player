# What's New

This file is the curated changelog for the next weekly beta. Update it in the
same pull request or commit as a user-visible player change; every Sunday
release embeds its current contents and links back to the exact revision used.

## Beta 2

- Added runtime plugin management and consolidated theme support, including
  custom Home layouts and Home-only background images.
- UI and theme reloads now preserve active Bluetooth, Wi-Fi, remote-control,
  AirPlay, DLNA, and Wi-Fi Import services while releasing stale UI resources.
- Reduced memory pressure during plugin reloads, artwork/database scans, long
  track seeking, and Stock-player handoff.
- Improved boot selection: Stock always exposes the menu, newer player builds
  are preferred, and equal builds prefer the internal player.
- Added manual clock controls, automatic synchronization, 12/24-hour display,
  and timezone selection under Settings > System > Clock.
- Added collection action menus for artists, album artists, and albums.
- Improved long MP3/AAC/Opus seeking and rapid skip/seek recovery.
- Made volume and brightness dragging responsive while coalescing hardware
  updates in the background.
- Added adaptive SD-card readiness handling and clearer boot diagnostics.
