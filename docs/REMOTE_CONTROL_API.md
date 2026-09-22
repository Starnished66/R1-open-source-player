# Remote Control API v1 — Android integration

The player keeps decoding and playing audio locally. A client sends control requests and reads metadata; no audio stream is sent to the phone. Remote Control must be enabled on the player for either transport to accept requests. The setting is session-only and is off again after a player restart.

## Connections and discovery

| Transport | Discovery and connection | Framing |
| --- | --- | --- |
| Wi-Fi | Player's **Wireless → Remote Control** screen shows `http://<player-ip>:8899` and a QR code. The phone and player must share a reachable network. | Ordinary HTTP/1.1. |
| Bluetooth Classic | Pair with the player, then discover the SDP service named **Compas Remote Control** with UUID `9d2f8a7e-6b4c-4d51-8e43-38f7a2c0b719`. On Android use `BluetoothDevice.createRfcommSocketToServiceRecord(UUID)` on a worker thread. | One HTTP/1.1 request and response per RFCOMM socket. Close the socket after reading the response. |

The Bluetooth connection carries **the same method, path, query string, status code, headers, and body** as Wi-Fi HTTP. It is a byte stream, so callers must not assume one read returns a whole response. Parse headers until `\r\n\r\n`, then read exactly `Content-Length` bytes. Request bodies are not used in v1; send parameters in the URL query string even for `POST`. For example:

```http
GET /api/v1/status HTTP/1.1
Host: compas
Connection: close

```

The RFCOMM UUID is the service identifier for discovery. Do not use the generic Serial Port Profile UUID. The service is advertised only while Remote Control is enabled and Bluetooth is available. Bluetooth pairing is required by the service. A paired phone may still need to reconnect after the player toggles Bluetooth or restarts.

## Common rules

- The Wi-Fi base URL is `http://<player-ip>:8899`. Prefix all paths below with `/api/v1`.
- Every HTTP response has `Content-Length` and `Connection: close`. JSON responses have `Content-Type: application/json`; cover art uses `image/jpeg` or `image/png`. Control requests currently return `text/plain` (`OK` on success).
- JSON string fields can be empty. Song `index` values are persistent database IDs, not array positions. Treat them as signed 64-bit integers. Queue `offset` is a zero-based position in the current upcoming queue and may change between reads.
- Query values must be UTF-8 URL encoded. The server accepts `+` as a space and percent decoding for names and filters.
- Poll `/status` for playback changes. A one-second interval matches the web client. Requests are accepted asynchronously: `200 OK` means queued or applied, not that playback has finished changing; read `/status` again to observe the result.
- The server is a small, single-request-per-connection service. Bound concurrent polling and pagination; do not send request bodies or expect persistent connections, WebSockets, or push notifications.

## Discovery and playback state

| Method and path | Response |
| --- | --- |
| `GET /capabilities` | `{"version":1,"transports":["wifi","bluetooth_classic_rfcomm"],"features":["status","playback_controls","queue","library_browse","playlists","album_art"]}`. A transport is listed as supported, though its radio may currently be off. |
| `GET /status` | `playing` and `paused` booleans; `title`, `artist`, `album` strings; `position_seconds`, `duration_seconds` integers; `volume` number from 0 to 1; `play_mode` integer. Modes: `0` sequential, `1` repeat all, `2` repeat one, `3` shuffle. |
| `GET /art` | Artwork for the currently playing file, or `404` if unavailable. |
| `GET /art?index=<song-id>` | Artwork for a library song, or `404` if unavailable. Use the returned `Content-Type`; there is no fixed image size. |

The current status producer does not populate album metadata; `album` may be empty even when a file has album tags. The current track can be identified for display by its title/artist, while `/art` without an index resolves to its actual file.

## Playback commands

All commands use `POST`. Successful commands return `200 OK` with text body `OK`.

| Path | Effect and parameters |
| --- | --- |
| `/playback/toggle` | Toggle play/pause. |
| `/playback/next` | Skip to the next song. |
| `/playback/prev` | Skip to the previous song. |
| `/playback/mode` | Cycle sequential → repeat all → repeat one → shuffle. |
| `/playback/seek?seconds=<n>` | Seek to a nonnegative whole-second position. |
| `/playback/volume?percent=<n>` | Set volume to integer 0–100. |
| `/playback/play?index=<song-id>` | Play the song. Optional context: `playlist=<key>`, or `artist=<name>&album=<name>`, or `album_artist=<name>&album=<name>`. Context controls the following playback queue. |
| `/playback/queue?index=<song-id>` | Add a library song to the upcoming queue. |

## Browse the library

| Method and path | Response |
| --- | --- |
| `GET /library?offset=0&limit=50&q=<text>` | `{"total":N,"songs":[{"index":ID,"title":"...","artist":"..."}]}`. `q` is optional. `limit` is capped at 100. Use `total` and the returned song count for pagination. |
| `GET /library?artist=<name>&album=<name>` | Same shape, filtered by artist and album. `album_artist=<name>` may replace `artist`. Filters may be combined with `offset` and `limit`. |
| `GET /library/artists` | `{"artists":[{"name":"...","count":N,"index":ID,"album_artist":"..."}]}`. |
| `GET /library/album_artists` | Same shape as `/library/artists`; the grouping is by album artist. |
| `GET /library/albums?artist=<name>` | `{"albums":[{"name":"...","count":N,"index":ID,"album_artist":"..."}]}`. `album_artist=<name>` is also accepted. |

The artist and album group lists are capped at 2,000 entries in v1. Use a library song ID to request its artwork or play it.

## Queue and playlists

| Method and path | Response or effect |
| --- | --- |
| `GET /queue?offset=<n>&limit=<n>` | A page of upcoming songs in the player's displayed play order: `{"total":N,"offset":0,"revision":R,"songs":[{"offset":0,"index":ID,"title":"...","artist":"..."}]}`. Offset defaults to 0, limit defaults to 25 and is capped at 25. Song offsets are global across pages. `index` is the library song ID; `-1` means metadata could not be resolved and that row cannot be selected by song ID. The current track is excluded. |
| `POST /queue/remove?offset=<n>&revision=<R>` | Remove one upcoming song at the global zero-based offset. The revision must match the latest queue response or the request is ignored. |
| `POST /queue/clear?revision=<R>` | Clear the playback queue around the current track. The revision must match the latest queue response or the request is ignored. |
| `GET /playlists` | `{"playlists":[{"name":"Favorites","key":"@favorites","internal":true,"writable":false},...]}`. User playlists have `name`, `internal:false`, `writable:true`; use the filename stem as their key. |
| `GET /playlists/songs?name=<key>` | `{"songs":[{"index":ID,"title":"...","artist":"..."}]}`. Built-in keys include `@favorites` and `@most_played`. |
| `POST /playlists?name=<new-name>&index=<song-id>` | Create a writable playlist containing the song. |
| `POST /playlists/add?name=<name>&index=<song-id>` | Add a song to an existing writable playlist. |

Playlist names are a single path component: empty names, slash, backslash, and `..` are rejected. Playlist mutation may take longer than playback controls because it writes to the SD card.

## Errors and compatibility

`400` means a malformed or unsafe request, `404` means a missing route or resource, `405` means an unsupported method, `409` means a queue mutation used a stale revision, and `500` means an internal failure. Error bodies are currently plain text, including on `/api/v1`; do not require a JSON error object. Artwork absence is a normal `404`. Validate user input locally and show a retry path for transport errors.

The original unversioned `/api/...` paths remain available for the existing web remote. New clients should use `/api/v1/...`. The web page itself is served at `/` over Wi-Fi; the Android app should consume the API directly. The API requires no Wi-Fi password or application token once the phone can reach the player; only enable Remote Control on networks you trust. Bluetooth uses the bonded link for access. This is a control interface, not an audio output profile.

## Validation status

The Wi-Fi and Bluetooth code paths compile and link into the R1 target build. Bluetooth pairing, SDP discovery, RFCOMM requests, coexistence with A2DP playback, and R3 hardware behavior still require validation on physical devices before treating this contract as field-proven.
