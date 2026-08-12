#ifndef IMPORT_WEB_H
#define IMPORT_WEB_H

/* "Import via Wi-Fi": reuses the stock firmware's own thttpd + CGI file-
 * manager web UI (/usr/share/web -- create/delete/download/list/move/
 * upload.cgi, real compiled cgic binaries) rather than reimplementing a
 * file-upload webapp ourselves. This module is just the orchestration the
 * stock player's own /usr/bin/cgic_enable / cgic_disable scripts do
 * (confirmed by reading them directly): resync the static web assets into
 * the writable data partition, then start/stop thttpd and the LAN-
 * discovery udp_server alongside it. */

/* Mirrors /usr/bin/cgic_enable: resyncs /usr/share/web into
 * /usr/data/share/web (skips the copy if already present -- unlike the
 * stock script's MD5-checksum resync, this project's own web assets never
 * change at runtime, so a one-time first-run copy is enough), deploys the
 * English index.html/index.js pair (this project has no CN/EN
 * localization switch, so always that one, unlike the stock script's
 * /usr/data/region-based choice), then starts `udp_server -p 4399` and
 * `thttpd -C /etc/thttpd.conf` as background daemons. The bulk directory
 * copy only actually runs once ever (first call); every later call is just
 * two small file copies plus (re)spawning the daemons, so this is called
 * directly on the UI thread rather than threaded off it. */
void import_web_start(void);

/* Mirrors /usr/bin/cgic_disable: kills udp_server, thttpd, and any
 * still-running CGI request handlers. */
void import_web_stop(void);

#endif /* IMPORT_WEB_H */
