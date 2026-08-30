#include "gui_track_info.h"

#include "gui.h"
#include "gui_subsonic.h"
#include "gui_theme.h"
#include "screen_builders.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

extern void nav_push(lv_obj_t * screen);

static lv_obj_t * info_screen;
static lv_obj_t * info_list;
static lv_obj_t * info_title;
static gui_track_info_context_t current_context;
static uint64_t context_generation;
static uint64_t rendered_context_generation;
static audio_current_format_info_t rendered_runtime;
static bool rendered_runtime_valid;

static uint64_t local_facts_generation;
static bool local_stat_ok;
static uint64_t local_file_size;

static const char * codec_name(audio_codec_t codec) {
    switch (codec) {
        case AUDIO_CODEC_FLAC: return "FLAC";
        case AUDIO_CODEC_MP3: return "MP3";
        case AUDIO_CODEC_PCM: return "PCM";
        case AUDIO_CODEC_DSD: return "DSD";
        case AUDIO_CODEC_AAC: return "AAC";
        case AUDIO_CODEC_ALAC: return "ALAC";
        case AUDIO_CODEC_APE: return "Monkey's Audio";
        case AUDIO_CODEC_WMA: return "WMA";
        case AUDIO_CODEC_OPUS: return "Opus";
        case AUDIO_CODEC_VORBIS: return "Vorbis";
        case AUDIO_CODEC_UNKNOWN: break;
    }
    return NULL;
}

static audio_codec_t codec_from_container(const char * container) {
    if (!container || !container[0]) return AUDIO_CODEC_UNKNOWN;
    if (strcasecmp(container, "FLAC") == 0) return AUDIO_CODEC_FLAC;
    if (strcasecmp(container, "MP3") == 0) return AUDIO_CODEC_MP3;
    if (strcasecmp(container, "WAV") == 0 || strcasecmp(container, "AIFF") == 0)
        return AUDIO_CODEC_PCM;
    if (strcasecmp(container, "DSF") == 0 || strcasecmp(container, "DFF") == 0)
        return AUDIO_CODEC_DSD;
    if (strcasecmp(container, "AAC") == 0 || strcasecmp(container, "ADTS") == 0)
        return AUDIO_CODEC_AAC;
    /* M4A may carry AAC or ALAC and Ogg may carry Vorbis or Opus. Wait for
     * the actual decoder snapshot instead of presenting a plausible but
     * potentially wrong codec while the track is still opening. */
    if (strcasecmp(container, "APE") == 0) return AUDIO_CODEC_APE;
    if (strcasecmp(container, "ASF") == 0 || strcasecmp(container, "WMA") == 0)
        return AUDIO_CODEC_WMA;
    return AUDIO_CODEC_UNKNOWN;
}

static void format_rate(unsigned int hz, char * out, size_t out_size) {
    if (hz == 0) {
        out[0] = '\0';
    } else if (hz % 1000 == 0) {
        snprintf(out, out_size, "%u kHz", hz / 1000);
    } else {
        snprintf(out, out_size, "%.1f kHz", (double) hz / 1000.0);
    }
}

static void format_file_size(uint64_t bytes, char * out, size_t out_size) {
    if (bytes >= 1024ULL * 1024ULL * 1024ULL)
        snprintf(out, out_size, "%.2f GB", (double) bytes / (1024.0 * 1024.0 * 1024.0));
    else if (bytes >= 1024ULL * 1024ULL)
        snprintf(out, out_size, "%.1f MB", (double) bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024ULL)
        snprintf(out, out_size, "%.1f KB", (double) bytes / 1024.0);
    else
        snprintf(out, out_size, "%llu bytes", (unsigned long long) bytes);
}

/* Extract only a radio host. Userinfo, port, path, query and fragment are
 * intentionally discarded so even an unusual credential-bearing station
 * URL cannot leak secrets into the UI. */
static void sanitized_url_host(const char * url, char * out, size_t out_size) {
    out[0] = '\0';
    if (!url || out_size == 0) return;
    const char * start = strstr(url, "://");
    start = start ? start + 3 : url;
    const char * end = start + strcspn(start, "/?#");
    const char * host = start;
    for (const char * p = start; p < end; p++) {
        if (*p == '@') host = p + 1;
    }
    const char * host_end = end;
    if (host < end && *host == '[') {
        const char * close = memchr(host, ']', (size_t) (end - host));
        if (close) {
            host++;
            host_end = close;
        }
    } else {
        const char * colon = memchr(host, ':', (size_t) (end - host));
        if (colon) host_end = colon;
    }
    size_t len = (size_t) (host_end - host);
    if (len == 0) return;
    if (len >= out_size) len = out_size - 1;
    memcpy(out, host, len);
    out[len] = '\0';
}

static void wrap_text_to_width(const char * src, char * out, size_t out_size, const lv_font_t * font, int32_t max_width) {
    if (!src || !out || out_size == 0) return;
    if (!font || max_width <= 0) {
        snprintf(out, out_size, "%s", src);
        return;
    }

    size_t out_pos = 0;
    int32_t cur_line_w = 0;

    /* uint32_t, not size_t -- _lv_text_encoded_next()'s own signature is
     * uint32_t (*)(const char *, uint32_t *) (lv_text_private.h). On a
     * 32-bit target size_t and uint32_t are the same width, so this built
     * silently there, but on a 64-bit host passing a size_t* (8 bytes)
     * where the callee only ever writes/reads 4 lets the top 4 bytes sit
     * uninitialized garbage that the next `i`/`c_idx` read as part of a
     * wider size_t -- a real correctness bug on host, not just a build
     * nuisance. token_start/token_bytes/c_idx/char_start are all derived
     * from the same byte-index space and follow suit; char_len already
     * was uint32_t. */
    uint32_t i = 0;
    while (src[i] != '\0' && out_pos + 1 < out_size) {
        if (src[i] == '\n') {
            out[out_pos++] = '\n';
            cur_line_w = 0;
            i++;
            continue;
        }

        uint32_t token_start = i;
        int32_t token_w = 0;
        while (src[i] != '\0' && src[i] != '\n') {
            uint32_t letter = _lv_text_encoded_next(src, &i);
            uint32_t next_letter = src[i] ? _lv_text_encoded_next(src, NULL) : 0;
            token_w += lv_font_get_glyph_width(font, letter, next_letter);

            if (letter == '/' || letter == ' ' || letter == '-' || letter == '_' ||
                letter == '.' || letter == ':' || letter == '\\') {
                break;
            }
        }
        uint32_t token_bytes = i - token_start;

        if (cur_line_w + token_w <= max_width || cur_line_w == 0) {
            if (out_pos + token_bytes < out_size) {
                memcpy(&out[out_pos], &src[token_start], token_bytes);
                out_pos += token_bytes;
                cur_line_w += token_w;
            } else {
                break;
            }
        } else {
            if (out_pos + 1 < out_size) {
                out[out_pos++] = '\n';
                cur_line_w = 0;
            }

            if (token_w > max_width) {
                uint32_t c_idx = token_start;
                while (c_idx < token_start + token_bytes && out_pos + 1 < out_size) {
                    uint32_t char_start = c_idx;
                    uint32_t letter = _lv_text_encoded_next(src, &c_idx);
                    uint32_t next_letter = (c_idx < token_start + token_bytes) ? _lv_text_encoded_next(src, NULL) : 0;
                    int32_t glyph_w = lv_font_get_glyph_width(font, letter, next_letter);
                    uint32_t char_len = c_idx - char_start;

                    if (cur_line_w + glyph_w > max_width && cur_line_w > 0) {
                        out[out_pos++] = '\n';
                        cur_line_w = 0;
                    }
                    if (out_pos + char_len < out_size) {
                        memcpy(&out[out_pos], &src[char_start], char_len);
                        out_pos += char_len;
                        cur_line_w += glyph_w;
                    }
                }
            } else {
                if (out_pos + token_bytes < out_size) {
                    memcpy(&out[out_pos], &src[token_start], token_bytes);
                    out_pos += token_bytes;
                    cur_line_w = token_w;
                }
            }
        }
    }
    out[out_pos < out_size ? out_pos : out_size - 1] = '\0';
}

static void add_info_line(const char * name, const char * value) {
    if (!info_list || !name || !value || !value[0]) return;
    char raw_line[2304];
    snprintf(raw_line, sizeof(raw_line), "%s: %s", name, value);

    const lv_font_t * font = gui_theme_font(GUI_FONT_ROLE_BODY);
    int32_t scr_w = lv_display_get_horizontal_resolution(lv_display_get_default());
    int32_t max_w = scr_w - 48;
    if (max_w <= 0) max_w = 432;

    char wrapped_line[4096];
    wrap_text_to_width(raw_line, wrapped_line, sizeof(wrapped_line), font, max_w);

    lv_obj_t * label = lv_label_create(info_list);
    lv_obj_set_width(label, max_w);
    lv_obj_set_height(label, LV_SIZE_CONTENT);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_add_style(label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_pad_top(label, 8, 0);
    lv_obj_add_flag(label, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_label_set_text(label, wrapped_line);
}

static void load_local_facts_once(void) {
    if (local_facts_generation == context_generation) return;
    local_facts_generation = context_generation;
    local_stat_ok = false;
    local_file_size = 0;
    if (current_context.source != GUI_TRACK_SOURCE_LOCAL || !current_context.path[0]) return;
    struct stat st;
    if (stat(current_context.path, &st) == 0 && st.st_size >= 0) {
        local_stat_ok = true;
        local_file_size = (uint64_t) st.st_size;
    }
}

static bool current_runtime(audio_current_format_info_t * out) {
    if (!audio_get_current_format_info(out)) return false;
    /* The decoder snapshot is still authoritative when the UI-side context
     * has not been populated yet.  The old strict strcmp rejected it against
     * an empty path and left the Information screen completely blank even
     * though a track was actively decoded.  Once a context exists, retain
     * the path check so a late snapshot from the previous track can never be
     * presented as information for the new one. */
    if (!current_context.path[0]) return true;
    return strcmp(out->path, current_context.path) == 0;
}

static void rebuild_info_text(const audio_current_format_info_t * runtime, bool runtime_valid) {
    if (!info_list) return;
    lv_obj_clean(info_list);
    load_local_facts_once();

    char value[256];
    char rate[48];
    audio_codec_t codec = runtime_valid && runtime->codec != AUDIO_CODEC_UNKNOWN
        ? runtime->codec : current_context.declared_codec;
    if (codec == AUDIO_CODEC_UNKNOWN) codec = codec_from_container(current_context.container);
    add_info_line("Codec", codec_name(codec));
    add_info_line("Container", current_context.container);

    unsigned int source_rate = runtime_valid && runtime->source_sample_rate
        ? runtime->source_sample_rate : current_context.declared_sample_rate;
    unsigned int source_depth = runtime_valid && runtime->source_bit_depth
        ? runtime->source_bit_depth : current_context.declared_bit_depth;
    if (!source_rate && !current_context.path[0]) source_rate = audio_get_sample_rate();
    format_rate(source_rate, rate, sizeof(rate));
    value[0] = '\0';
    if (codec == AUDIO_CODEC_DSD && source_rate) {
        snprintf(value, sizeof(value), "1-bit DSD / %.4g MHz", (double) source_rate / 1000000.0);
    } else if (source_depth && rate[0]) {
        snprintf(value, sizeof(value), "%u-bit / %s", source_depth, rate);
    } else if (rate[0]) {
        snprintf(value, sizeof(value), "%s", rate);
    } else if (source_depth) {
        snprintf(value, sizeof(value), "%u-bit", source_depth);
    }
    add_info_line("Source", value);

    if (runtime_valid && runtime->output_sample_rate) {
        format_rate(runtime->output_sample_rate, rate, sizeof(rate));
        snprintf(value, sizeof(value), "%u-bit PCM / %s",
                 runtime->output_bit_depth ? runtime->output_bit_depth : 16, rate);
        add_info_line("Output", value);
    }

    unsigned int channels = runtime_valid && runtime->channels
        ? runtime->channels : current_context.declared_channels;
    if (channels == 1) snprintf(value, sizeof(value), "Mono (1 channel)");
    else if (channels == 2) snprintf(value, sizeof(value), "Stereo (2 channels)");
    else if (channels > 0) snprintf(value, sizeof(value), "%u channels", channels);
    else value[0] = '\0';
    add_info_line("Channels", value);

    unsigned int bitrate = runtime_valid && runtime->bitrate_kbps
        ? runtime->bitrate_kbps : current_context.declared_bitrate_kbps;
    bool bitrate_estimated = false;
    double duration = runtime_valid && runtime->duration_seconds > 0.0
        ? runtime->duration_seconds : current_context.declared_duration_seconds;
    if (duration <= 0.0 && !current_context.path[0]) duration = audio_get_duration_seconds();
    if (!bitrate && local_stat_ok && duration > 0.0) {
        double estimate = ((double) local_file_size * 8.0) / duration / 1000.0;
        if (estimate > 0.0 && estimate < 1000000.0) {
            bitrate = (unsigned int) (estimate + 0.5);
            bitrate_estimated = true;
        }
    }
    if (bitrate) snprintf(value, sizeof(value), "%s%u kbps", bitrate_estimated ? "~" : "", bitrate);
    else value[0] = '\0';
    add_info_line("Bitrate", value);

    if (duration > 0.0) {
        gui_format_time(duration, value, sizeof(value));
    } else {
        value[0] = '\0';
    }
    add_info_line("Duration", value);
    if (local_stat_ok) {
        format_file_size(local_file_size, value, sizeof(value));
        add_info_line("File size", value);
    }

    value[0] = '\0';
    if (current_context.has_disc_number && current_context.has_track_number)
        snprintf(value, sizeof(value), "Disc %d / Track %d", current_context.disc_number,
                 current_context.track_number);
    else if (current_context.has_disc_number)
        snprintf(value, sizeof(value), "Disc %d", current_context.disc_number);
    else if (current_context.has_track_number)
        snprintf(value, sizeof(value), "Track %d", current_context.track_number);
    add_info_line("Position", value);

    value[0] = '\0';
    if (runtime_valid && runtime->replaygain_applied) {
        snprintf(value, sizeof(value), "Applied %+.1f dB", runtime->replaygain_applied_db);
    } else if (current_context.has_replaygain_track || current_context.has_replaygain_album) {
        if (current_context.replaygain_mode == 0) {
            snprintf(value, sizeof(value), "Off");
        } else if (current_context.replaygain_mode == 2 && current_context.has_replaygain_album) {
            snprintf(value, sizeof(value), "Album %+.1f dB", current_context.replaygain_album_db);
        } else if (current_context.has_replaygain_track) {
            snprintf(value, sizeof(value), "Track %+.1f dB", current_context.replaygain_track_db);
        } else {
            snprintf(value, sizeof(value), "Album %+.1f dB", current_context.replaygain_album_db);
        }
    }
    add_info_line("ReplayGain", value);

    value[0] = '\0';
    if (current_context.source == GUI_TRACK_SOURCE_SUBSONIC) {
        add_info_line("Provider", "Subsonic");
    } else if (current_context.source == GUI_TRACK_SOURCE_PLUGIN) {
        if (current_context.provider[0] && current_context.track_id[0])
            snprintf(value, sizeof(value), "%s / %s", current_context.provider, current_context.track_id);
        else
            snprintf(value, sizeof(value), "%s", current_context.provider);
        add_info_line("Provider", value);
    } else if (current_context.source == GUI_TRACK_SOURCE_RADIO) {
        char host[192];
        sanitized_url_host(current_context.path, host, sizeof(host));
        if (host[0]) snprintf(value, sizeof(value), "Radio / %s", host);
        else snprintf(value, sizeof(value), "Radio");
        add_info_line("Provider", value);
    } else if (runtime_valid && runtime->is_stream) {
        /* Do not expose runtime->path here: it may be a signed/authenticated
         * URL.  This fallback only describes the source generically. */
        add_info_line("Provider", "Network stream");
    } else {
        const char * local_path = current_context.path;
        if (!local_path[0] && runtime_valid && !runtime->is_stream &&
            !strstr(runtime->path, "://"))
            local_path = runtime->path;
        add_info_line("Location", local_path);
    }

    if (lv_obj_get_child_count(info_list) == 0) {
        lv_obj_t * label = lv_label_create(info_list);
        lv_obj_set_width(label, lv_pct(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
        lv_obj_set_style_pad_top(label, 8, 0);
        lv_label_set_text(label, "No track information available");
    }
}

/* For gui_reload.c's in-process UI reload -- gui_track_info_init() is
 * guarded (`if (info_screen) return;`) so it silently no-ops on a second
 * call unless info_screen is deleted AND nulled first here. */
void gui_track_info_teardown(void) {
    if (!info_screen) return;
    lv_obj_del(info_screen);
    info_screen = NULL;
    info_list = NULL;
    info_title = NULL;
}

void gui_track_info_init(void) {
    if (info_screen) return;
    info_screen = build_subsonic_list_screen("Information", &info_title, &info_list);
    /* Plain left-aligned Name: value labels, same pattern as Wi-Fi Info.
     * No pill rows, no marquee.  The list stays vertically scrollable for a
     * long local path or a larger font size. */
    lv_obj_set_flex_align(info_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_left(info_list, 24, 0);
    lv_obj_set_style_pad_right(info_list, 24, 0);
    lv_obj_set_style_pad_top(info_list, 12, 0);
    lv_obj_set_style_pad_bottom(info_list, 16, 0);
}

lv_obj_t * gui_track_info_get_screen(void) {
    return info_screen;
}

void gui_track_info_set_current(const gui_track_info_context_t * context) {
    if (context) current_context = *context;
    else memset(&current_context, 0, sizeof(current_context));
    context_generation++;
}

void gui_track_info_open(void) {
    if (!info_screen) gui_track_info_init();
    audio_current_format_info_t runtime;
    bool valid = current_runtime(&runtime);
    rebuild_info_text(valid ? &runtime : NULL, valid);
    rendered_context_generation = context_generation;
    rendered_runtime_valid = valid;
    if (valid) rendered_runtime = runtime;
    else memset(&rendered_runtime, 0, sizeof(rendered_runtime));
    nav_push(info_screen);
}

void gui_track_info_poll(void) {
    if (!info_screen || lv_screen_active() != info_screen) return;
    audio_current_format_info_t runtime;
    bool valid = current_runtime(&runtime);
    if (valid) runtime.generation = 0; /* seek-only republishes do not change displayed facts */
    audio_current_format_info_t previous = rendered_runtime;
    previous.generation = 0;
    bool runtime_changed = valid != rendered_runtime_valid ||
        (valid && memcmp(&runtime, &previous, sizeof(runtime)) != 0);
    if (rendered_context_generation == context_generation && !runtime_changed) return;

    rebuild_info_text(valid ? &runtime : NULL, valid);
    rendered_context_generation = context_generation;
    rendered_runtime_valid = valid;
    if (valid) rendered_runtime = runtime;
    else memset(&rendered_runtime, 0, sizeof(rendered_runtime));
}
