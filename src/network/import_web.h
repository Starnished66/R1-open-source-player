#ifndef IMPORT_WEB_H
#define IMPORT_WEB_H

/* "Import via Wi-Fi": reuses the stock firmware's thttpd + CGI file-manager
 * web UI (/usr/share/web -- create/delete/download/list/move/upload.cgi)
 * rather than reimplementing a file-upload webapp. This module mirrors
 * what the stock player's /usr/bin/cgic_enable / cgic_disable scripts do:
 * sync the web assets into the writable data partition, then start/stop
 * thttpd and the LAN-discovery udp_server alongside it. */

/* Mirrors /usr/bin/cgic_enable: resyncs /usr/share/web into
 * /usr/data/share/web (only on first call, since these assets never change
 * at runtime), deploys the English index.html/index.js pair (no CN/EN
 * switch in this project), then starts `udp_server -p 4399` and
 * `thttpd -C /etc/thttpd.conf` as background daemons. Cheap enough after
 * the first call to run directly on the UI thread. */
void import_web_start(void);

/* Mirrors /usr/bin/cgic_disable: kills udp_server, thttpd, and any
 * still-running CGI request handlers. */
void import_web_stop(void);

#endif /* IMPORT_WEB_H */
