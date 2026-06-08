/*
 * MP3 player feature module (PIE, transient) -- first consumer of the feature-module
 * framework (docs/module-menu-registration.md, docs/mp3-player-plan.md).
 *
 * Registers a Tools entry "MP3 Player" AND claims ".mp3"; picking a .mp3 in the file
 * browser launches it with that path (extension dispatch). It runs as a THEME-NATIVE
 * full-screen app: the core's themed header (title + battery) and footer are drawn each
 * frame and all content uses the gui_api theme colors, so the player matches the active
 * theme by default. Layout (content area y 23..217):
 *   - now playing: song name (marquee), elapsed / total, a progress bar
 *   - a selectable button bar: Prev | Seek | Play/Pause | Next | Playlist
 *     LEFT/RIGHT move the selection, A activates (LEFT/RIGHT do NOT skip songs).
 * The play queue is the sibling *.mp3 in the launched file's directory (host->list_dir).
 *
 * Audio: minimp3 (vendored) streams from SD via the host file API, downmixed to mono and
 * linear-resampled to the fixed 48 kHz SAI rate, double-buffered into a register-level
 * SAI1+DMA sink (audio_sai.c). Constant memory (bounded pool buffers).
 *
 * Increment 1 (this build): theme-native UI + button bar; Prev/Play-Pause/Next work.
 * Increment 2 (next): the Seek scrubber (needs a driver seek primitive) and the Winamp
 * playlist pop-up (Add via the minimal file browser / pick_file, Play, Remove).
 */
#include "system/module.h"
#include "system/feature.h"
#include "system/input.h"   /* INPUT_B / A / LEFT / RIGHT */
#include <string.h>
#include "audio_sai.h"
#include "mp3_strings_gen.h"   /* compiled-in translations (cooked from i18n/modules/mp3/*.json) */

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD       /* Cortex-M7 has no NEON/SSE; use the portable C path */
#include "minimp3.h"

MODULE_HEADER;

/* ---- output (DMA) double buffer: mono, 48 kHz ---- */
#define OUT_HALF    4800                  /* 100 ms per half */
#define OUT_SAMPLES (OUT_HALF * 2)
static int16_t g_out[OUT_SAMPLES];

/* ---- streaming MP3 decode state (all static -> module pool, not the stack) ---- */
#define IN_CAP        65536       /* raw-MP3 window; ID3 tag is skipped by seek, so 64K buffer prevents SD card stutters */
#define IN_REFILL_LO  16384
static const feature_host_t *g_h;
static void   *g_fp;
static uint8_t g_in[IN_CAP];
static int      g_in_len, g_in_pos, g_eof;
static mp3dec_t g_dec;
static int16_t  g_frame[MINIMP3_MAX_SAMPLES_PER_FRAME];
static int      g_frame_n, g_frame_pos;
static uint32_t g_played;                 /* output samples produced this track */
static int      g_bitrate;                /* kbps of the current stream (for duration est.) */
static uint32_t g_filepos;                /* bytes read from the file so far (for seek-to-current) */
static uint32_t g_total_sec;              /* track duration (Xing-exact or CBR-corrected); 0 = est */
static uint32_t g_id3;                     /* ID3v2 tag size of the loaded track (audio starts here) */
static int32_t  g_prev, g_cur;
static uint32_t g_frac, g_step;
static int      g_primed;

static void refill(void) {
    int rem = g_in_len - g_in_pos;
    if (rem > 0 && g_in_pos > 0) memmove(g_in, g_in + g_in_pos, (size_t)rem);
    g_in_len = rem; g_in_pos = 0;
    if (g_h->is_io_busy && g_h->is_io_busy()) return;
    while (g_in_len < (int)sizeof(g_in) && !g_eof) {
        int r = g_h->file_read(g_fp, g_in + g_in_len, (uint32_t)(sizeof(g_in) - g_in_len));
        if (r <= 0) { g_eof = 1; break; }
        g_in_len += r;
        g_filepos += (uint32_t)r;
    }
}
static int decode_frame(void) {
    for (;;) {
        if ((g_in_len - g_in_pos) < IN_REFILL_LO && !g_eof) refill();
        if ((g_in_len - g_in_pos) <= 0) return 0;
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&g_dec, g_in + g_in_pos,
                                          g_in_len - g_in_pos, g_frame, &info);
        g_in_pos += info.frame_bytes;
        if (info.frame_bytes == 0) { if (g_eof) return 0; refill(); continue; }
        if (samples == 0) continue;
        if (info.channels == 2)
            for (int i = 0; i < samples; i++)
                g_frame[i] = (int16_t)(((int)g_frame[2 * i] + g_frame[2 * i + 1]) >> 1);
        g_step = (uint32_t)(((uint64_t)info.hz << 16) / AUDIO_SAMPLE_RATE);
        if (info.bitrate_kbps && !g_bitrate) g_bitrate = info.bitrate_kbps;   /* freeze (VBR-stable) */
        g_frame_n = samples; g_frame_pos = 0;
        return 1;
    }
}
static int src_next(int32_t *out) {
    while (g_frame_pos >= g_frame_n) if (!decode_frame()) return 0;
    *out = g_frame[g_frame_pos++];
    return 1;
}
static int produce(int16_t *dst, int count) {
    if (!g_primed) { int32_t v; if (!src_next(&v)) return 0; g_prev = g_cur = v; g_frac = 0; g_primed = 1; }
    for (int i = 0; i < count; i++) {
        while (g_frac >= 0x10000u) {
            int32_t v; if (!src_next(&v)) return i;
            g_prev = g_cur; g_cur = v; g_frac -= 0x10000u;
        }
        dst[i] = (int16_t)(g_prev + (((g_cur - g_prev) * (int32_t)g_frac) >> 16));
        g_frac += g_step;
    }
    return count;
}
static void decode_reset(void) {
    g_in_len = g_in_pos = g_eof = 0;
    g_frame_n = g_frame_pos = 0;
    g_primed = 0; g_played = 0; g_bitrate = 0; g_filepos = 0;
    mp3dec_init(&g_dec);
}

static uint32_t syncsafe(const uint8_t *p) {
    return ((uint32_t)(p[0] & 0x7f) << 21) | ((uint32_t)(p[1] & 0x7f) << 14)
         | ((uint32_t)(p[2] & 0x7f) << 7)  | (uint32_t)(p[3] & 0x7f);
}
/* Compute the track duration ONCE. The caller has already skipped any ID3v2 tag (via seek),
 * so g_in[0] is at/near the first MPEG audio frame: find it, read its Xing/Info VBR frame
 * count if present (exact for VBR + CBR alike), else estimate (size - id3) * 8 / bitrate.
 * Sets g_total_sec (0 = couldn't parse -> draw falls back to a bitrate estimate). This is
 * why the time was wrong + jumpy before: a VBR file's per-frame bitrate is not its average,
 * and album-art ID3 tags inflate the file size. */
static void compute_duration(uint32_t file_size, uint32_t id3) {
    static const int BR1[16] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
    static const int BR2[16] = {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0};
    static const int SR1[4] = {44100,48000,32000,0}, SR2[4] = {22050,24000,16000,0}, SR25[4] = {11025,12000,8000,0};
    g_total_sec = 0;
    for (int p = 0; p + 4 <= g_in_len; p++) {
        if (g_in[p] != 0xFF || (g_in[p+1] & 0xE0) != 0xE0) continue;
        int ver = (g_in[p+1] >> 3) & 3;     /* 0=2.5  2=2  3=1 (1=reserved) */
        int layer = (g_in[p+1] >> 1) & 3;   /* 1=Layer III */
        int bri = (g_in[p+2] >> 4) & 0xF, sri = (g_in[p+2] >> 2) & 3, chan = (g_in[p+3] >> 6) & 3;
        if (ver == 1 || layer != 1 || bri == 0 || bri == 15 || sri == 3) continue;
        int bitrate = (ver == 3) ? BR1[bri] : BR2[bri];
        int srate = (ver == 3) ? SR1[sri] : (ver == 2) ? SR2[sri] : SR25[sri];
        int spf = (ver == 3) ? 1152 : 576;
        if (!bitrate || !srate) continue;
        int si = (ver == 3) ? ((chan == 3) ? 17 : 32) : ((chan == 3) ? 9 : 17);
        int xo = p + 4 + si;
        if (xo + 12 <= g_in_len &&
            (memcmp(&g_in[xo], "Xing", 4) == 0 || memcmp(&g_in[xo], "Info", 4) == 0)) {
            uint32_t flags = ((uint32_t)g_in[xo+4]<<24)|((uint32_t)g_in[xo+5]<<16)|((uint32_t)g_in[xo+6]<<8)|g_in[xo+7];
            if (flags & 1) {
                uint32_t fc = ((uint32_t)g_in[xo+8]<<24)|((uint32_t)g_in[xo+9]<<16)|((uint32_t)g_in[xo+10]<<8)|g_in[xo+11];
                g_total_sec = (uint32_t)((uint64_t)fc * (uint32_t)spf / (uint32_t)srate);
                return;
            }
        }
        g_total_sec = (uint32_t)((uint64_t)(file_size - id3) * 8u / ((uint32_t)bitrate * 1000u));
        return;
    }
}

/* ---- play queue: sibling *.mp3 in the launched file's directory ---- */
#define Q_MAX 64
#define Q_NAME 128                /* full paths (Add can pull from any folder); longer are skipped */
static char     g_dir[256];
static char     g_qname[Q_MAX][Q_NAME];
static uint32_t g_qsize[Q_MAX];
static int      g_qn;
static void join_path(char *out, size_t cap, const char *dir, const char *name);   /* fwd */

static int ext_is_mp3(const char *name) {
    const char *dot = 0;
    for (const char *p = name; *p; p++) if (*p == '.') dot = p;
    if (!dot) return 0;
    /* skip AppleDouble sidecars ("._name") that also end in .mp3 */
    if (name[0] == '.' && name[1] == '_') return 0;
    return (dot[1] | 32) == 'm' && (dot[2] | 32) == 'p' && dot[3] == '3' && dot[4] == 0;
}
/* list_dir callback: store the sibling's FULL path (g_dir + name). */
static void q_add(const char *name, int is_dir, uint32_t size, void *user) {
    (void)user;
    if (is_dir || g_qn >= Q_MAX || !ext_is_mp3(name)) return;
    char full[Q_NAME];
    join_path(full, sizeof(full), g_dir[0] ? g_dir : "/", name);
    size_t n = strlen(full);
    if (n >= Q_NAME) return;
    memcpy(g_qname[g_qn], full, n + 1);
    g_qsize[g_qn] = size;
    g_qn++;
}
/* Append an already-absolute path to the queue (used by Add). */
static void q_add_full(const char *path, uint32_t size) {
    size_t n = strlen(path);
    if (n == 0 || n >= Q_NAME || g_qn >= Q_MAX) return;
    memcpy(g_qname[g_qn], path, n + 1);
    g_qsize[g_qn] = size;
    g_qn++;
}
static int ci_less(const char *a, const char *b) {
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return x < y;
        if (!x) return 0;
    }
}
static void q_sort(void) {
    for (int i = 1; i < g_qn; i++) {
        char tn[Q_NAME]; memcpy(tn, g_qname[i], Q_NAME); uint32_t ts = g_qsize[i];
        int j = i - 1;
        while (j >= 0 && ci_less(tn, g_qname[j])) {
            memcpy(g_qname[j + 1], g_qname[j], Q_NAME); g_qsize[j + 1] = g_qsize[j]; j--;
        }
        memcpy(g_qname[j + 1], tn, Q_NAME); g_qsize[j + 1] = ts;
    }
}
static void split_path(const char *path, char *dir, size_t dcap, const char **base) {
    const char *slash = path;
    for (const char *p = path; *p; p++) if (*p == '/') slash = p;
    size_t dl = (size_t)(slash - path);
    if (dl >= dcap) dl = dcap - 1;
    memcpy(dir, path, dl); dir[dl] = 0;
    *base = (*slash == '/') ? slash + 1 : slash;
}
static void join_path(char *out, size_t cap, const char *dir, const char *name) {
    size_t i = 0;
    for (const char *p = dir; *p && i < cap - 1; p++) out[i++] = *p;
    if (i == 0 || out[i - 1] != '/') { if (i < cap - 1) out[i++] = '/'; }
    for (const char *p = name; *p && i < cap - 1; p++) out[i++] = *p;
    out[i] = 0;
}

/* ---- UI ---- */
enum { BTN_PREV, BTN_SEEK, BTN_PLAY, BTN_NEXT, BTN_LIST, NBTN };

static void fmt_mmss(char *b, uint32_t sec) {
    if (sec > 99 * 60 + 59) sec = 99 * 60 + 59;
    uint32_t m = sec / 60, s = sec % 60;
    b[0] = (char)('0' + (m / 10) % 10); b[1] = (char)('0' + m % 10); b[2] = ':';
    b[3] = (char)('0' + s / 10);        b[4] = (char)('0' + s % 10); b[5] = 0;
}
/* "/music/Peaches - X.mp3" -> "Peaches - X" (basename, trailing .mp3 dropped). */
static void disp_name(char *dst, size_t cap, const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++) if (*p == '/') base = p + 1;
    size_t n = strlen(base);
    if (n >= 4 && base[n-4]=='.' && (base[n-3]|32)=='m' && (base[n-2]|32)=='p' && base[n-1]=='3') n -= 4;
    if (n >= cap) n = cap - 1;
    memcpy(dst, base, n); dst[n] = 0;
}
static const char *btn_label(int i, int paused) {
    switch (i) {
        case BTN_PREV: return ms(MS_PREV);
        case BTN_SEEK: return ms(MS_SEEK);
        case BTN_PLAY: return paused ? ms(MS_PLAY) : ms(MS_PAUSE);
        case BTN_NEXT: return ms(MS_NEXT);
        default:       return ms(MS_LIST);
    }
}
static void draw_buttons(const gui_api_t *g, int sel, int paused) {
    const int n = NBTN, bw = 56, gap = 4, bh = 28, y = 180;
    int x0 = (320 - (n * bw + (n - 1) * gap)) / 2;
    for (int i = 0; i < n; i++) {
        int x = x0 + i * (bw + gap);
        if (i == sel) { g->fill_rect(x, y, bw, bh, g->color_accent()); g->draw_rect(x, y, bw, bh, g->color_fg()); }
        else            g->draw_rect(x, y, bw, bh, g->color_border());
        const char *lab = btn_label(i, paused);
        g->draw_text(x + (bw - g->text_width(lab)) / 2, y + 8, lab, g->color_fg());
    }
}
static void draw_ui(const feature_host_t *h, const char *song, uint32_t size, int idx, int total_tracks,
                    int sel, int paused, int seek_mode, int seek_pct) {
    const gui_api_t *g = h->gui;
    uint32_t tick = h->get_tick();
    uint32_t total   = g_total_sec ? g_total_sec
                       : (g_bitrate > 0) ? ((uint64_t)size * 8u) / ((uint32_t)g_bitrate * 1000u) : 0;
    uint32_t elapsed = g_played / AUDIO_SAMPLE_RATE;
    int pct = (total > 0) ? (int)((uint64_t)elapsed * 100u / total) : 0;
    if (pct > 100) pct = 100;
    int barpct = seek_mode ? seek_pct : pct;                  /* scrub target while seeking */
    uint32_t showsec = seek_mode ? (uint32_t)((uint64_t)total * seek_pct / 100) : elapsed;

    g->fill_rect(0, 23, 320, 195, g->color_bg());            /* clear content area (theme bg) */
    h->draw_header(ms(MS_TITLE));

    char dn[96]; disp_name(dn, sizeof(dn), song);
    g->draw_text_marquee(20, 44, 280, dn, g->color_fg(), true, tick);

    char te[6], tt[6], line[20];
    fmt_mmss(te, showsec); fmt_mmss(tt, total);
    int k = 0;
    for (const char *p = te; *p; p++) line[k++] = *p;
    line[k++] = ' '; line[k++] = '/'; line[k++] = ' ';
    for (const char *p = tt; *p; p++) line[k++] = *p;
    line[k] = 0;
    g->draw_text(20, 74, line, g->color_fg());

    /* "Track X/N" right-aligned-ish */
    char num[16]; int m = 0, x = idx + 1, nn = total_tracks;
    num[m++]='T'; num[m++]='r'; num[m++]='a'; num[m++]='c'; num[m++]='k'; num[m++]=' ';
    if (x >= 10) num[m++] = (char)('0' + (x / 10) % 10);
    num[m++] = (char)('0' + x % 10); num[m++] = '/';
    if (nn >= 10) num[m++] = (char)('0' + (nn / 10) % 10);
    num[m++] = (char)('0' + nn % 10); num[m] = 0;
    g->draw_text(320 - 20 - g->text_width(num), 74, num, g->color_fg());

    /* progress bar; while scrubbing, the fill is fg so the seek target reads distinctly */
    g->draw_progress_bar(20, 100, 280, 14, barpct, g->color_border(),
                         seek_mode ? g->color_fg() : g->color_accent());

    draw_buttons(g, sel, paused);
    h->draw_footer(seek_mode ? ms(MS_FOOTER_SEEK) : ms(MS_FOOTER_NORMAL));
    h->present();
}

enum { ACT_STOP = 0, ACT_NEXT, ACT_PREV, ACT_END, ACT_GOTO };

/* ---- Winamp-style playlist pop-up (overlay; music keeps playing behind it) ---- */
static int g_pl_open, g_pl_sel, g_pl_btn, g_pl_top;
enum { PB_ADD = 0, PB_PLAY, PB_REMOVE, NPB };

static void draw_playlist(const feature_host_t *h, int cur) {
    const gui_api_t *g = h->gui;
    const int x = 14, y = 28, w = 292, rowh = 17, rows = 8;   /* list area */
    g->fill_rect(0, 23, 320, 195, g->color_bg());
    h->draw_header(ms(MS_PLAYLIST));
    g->draw_rect(x, y, w, rows * rowh + 4, g->color_border());
    if (g_pl_sel < g_pl_top) g_pl_top = g_pl_sel;
    if (g_pl_sel >= g_pl_top + rows) g_pl_top = g_pl_sel - rows + 1;
    for (int i = 0; i < rows; i++) {
        int idx = g_pl_top + i;
        if (idx >= g_qn) break;
        int ry = y + 2 + i * rowh;
        if (idx == g_pl_sel) g->fill_rect(x + 1, ry, w - 2, rowh, g->color_accent());
        if (idx == cur) g->draw_text(x + 4, ry + 2, ">", g->color_fg());   /* playing marker */
        char dn[80]; disp_name(dn, sizeof(dn), g_qname[idx]);
        g->draw_text_marquee(x + 16, ry + 2, w - 22, dn, g->color_fg(), idx == g_pl_sel, h->get_tick());
    }
    const char *bl[NPB] = { ms(MS_ADD), ms(MS_PLAY), ms(MS_REMOVE) };
    const int bw = 86, gap = 8, bh = 22, by = 184;
    int bx = (320 - (NPB * bw + (NPB - 1) * gap)) / 2;
    for (int i = 0; i < NPB; i++) {
        int xx = bx + i * (bw + gap);
        if (i == g_pl_btn) { g->fill_rect(xx, by, bw, bh, g->color_accent()); g->draw_rect(xx, by, bw, bh, g->color_fg()); }
        else                 g->draw_rect(xx, by, bw, bh, g->color_border());
        g->draw_text(xx + (bw - g->text_width(bl[i])) / 2, by + 6, bl[i], g->color_fg());
    }
    h->draw_footer(ms(MS_FOOTER_PLAYLIST));
    h->present();
}

/* Remove g_pl_sel from the queue, shifting it down; adjust the play index *pcur. */
static void pl_remove(int *pcur) {
    int r = g_pl_sel;
    if (r < 0 || r >= g_qn) return;
    for (int i = r; i < g_qn - 1; i++) { memcpy(g_qname[i], g_qname[i + 1], Q_NAME); g_qsize[i] = g_qsize[i + 1]; }
    g_qn--;
    if (r < *pcur) (*pcur)--;
    if (g_pl_sel >= g_qn) g_pl_sel = (g_qn > 0) ? g_qn - 1 : 0;
}

/* Feed the DMA half the hardware isn't reading; silence (and freeze) while paused. Sets
 * *action=ACT_END + returns 1 once the stream is drained. */
static int feed(int paused, int *last_half, int *drained, int *action) {
    uint32_t pos = audio_pos(OUT_SAMPLES);
    int fill_half = ((pos < OUT_HALF) ? 0 : 1) ^ 1;
    if (fill_half == *last_half) return 0;
    int base = fill_half * OUT_HALF;
    if (paused) {
        memset(g_out + base, 0, OUT_HALF * sizeof(int16_t));
    } else {
        int n = produce(g_out + base, OUT_HALF);
        g_played += (uint32_t)n;
        if (n < OUT_HALF) {
            memset(g_out + base + n, 0, (size_t)(OUT_HALF - n) * sizeof(int16_t));
            if (n == 0 && ++(*drained) >= 2) { *action = ACT_END; }
        }
    }
    audio_clean_range(g_out + base, OUT_HALF * sizeof(int16_t));
    *last_half = fill_half;
    return (*action == ACT_END && !paused && *drained >= 2);
}

/* Stream + play the track at *pcur with the full UI + playlist pop-up. Returns why it
 * stopped (ACT_GOTO leaves the new track index in *pcur). */
/* Open + start the track at index `idx` from the beginning (skip ID3 tag, compute
 * duration, prime + start the DMA). Returns 1 on success, 0 if the file won't open. */
static int load_track(const feature_host_t *h, int idx, uint32_t start_byte, int silent) {
    if (g_fp) { h->file_close(g_fp); g_fp = 0; }
    g_h = h;
    decode_reset();
    g_fp = h->file_open(g_qname[idx]);
    if (!g_fp) return 0;
    refill();
    g_id3 = (g_in_len >= 10 && g_in[0]=='I' && g_in[1]=='D' && g_in[2]=='3')
            ? 10 + syncsafe(&g_in[6]) : 0;
    if (g_id3 > 0 && h->file_seek && h->file_seek(g_fp, g_id3) == 0) {  /* skip ID3v2 tag */
        g_in_len = g_in_pos = 0; g_filepos = g_id3;
        refill();
    }
    compute_duration(g_qsize[idx], g_id3);
    if (start_byte > g_id3 && h->file_seek && h->file_seek(g_fp, start_byte) == 0) {  /* resume pos */
        g_in_len = g_in_pos = 0; g_eof = 0; g_filepos = start_byte;
        refill();
        if (g_qsize[idx] > g_id3) {
            uint32_t total = g_total_sec ? g_total_sec
                : (g_bitrate > 0) ? ((uint64_t)g_qsize[idx] * 8u) / ((uint32_t)g_bitrate * 1000u) : 0;
            uint32_t audio = g_qsize[idx] - g_id3;
            g_played = total ? (uint32_t)(((uint64_t)total * (start_byte - g_id3) / audio) * AUDIO_SAMPLE_RATE) : 0;
        }
    }
    int got = produce(g_out, OUT_SAMPLES);          /* fully pre-buffer before starting */
    if (got < OUT_SAMPLES) memset(g_out + got, 0, (size_t)(OUT_SAMPLES - got) * sizeof(int16_t));
    g_played += (uint32_t)got;
    if (silent) memset(g_out, 0, sizeof(g_out));    /* start paused -> silence out, decoder stays primed */
    audio_clean_range(g_out, sizeof(g_out));
    audio_start(g_out, OUT_SAMPLES);                /* SINGLE start -> no blip, buffer pre-filled */
    return 1;
}

/* The idle/stopped screen (no track loaded): empty playlist -> prompt to Add; otherwise a
 * "stopped" prompt. The button bar is shown so the user can reach Playlist / Play. */
static void draw_idle(const feature_host_t *h, int sel) {
    const gui_api_t *g = h->gui;
    g->fill_rect(0, 23, 320, 195, g->color_bg());
    h->draw_header(ms(MS_TITLE));
    if (g_qn == 0) {
        g->draw_text(20, 64, ms(MS_EMPTY), g->color_fg());
        g->draw_text(20, 90, ms(MS_HELP_ADD), g->color_fg());
    } else {
        g->draw_text(20, 64, ms(MS_STOPPED), g->color_fg());
        g->draw_text(20, 90, ms(MS_HELP_PLAY), g->color_fg());
    }
    draw_buttons(g, sel, 1);
    h->draw_footer(ms(MS_FOOTER_NORMAL));
    h->present();
}

/* Re-open the loaded track at its current byte position (after the Add picker, which
 * re-mounts filesystems and can invalidate the open file). Preserves elapsed/duration. */
static void reopen_at_byte(const feature_host_t *h, int idx, uint32_t byte, int *last_half, int *drained) {
    if (g_fp) { h->file_close(g_fp); g_fp = 0; }
    g_fp = h->file_open(g_qname[idx]);
    if (!g_fp) return;
    if (byte && h->file_seek) h->file_seek(g_fp, byte);
    g_filepos = byte; g_in_len = g_in_pos = 0; g_eof = 0;
    g_frame_n = g_frame_pos = 0; g_primed = 0; mp3dec_init(&g_dec);
    int g2 = produce(g_out, OUT_SAMPLES);
    if (g2 < OUT_SAMPLES) memset(g_out + g2, 0, (size_t)(OUT_SAMPLES - g2) * sizeof(int16_t));
    audio_clean_range(g_out, sizeof(g_out));
    audio_start(g_out, OUT_SAMPLES);
    *last_half = -1; *drained = 0;
}

/* ---- persistent state (host->state_save/load -> internal LFS): playlist + position ----
 * Serialized into g_in (the raw-MP3 buffer, free at save/load time -> no extra RAM).
 * Format: ver, paused, count, cur, byte_pos, then per song: size(u32) + namelen(u16) + name. */
#define STATE_VER 2
static uint32_t st_put32(uint8_t *b, uint32_t o, uint32_t v) {
    b[o] = (uint8_t)v; b[o+1] = (uint8_t)(v >> 8); b[o+2] = (uint8_t)(v >> 16); b[o+3] = (uint8_t)(v >> 24);
    return o + 4;
}
static uint32_t st_get32(const uint8_t *b, uint32_t o) {
    return (uint32_t)b[o] | ((uint32_t)b[o+1] << 8) | ((uint32_t)b[o+2] << 16) | ((uint32_t)b[o+3] << 24);
}
static uint32_t state_serialize(int cur, uint32_t byte_pos, int paused, uint32_t source) {
    uint8_t *b = g_in; uint32_t o = 0;
    b[o++] = STATE_VER; b[o++] = paused ? 1 : 0;
    o = st_put32(b, o, (uint32_t)g_qn);
    o = st_put32(b, o, (uint32_t)cur);
    o = st_put32(b, o, byte_pos);
    o = st_put32(b, o, source);             /* partition token, so paths resolve after reboot */
    for (int i = 0; i < g_qn; i++) {
        uint32_t nl = (uint32_t)strlen(g_qname[i]);
        if (o + 6 + nl > IN_CAP) break;
        o = st_put32(b, o, g_qsize[i]);
        b[o++] = (uint8_t)nl; b[o++] = (uint8_t)(nl >> 8);
        memcpy(b + o, g_qname[i], nl); o += nl;
    }
    return o;
}
static int state_deserialize(uint32_t len, int *cur, uint32_t *byte_pos, int *paused, uint32_t *source) {
    const uint8_t *b = g_in;
    if (len < 18 || b[0] != STATE_VER) return 0;
    uint32_t o = 1;
    *paused = b[o++];
    int n = (int)st_get32(b, o); o += 4;
    *cur = (int)st_get32(b, o); o += 4;
    *byte_pos = st_get32(b, o); o += 4;
    *source = st_get32(b, o); o += 4;
    g_qn = 0;
    for (int i = 0; i < n && g_qn < Q_MAX; i++) {
        if (o + 6 > len) break;
        uint32_t sz = st_get32(b, o); o += 4;
        uint32_t nl = (uint32_t)b[o] | ((uint32_t)b[o+1] << 8); o += 2;
        if (o + nl > len || nl >= Q_NAME) break;
        memcpy(g_qname[g_qn], b + o, nl); g_qname[g_qn][nl] = 0; o += nl;
        g_qsize[g_qn] = sz; g_qn++;
    }
    if (*cur < 0 || *cur >= g_qn) *cur = 0;
    return g_qn > 0;
}

/* Unified player: idle (empty/stopped), playback, button bar, seek scrubber, and the
 * playlist pop-up (Add/Play/Remove) in one loop, so the queue can grow from empty. `initial`
 * is the launching file (NULL = Tools launch -> start empty; user builds the list via Add). */
static int cur = 0;
static int loaded = -1, paused = 0, sel = BTN_PLAY, seek_mode = 0, seek_pct = 0;
static int last_half = -1, drained = 0;
static int g_bg_active = 0;

static void mp3_bg_tick(void) {
    if (g_bg_active && loaded >= 0 && !paused) {
        int action = ACT_END;
        if (feed(paused, &last_half, &drained, &action)) {
            cur = (g_qn > 0) ? (cur + 1) % g_qn : 0;
            if (g_qn > 0 && load_track(g_h, cur, 0, 0)) {
                loaded = cur;
                last_half = -1;
                drained = 0;
            } else {
                audio_stop();
                if (g_fp) { g_h->file_close(g_fp); g_fp = NULL; }
                loaded = -1;
                g_bg_active = 0;
                g_h->register_bg_tick(NULL);
            }
        }
    }
}

static void player(const feature_host_t *h, const char *initial) {
    g_h = h;
    if (g_bg_active) {
        g_bg_active = 0;
        h->register_bg_tick(NULL);
        if (initial) {
            char dir_temp[256];
            const char *base;
            split_path(initial, dir_temp, sizeof(dir_temp), &base);
            (void)base;
            if (strcmp(g_dir, dir_temp) != 0) {
                strcpy(g_dir, dir_temp);
                g_qn = 0;
                if (h->list_dir) h->list_dir(g_dir[0] ? g_dir : "/", q_add, 0);
                q_sort();
            }
            cur = 0;
            for (int i = 0; i < g_qn; i++)
                if (!ci_less(g_qname[i], initial) && !ci_less(initial, g_qname[i])) { cur = i; break; }
            if (g_qn == 0) q_add_full(initial, 0);

            paused = 0;
            seek_mode = 0;
            loaded = load_track(h, cur, 0, 0) ? cur : -1;
            last_half = -1;
            drained = 0;
        }
    } else {
        if (initial) { const char *base; split_path(initial, g_dir, sizeof(g_dir), &base); (void)base; }
        else         g_dir[0] = 0;
        g_qn = 0;
        if (initial && h->list_dir) h->list_dir(g_dir[0] ? g_dir : "/", q_add, 0);
        q_sort();
        cur = 0;
        if (initial) {
            for (int i = 0; i < g_qn; i++)
                if (!ci_less(g_qname[i], initial) && !ci_less(initial, g_qname[i])) { cur = i; break; }
            if (g_qn == 0) q_add_full(initial, 0);
        }

        g_pl_open = 0;
        loaded = -1; paused = 0; sel = BTN_PLAY; seek_mode = 0; seek_pct = 0;
        last_half = -1; drained = 0;

        /* Tools launch (no file): restore the saved playlist + position, if any. */
        uint32_t resume_pos = 0, resume_src = 0; int resume_paused = 0, have_resume = 0;
        if (!initial && h->state_load) {
            int got = h->state_load(g_in, IN_CAP);
            if (got > 0) have_resume = state_deserialize((uint32_t)got, &cur, &resume_pos, &resume_paused, &resume_src);
        }
        /* Re-select the source partition BEFORE loading, so the stored paths resolve. */
        if (have_resume && resume_src && h->set_source) h->set_source(resume_src);

        /* Cold boot (first feature launch since power-on): come up PAUSED, don't auto-play (but
         * still restore the position). Same-session re-entry: restore the saved play/pause. */
        int first = (h->is_first_launch && h->is_first_launch());
        int start_paused = have_resume && first;
        if (g_qn > 0 && load_track(h, cur, have_resume ? resume_pos : 0, start_paused)) {
            loaded = cur;
            paused = have_resume ? (first ? 1 : resume_paused) : 0;
        }
    }

    while (1) {
        h->input_update();

        if (loaded >= 0) {                                  /* feed audio; auto-advance on EOF */
            int action = ACT_END;
            if (feed(paused, &last_half, &drained, &action)) {
                cur = (g_qn > 0) ? (cur + 1) % g_qn : 0;
                if (g_qn > 0 && load_track(h, cur, 0, 0)) { loaded = cur; last_half = -1; drained = 0; }
                else { audio_stop(); loaded = -1; }
            }
        }

        if (g_pl_open) {
            if (h->just_pressed(INPUT_B))           g_pl_open = 0;
            else if (h->just_pressed(INPUT_UP))   { if (g_pl_sel > 0) g_pl_sel--; }
            else if (h->just_pressed(INPUT_DOWN)) { if (g_pl_sel < g_qn - 1) g_pl_sel++; }
            else if (h->just_pressed(INPUT_LEFT))   g_pl_btn = (g_pl_btn - 1 + NPB) % NPB;
            else if (h->just_pressed(INPUT_RIGHT))  g_pl_btn = (g_pl_btn + 1) % NPB;
            else if (h->just_pressed(INPUT_A)) {
                if (g_pl_btn == PB_PLAY && g_qn > 0) {
                    cur = g_pl_sel; paused = 0; seek_mode = 0;
                    loaded = load_track(h, cur, 0, 0) ? cur : -1;
                    last_half = -1; drained = 0;
                    g_pl_open = 0;                  /* close the popup -> show now playing */
                }
                else if (g_pl_btn == PB_REMOVE && g_qn > 0) {
                    int was_loaded = (g_pl_sel == loaded);
                    pl_remove(&cur);                       /* shifts array, adjusts cur */
                    if (was_loaded) {
                        audio_stop();
                        if (g_fp) { h->file_close(g_fp); g_fp = 0; }
                        loaded = -1;
                        if (g_qn > 0) {
                            if (cur >= g_qn) cur = 0;
                            if (load_track(h, cur, 0, 0)) { loaded = cur; last_half = -1; drained = 0; paused = 0; }
                        }
                    } else if (loaded > g_pl_sel) {
                        loaded--;                          /* a track before the playing one shifted */
                    }
                }
                else if (g_pl_btn == PB_ADD && h->pick_file) {
                    uint32_t cur_byte = (loaded >= 0) ? g_filepos - (uint32_t)(g_in_len - g_in_pos) : 0;
                    if (loaded >= 0) audio_stop();
                    char pbuf[256]; uint32_t psize = 0; bool pdir = false;
                    if (h->pick_file("mp3", pbuf, sizeof(pbuf), &psize, &pdir)) {
                        if (pdir) {                          /* add every .mp3 in the folder */
                            size_t dl = strlen(pbuf);
                            if (dl < sizeof(g_dir)) memcpy(g_dir, pbuf, dl + 1);   /* q_add's join base */
                            if (h->list_dir) h->list_dir(pbuf, q_add, 0);
                        } else {
                            q_add_full(pbuf, psize);
                        }
                    }
                    if (loaded >= 0) reopen_at_byte(h, loaded, cur_byte, &last_half, &drained);
                }
            }
            draw_playlist(h, loaded);
        } else if (seek_mode && loaded >= 0) {
            uint32_t size = g_qsize[cur];
            if (h->just_pressed(INPUT_LEFT))       { seek_pct -= 5; if (seek_pct < 0)   seek_pct = 0; }
            else if (h->just_pressed(INPUT_RIGHT)) { seek_pct += 5; if (seek_pct > 100) seek_pct = 100; }
            else if (h->just_pressed(INPUT_B))       seek_mode = 0;
            else if (h->just_pressed(INPUT_A)) {
                if (h->file_seek && size > 0) {
                    uint32_t audio = (size > g_id3) ? (size - g_id3) : size;
                    uint32_t target = g_id3 + (uint32_t)((uint64_t)audio * seek_pct / 100);
                    if (h->file_seek(g_fp, target) == 0) {
                        g_in_len = g_in_pos = 0; g_eof = 0; g_filepos = target;
                        g_frame_n = g_frame_pos = 0; g_primed = 0; mp3dec_init(&g_dec);
                        uint32_t total = g_total_sec ? g_total_sec
                                         : (g_bitrate > 0) ? ((uint64_t)size * 8u) / ((uint32_t)g_bitrate * 1000u) : 0;
                        g_played = (uint32_t)(((uint64_t)total * seek_pct / 100) * AUDIO_SAMPLE_RATE);
                        int g2 = produce(g_out, OUT_SAMPLES);
                        if (g2 < OUT_SAMPLES) memset(g_out + g2, 0, (size_t)(OUT_SAMPLES - g2) * sizeof(int16_t));
                        audio_clean_range(g_out, sizeof(g_out)); audio_start(g_out, OUT_SAMPLES);
                        last_half = -1; drained = 0;
                    }
                }
                seek_mode = 0;
            }
            draw_ui(h, g_qname[cur], g_qsize[cur], cur, g_qn, BTN_SEEK, paused, 1, seek_pct);
        } else {
            if (h->just_pressed(INPUT_B)) break;                              /* quit */
            else if (h->just_pressed(INPUT_LEFT))   sel = (sel - 1 + NBTN) % NBTN;
            else if (h->just_pressed(INPUT_RIGHT))  sel = (sel + 1) % NBTN;
            else if (h->just_pressed(INPUT_A)) {
                if (sel == BTN_LIST) {
                    g_pl_open = 1;
                    g_pl_sel = (loaded >= 0) ? loaded : 0;
                    g_pl_btn = (g_qn > 0) ? PB_PLAY : PB_ADD;
                } else if (g_qn > 0) {
                    if (sel == BTN_PREV)      { cur = (cur - 1 + g_qn) % g_qn; loaded = load_track(h, cur, 0, 0) ? cur : -1; last_half = -1; drained = 0; paused = 0; }
                    else if (sel == BTN_NEXT) { cur = (cur + 1) % g_qn;        loaded = load_track(h, cur, 0, 0) ? cur : -1; last_half = -1; drained = 0; paused = 0; }
                    else if (sel == BTN_PLAY) {
                        if (loaded >= 0) paused = !paused;
                        else { if (cur >= g_qn) cur = 0; loaded = load_track(h, cur, 0, 0) ? cur : -1; last_half = -1; drained = 0; paused = 0; }
                    }
                    else if (sel == BTN_SEEK && loaded >= 0) {
                        uint32_t size = g_qsize[cur];
                        uint32_t total = g_total_sec ? g_total_sec : (g_bitrate > 0) ? ((uint64_t)size * 8u) / ((uint32_t)g_bitrate * 1000u) : 0;
                        uint32_t el = g_played / AUDIO_SAMPLE_RATE;
                        seek_pct = (total > 0) ? (int)((uint64_t)el * 100u / total) : 0;
                        seek_mode = 1;
                    }
                }
            }
            if (loaded >= 0) draw_ui(h, g_qname[cur], g_qsize[cur], cur, g_qn, sel, paused, 0, 0);
            else             draw_idle(h, sel);
        }
    }

    /* On quit, persist the playlist + current position (compute the byte BEFORE serialize,
     * which reuses g_in). Saves even when empty, so clearing the list persists too. */
    if (h->state_save) {
        uint32_t cur_byte = (loaded >= 0) ? g_filepos - (uint32_t)(g_in_len - g_in_pos) : 0;
        uint32_t src = h->source_id ? h->source_id() : 0;
        uint32_t len = state_serialize(loaded >= 0 ? loaded : cur, cur_byte, paused, src);
        h->state_save(g_in, len);
    }

    if (loaded >= 0 && !paused) {
        g_bg_active = 1;
        h->register_bg_tick(mp3_bg_tick);
    } else {
        g_bg_active = 0;
        h->register_bg_tick(NULL);
        if (g_fp) { audio_stop(); h->file_close(g_fp); g_fp = 0; }
    }
}

static void run(const feature_host_t *h, const char *path) {
    ms_set_lang(h->gui->lang_code());   /* select the compiled-in translation column for this run */
    player(h, path);   /* path == NULL -> empty playlist; user adds via Playlist > Add */
}

void init_module(const feature_host_t *host, feature_api_t *out) {
    (void)host;
    out->run = run;
}
