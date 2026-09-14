# What's New — Next Release

Unreleased changes following the September 14, 2026 release notes.

## Charging and USB

- **No unnecessary library scans after charging.** Unplugging from a wall
  charger or a charging-only cable no longer triggers a database update.
  Automatic scans now require a confirmed USB Storage connection to a computer
  and still respect the auto-rescan setting.
- A confirmed Storage session is remembered even if the USB controller
  suspends or resets before unplugging. Unreadable power-status samples are
  no longer treated as disconnections.

This detects a computer Storage session, not individual file transfers: a
computer connection can still trigger a scan even if no files were changed.
