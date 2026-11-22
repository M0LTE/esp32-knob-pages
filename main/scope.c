// 2.4 GHz scope: the knob's own Wi-Fi radio as a receive-only spectrum
// analyser. The receiver's debug "dump" engine copies raw baseband I/Q into a
// fixed 64 KB bank of internal SRAM; the CPU then reads it like any array.
// How to drive that engine (the registers, the SRAM bank and its owner code,
// the sample format, the receive-only PHY set-up and tuning off the Wi-Fi
// channels) comes from ESP-SDR by ESPARGOS (GPL-3.0,
// https://espargos.net/espsdr/), main/targets/esp32s3/receiver.c and
// main/common/; see NOTICE. Nothing here transmits: Wi-Fi runs with no
// interface (WIFI_MODE_NULL), the transmit path is switched off, and no test
// tone or calibration that keys the transmitter is started.
//
// The capture bank is taken out of the heap at link time and handed back in a
// normal boot, so it costs nothing until the scope is used. Entering the scope
// restarts into a scope boot (no Wi-Fi connection, web server or sources);
// leaving restarts back. Any crash in a scope boot comes back as a normal boot.
#include "scope.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/lock.h>

#include "board.h"
#include "esp_attr.h"
#include "esp_dsp.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_heap_caps_init.h"
#include "esp_log.h"
#include "esp_private/phy.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "heap_memory_layout.h"
#include "model.h"
#include "ota.h"
#include "platform.h"
#include "ui.h"
#include "soc/soc.h"

static const char *TAG = "scope";

// ---- the capture bank (ESP-SDR: S3 snapshot aperture, SRAM owner code 4) ----
#define IQ_BASE 0x3fcd0000u
#define IQ_END 0x3fce0000u
#define IQ_MAX 16380u
#define IQ ((volatile uint32_t *)IQ_BASE)
#define DUMP_CTRL 0x60033d5cu
#define DUMP_LANES 0x60033d90u
#define SRAM_OWNER 0x600c101cu
#define MARK 0xa5a0055au
SOC_RESERVE_MEMORY_REGION(IQ_BASE, IQ_END, knob_rf_dump);

// ---- PHY and ROM entry points (ESP-SDR receiver.c, rx_lo.h) ----
extern void stop_tx_tone(unsigned);
extern void rom_pbus_workmode(void);
extern void rom_pbus_xpd_rx_on(unsigned);
extern void rom_pbus_xpd_tx_off(void);
extern void rom_set_rxclk_en(unsigned);
extern void rom_rx_gain_force(unsigned, unsigned);
extern void set_chanfreq(unsigned, unsigned);
extern void set_rf_freq_offset(unsigned, unsigned, int);
extern unsigned rom_chip_i2c_readReg(unsigned, unsigned, unsigned);
extern void rom_chip_i2c_writeReg(unsigned, unsigned, unsigned, unsigned);

#define F_MIN 2200
#define F_MAX 2700

// ---- boot hand-over, kept across a restart (not across power-off) ----
#define MAGIC_SCOPE 0x53434f50u  // "SCOP": the next boot is a scope boot
#define MAGIC_BACK 0x4241434bu   // "BACK": coming back from (or failing to start) the scope
typedef struct {
    uint32_t magic, check;
    uint16_t test_secs, test_mhz;
    uint16_t test_span, pad;
} handover_t;
static RTC_NOINIT_ATTR handover_t s_ho;
static bool s_scope_boot, s_fast;
static handover_t s_test;  // this boot's test settings (scope boot) or the pending ones (normal boot)

void scope_boot_check(void)
{
    extern int _heap_start;
    bool valid = s_ho.check == (s_ho.magic ^ 0x5a5a5a5au);
    s_scope_boot = valid && s_ho.magic == MAGIC_SCOPE && (uintptr_t)&_heap_start <= IQ_BASE;
    s_fast = valid && (s_ho.magic == MAGIC_SCOPE || s_ho.magic == MAGIC_BACK);
    if (s_scope_boot) s_test = s_ho;
    // Used up: a crash or a reset from here on comes back as a normal boot.
    s_ho.magic = s_ho.check = 0;
    if (!s_scope_boot) {
        // Static data must stay below the bank (it does by about 150 KB).
        if ((uintptr_t)&_heap_start <= IQ_BASE) heap_caps_add_region(IQ_BASE, IQ_END);
    }
}

bool scope_boot(void) { return s_scope_boot; }
bool scope_fast_boot(void) { return s_fast; }

static void restart_with(uint32_t magic, const handover_t *t)
{
    handover_t h = {0};
    if (t) h = *t;
    h.magic = magic;
    h.check = magic ^ 0x5a5a5a5au;
    s_ho = h;
    platform_restart();
}

static void set_status(const char *st)
{
    model_lock();
    snprintf(g_model.sc_status, sizeof g_model.sc_status, "%s", st);
    model_unlock();
}

void scope_test(int secs, int mhz, int span)
{
    s_test.test_secs = secs < 10 ? 10 : secs > 600 ? 600 : secs;
    s_test.test_mhz = mhz;
    s_test.test_span = span;
    model_lock();
    g_model.sc_want = true;
    model_unlock();
}

void scope_enter(void)
{
    extern int _heap_start;
    if ((uintptr_t)&_heap_start > IQ_BASE) {
        set_status("NOT IN THIS BUILD");
        return;
    }
    // Restarting an update that is still on trial would roll it back.
    static int64_t last_check;
    int64_t now = esp_timer_get_time();
    if (last_check && now - last_check < 2000000) return;
    last_check = now;
    if (!ota_ready_for_sleep()) {
        set_status("CHECKING UPDATE, WAIT");
        return;
    }
    set_status("STOPPING WI-FI");
    ESP_LOGI(TAG, "restarting into the scope%s", s_test.test_secs ? " (test)" : "");
    vTaskDelay(pdMS_TO_TICKS(400));  // the page shows the status first
    restart_with(MAGIC_SCOPE, &s_test);
}

// ---- the receiver ----

typedef struct {
    int mhz;      // tuned centre
    int span;     // 40, 20, 10 or 5 MHz
    int fs;       // capture rate, MS/s: 40 or 16
    int n;        // FFT length
    int bins;     // FFT bins shown
    int gain;     // receiver gain index (manual)
} tune_t;

static tune_t s_t;
static uint32_t s_lpf_saved[2];
static int s_lpf_code;

static int nearest_span(float want)
{
    static const int spans[] = {40, 20, 10, 5};
    int best = 40;
    float d = 1e9f;
    for (int i = 0; i < 4; i++) {
        float e = fabsf(want - spans[i]);
        if (e < d) d = e, best = spans[i];
    }
    return best;
}

// ESP-SDR's S3 analogue filter curve (rx_bandwidth.h): filter code for a
// two-sided width in MHz.
static int lpf_code(int mhz)
{
    static const struct { uint8_t code, mhz; } c[] = {{0, 54}, {8, 36}, {16, 30}, {24, 21}, {32, 18}, {48, 15}, {60, 13}};
    if (mhz >= c[0].mhz) return c[0].code;
    for (int i = 1; i < 7; i++)
        if (mhz >= c[i].mhz)
            return c[i - 1].code + ((c[i].code - c[i - 1].code) * (c[i - 1].mhz - mhz) + (c[i - 1].mhz - c[i].mhz) / 2) /
                                       (c[i - 1].mhz - c[i].mhz);
    return c[6].code;
}

static void lpf_apply(void)
{
    for (int j = 0; j < 2; j++) {
        s_lpf_saved[j] = rom_chip_i2c_readReg(0x67, 0, 4 + j);
        rom_chip_i2c_writeReg(0x67, 0, 4 + j, (s_lpf_saved[j] & ~63u) | (unsigned)s_lpf_code);
    }
}

static void lpf_restore(void)
{
    for (int j = 0; j < 2; j++) rom_chip_i2c_writeReg(0x67, 0, 4 + j, s_lpf_saved[j]);
}

static void gain_set(int g)
{
    rom_rx_gain_force(0, g);  // the force line must see a low/high edge
    esp_rom_delay_us(2);
    rom_rx_gain_force(1, g);
    esp_rom_delay_us(2);
}

// Tune the receive LO and switch the receiver on with transmit off. Below
// 2210 MHz the LO runs at 6/5 of the wanted frequency through the alternate
// divider (ESP-SDR rx_lo.h).
static void radio_tune(int mhz)
{
    bool alt = mhz < 2210;
    unsigned khz = (unsigned)mhz * (alt ? 1200u : 1000u);
    bool channel = (mhz >= 2412 && mhz <= 2472 && (mhz - 2412) % 5 == 0) || mhz == 2484;
    _lock_t lock = phy_get_lock();
    _lock_acquire(&lock);
    unsigned v = rom_chip_i2c_readReg(0x65, 1, 0);
    if (v & 0x10u) rom_chip_i2c_writeReg(0x65, 1, 0, v & ~0x10u);
    set_chanfreq(channel ? (unsigned)mhz : 2412u, 0);
    if (!channel) set_rf_freq_offset(0, khz / 1000u, (int)(khz % 1000u));
    stop_tx_tone(1);
    rom_pbus_workmode();
    rom_pbus_xpd_tx_off();
    rom_pbus_xpd_rx_on(1);
    rom_set_rxclk_en(1);
    v = rom_chip_i2c_readReg(0x65, 1, 0);
    unsigned want = alt ? (v | 0x10u) : (v & ~0x10u);
    if (want != v) rom_chip_i2c_writeReg(0x65, 1, 0, want);
    esp_rom_delay_us(3000);
    gain_set(s_t.gain);
    _lock_release(&lock);
}

// One snapshot of n pairs into the bank. False on a timeout.
static bool capture(unsigned n)
{
    IQ[n - 1] = MARK;
    IQ[n / 2] = MARK;
    _lock_t lock = phy_get_lock();
    _lock_acquire(&lock);
    lpf_apply();
    uint32_t owner = REG_READ(SRAM_OWNER);
    int64_t start = esp_timer_get_time();
    REG_WRITE(DUMP_CTRL, 0);
    REG_WRITE(DUMP_LANES, 0 | (1u << 6) | (2u << 12) | (3u << 18));  // I, Q
    REG_WRITE(SRAM_OWNER, (owner & ~15u) | 4u);                     // the bank at 0x3fcd0000 to the engine
    uint32_t ctrl = 0x80000000u | (s_t.fs == 40 ? (1u << 15) : (1u << 16)) | n;
    REG_WRITE(DUMP_CTRL, ctrl);
    REG_WRITE(DUMP_CTRL, ctrl | (1u << 19));
    REG_WRITE(DUMP_CTRL, ctrl);
    while (!(REG_READ(DUMP_CTRL) & (1u << 18)) && esp_timer_get_time() - start < 20000) {
    }
    bool done = (REG_READ(DUMP_CTRL) & (1u << 18)) != 0;
    REG_WRITE(DUMP_CTRL, 0);
    REG_WRITE(SRAM_OWNER, owner);
    lpf_restore();
    _lock_release(&lock);
    return done && IQ[n - 1] != MARK && IQ[n / 2] != MARK;
}

static inline int s10(uint32_t v) { return (int)(v << 22) >> 22; }

// ---- spectrum ----
#define NMAX 1024
static float *s_buf;   // 2 * NMAX, FFT work
static float *s_win;   // NMAX
static float *s_pow;   // NMAX, power summed over the row
static uint8_t s_row[WF_W];

static void plan(int mhz, int span)
{
    s_t.mhz = mhz;
    s_t.span = span;
    s_t.fs = span >= 20 ? 40 : 16;
    s_t.n = NMAX;
    s_t.bins = (int)((float)span / s_t.fs * s_t.n + 0.5f);
    s_lpf_code = lpf_code(span >= 20 ? span + 2 : 13);
    dsps_wind_hann_f32(s_win, s_t.n);
}

// Window, FFT and add the power of each n-pair segment of the snapshot. The
// receiver puts RF above the LO at negative frequency, so Q is negated.
static void accumulate(unsigned pairs, int *clip, int *peak)
{
    int n = s_t.n;
    // DC (LO leakage) of this snapshot, removed before the window.
    int32_t si = 0, sq = 0;
    for (unsigned j = 0; j < pairs; j++) {
        uint32_t w = IQ[j];
        int i = s10(w), q = s10(w >> 10);
        si += i;
        sq += q;
        int a = i < 0 ? -i : i, b = q < 0 ? -q : q;
        if (a > *peak) *peak = a;
        if (b > *peak) *peak = b;
        if (a >= 500 || b >= 500) (*clip)++;
    }
    float di = (float)si / pairs, dq = (float)sq / pairs;
    for (unsigned seg = 0; seg + n <= pairs; seg += n) {
        for (int k = 0; k < n; k++) {
            uint32_t w = IQ[seg + k];
            s_buf[2 * k] = (s10(w) - di) * s_win[k];
            s_buf[2 * k + 1] = -(s10(w >> 10) - dq) * s_win[k];
        }
        dsps_fft2r_fc32(s_buf, n);
        dsps_bit_rev_fc32(s_buf, n);
        for (int k = 0; k < n; k++) {
            int d = (k + n / 2) & (n - 1);  // shifted: index 0 is -fs/2
            s_pow[d] += s_buf[2 * k] * s_buf[2 * k] + s_buf[2 * k + 1] * s_buf[2 * k + 1];
        }
    }
}

// Fold the shown bins to 360 columns (strongest bin per column), notch the
// LO leakage at the centre, and scale to 0..255 (about 0.4 dB a step).
static void make_row(int segs)
{
    int n = s_t.n, b = s_t.bins, first = n / 2 - b / 2;
    float bin_khz = s_t.fs * 1000.0f / n;
    int notch = (int)(60.0f / bin_khz + 0.5f);
    if (notch < 2) notch = 2;
    int c = n / 2;
    float lo = s_pow[c - notch - 1], hi = s_pow[c + notch + 1];
    for (int k = -notch; k <= notch; k++) s_pow[c + k] = lo + (hi - lo) * (k + notch + 1) / (2 * notch + 2);
    float scale = 1.0f / (segs > 0 ? segs : 1);
    for (int col = 0; col < WF_W; col++) {
        int b0 = first + col * b / WF_W, b1 = first + (col + 1) * b / WF_W;
        if (b1 <= b0) b1 = b0 + 1;
        float m = 0;
        for (int k = b0; k < b1; k++)
            if (s_pow[k] > m) m = s_pow[k];
        float db = 10.0f * log10f(m * scale + 1e-3f) - s_t.gain * 1.0f;  // roughly 1 dB a gain step
        int v = (int)((db + 45.0f) * 2.5f);
        s_row[col] = v < 0 ? 0 : v > 255 ? 255 : v;
    }
}

static void log_row(void)
{
    static const char ramp[] = " .:-=+*#%@";
    char line[73];
    int lo = 255, hi = 0;
    for (int i = 0; i < WF_W; i++) {
        if (s_row[i] < lo) lo = s_row[i];
        if (s_row[i] > hi) hi = s_row[i];
    }
    for (int i = 0; i < 72; i++) {
        int m = 0;
        for (int k = i * 5; k < i * 5 + 5; k++)
            if (s_row[k] > m) m = s_row[k];
        int r = hi > lo ? (m - lo) * 9 / (hi - lo) : 0;
        line[i] = ramp[r];
    }
    line[72] = 0;
    ESP_LOGI(TAG, "|%s| %d..%d", line, lo, hi);
}

static void scope_task(void *arg)
{
    s_buf = heap_caps_aligned_alloc(16, 2 * NMAX * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    // Only the FFT's own buffer needs to be fast; the rest can live in PSRAM.
    s_win = heap_caps_malloc(NMAX * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_pow = heap_caps_malloc(NMAX * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf || !s_win || !s_pow || dsps_fft2r_init_fc32(NULL, NMAX) != ESP_OK) {
        set_status("NO MEMORY FOR FFT");
        vTaskDeleteWithCaps(NULL);
        return;
    }
    s_t.gain = 56;
    int want_mhz = -1, want_span = -1, rows = 0, fails = 0, clip_rows = 0, quiet_rows = 0;
    int64_t last_tune = 0, last_log = esp_timer_get_time(), next_row = 0;
    uint32_t rows_at_log = 0;
    for (;;) {
        model_lock();
        float wc = g_model.sc_center_mhz_want, ws = g_model.sc_span_mhz_want;
        model_unlock();
        if (s_test.test_mhz) wc = s_test.test_mhz;
        if (s_test.test_span) ws = s_test.test_span;
        if (wc < 1) wc = 2437;
        int mhz = (int)lrintf(wc), span = nearest_span(ws > 0 ? ws : 40);
        if (mhz < F_MIN) mhz = F_MIN;
        if (mhz > F_MAX) mhz = F_MAX;
        int64_t now = esp_timer_get_time();
        // Retune on a change, and every few seconds in case the PHY moved it.
        if (mhz != want_mhz || span != want_span || now - last_tune > 3000000) {
            bool changed = mhz != want_mhz || span != want_span;
            if (changed) {
                set_status("TUNING");
                plan(mhz, span);
            }
            radio_tune(mhz);
            want_mhz = mhz;
            want_span = span;
            last_tune = now;
            if (changed) {
                ESP_LOGI(TAG, "tuned %d MHz, span %d MHz (%d MS/s, %d-point FFT, filter %d, gain %d)", mhz, span, s_t.fs,
                         s_t.n, s_lpf_code, s_t.gain);
                model_lock();
                g_model.sc_center_mhz = mhz;
                g_model.sc_span_mhz = span;
                model_unlock();
            }
        }
        // One row: a few snapshots, every segment averaged.
        memset(s_pow, 0, s_t.n * sizeof(float));
        int bursts = 2, segs = 0, clip = 0, peak = 0;
        unsigned pairs = (IQ_MAX / s_t.n) * s_t.n;
        for (int b = 0; b < bursts; b++) {
            if (!capture(pairs)) {
                fails++;
                continue;
            }
            accumulate(pairs, &clip, &peak);
            segs += pairs / s_t.n;
        }
        if (!segs) {
            set_status(fails > 20 ? "CAPTURE FAILED" : "NO SAMPLES");
            model_lock();
            g_model.sc_active = false;
            model_unlock();
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        make_row(segs);
        // A slow gain control of its own: back off on clipping, creep up when quiet.
        if (clip > (int)(pairs * bursts / 1000)) clip_rows++, quiet_rows = 0;
        else if (peak < 120) quiet_rows++, clip_rows = 0;
        else clip_rows = quiet_rows = 0;
        if (clip_rows >= 2 && s_t.gain > 8) s_t.gain -= 4, clip_rows = 0, radio_tune(mhz), last_tune = now;
        if (quiet_rows >= 15 && s_t.gain < 76) s_t.gain += 2, quiet_rows = 0, radio_tune(mhz), last_tune = now;
        model_lock();
        uint32_t seq = g_model.sc_row_seq;
        memcpy(g_model.sc_rows[seq % WF_ROWS], s_row, WF_W);
        memcpy(g_model.sc_spec, s_row, WF_W);
        g_model.sc_row_seq = seq + 1;
        g_model.sc_active = true;
        snprintf(g_model.sc_status, sizeof g_model.sc_status, "CAPTURING");
        model_unlock();
        rows++;
        if (now - last_log > 5000000) {
            float rps = (rows - rows_at_log) / ((now - last_log) / 1e6f);
            model_lock();
            g_model.sc_rows_per_s = rps;
            model_unlock();
            static uint32_t frames_at_log;
            uint32_t frames = board_frames_total();
            float fps = (frames - frames_at_log) / ((now - last_log) / 1e6f);
            frames_at_log = frames;
            ESP_LOGI(TAG, "%d MHz span %d: %.1f rows/s, fps %.1f, gain %d, peak %d, clipped %d, failed %d, heap %uK int (min %uK)",
                     mhz, span, rps, fps, s_t.gain, peak, clip, fails, (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
            log_row();
            rows_at_log = rows;
            last_log = now;
        }
        // About 15 rows a second.
        if (!next_row) next_row = now;
        next_row += 66000;
        int64_t wait = next_row - esp_timer_get_time();
        if (wait < 0) next_row = esp_timer_get_time(), wait = 0;
        vTaskDelay(pdMS_TO_TICKS(wait / 1000) + 1);
    }
}

void scope_run(void)
{
    set_status("STARTING RADIO");
    model_lock();
    g_model.sc_want = true;
    model_unlock();
    // Wi-Fi with no interface: the PHY is calibrated and powered, nothing
    // associates, scans or transmits frames.
    esp_err_t e = esp_event_loop_create_default();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (e == ESP_OK || e == ESP_ERR_INVALID_STATE) e = esp_wifi_init(&cfg);
    if (e == ESP_OK) e = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (e == ESP_OK) e = esp_wifi_set_mode(WIFI_MODE_NULL);
    if (e == ESP_OK) e = esp_wifi_start();
    if (e == ESP_OK) e = esp_wifi_set_ps(WIFI_PS_NONE);
    if (e == ESP_OK) e = esp_wifi_set_promiscuous(true);
    if (e == ESP_OK) e = esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "radio start failed: %s", esp_err_to_name(e));
        set_status("RADIO START FAILED");
    } else {
        ESP_LOGI(TAG, "radio up, heap %uK internal (min %uK)%s", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 s_test.test_secs ? ", test run" : "");
        xTaskCreatePinnedToCoreWithCaps(scope_task, "scope", 6144, NULL, 4, NULL, 0, MALLOC_CAP_SPIRAM);
    }
    int64_t t0 = esp_timer_get_time();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (s_test.test_secs && esp_timer_get_time() - t0 > s_test.test_secs * 1000000LL) {
            ESP_LOGI(TAG, "test time up");
            model_lock();
            g_model.sc_want = false;
            model_unlock();
        }
        model_lock();
        bool want = g_model.sc_want;
        model_unlock();
        if (!want) {
            set_status("RESTARTING");
            ESP_LOGI(TAG, "leaving the scope: restarting (heap %uK internal, min %uK)",
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
            vTaskDelay(pdMS_TO_TICKS(200));
            restart_with(MAGIC_BACK, NULL);
        }
    }
}
