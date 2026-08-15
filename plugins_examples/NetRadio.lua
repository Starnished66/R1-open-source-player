-- Net Radio plugin -- demonstrates plugin.register_stream_media_tile()
-- (appearing as a real icon-grid tile in Stream Media, next to Subsonic)
-- together with live http(s):// stream URL playback.
--
-- READ BEFORE ADDING YOUR OWN STATIONS -- current real limitations, not
-- just missing polish (see PLUGINS.md's "Live stream URLs" section):
--   * Every http(s):// URL is always decoded as MP3, regardless of what it
--     looks like. A station serving AAC/Opus/Ogg will fail to play.
--   * No duration, no seeking, no auto-advance into or out of a stream --
--     tap a different station (or Prev/Next) to change what's playing.
--   * No reconnect-on-drop -- if the connection is lost mid-stream,
--     playback just stops, same as a local file hitting a read error.
--   * No ICY "now playing" metadata or in-stream ID3 tags are surfaced --
--     only the label given to show_list() below is ever shown.
--
-- The station URLs below are SomaFM's own published direct-MP3-stream
-- endpoints (see https://somafm.com/listen/) -- a well-known, long-running
-- public internet radio service, used here because they're real, stable,
-- and actually MP3, not because this app has any special relationship with
-- them. SomaFM does periodically add/retire mirrors, so double-check
-- against their own listen page if a station stops responding.

local stations = {
    { name = "SomaFM: Groove Salad (ambient)",  url = "http://ice1.somafm.com/groovesalad-128-mp3" },
    { name = "SomaFM: Drone Zone (ambient)",     url = "http://ice1.somafm.com/dronezone-128-mp3" },
    { name = "SomaFM: Secret Agent (lounge)",    url = "http://ice1.somafm.com/secretagent-128-mp3" },
    { name = "SomaFM: Space Station (electronic)", url = "http://ice1.somafm.com/spacestation-128-mp3" },
}

local function open_stations()
    local labels, urls = {}, {}
    for i, s in ipairs(stations) do
        labels[i] = s.name
        urls[i] = s.url
    end

    plugin.show_list("Net Radio", labels, function(index)
        plugin.play_list(urls, index)
    end)
end

plugin.register_stream_media_tile("Net Radio", open_stations, "stream_media/radio.png")
