#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#include "config.h"
#include "sd_manager.h"
#include "ui_display.h"
#include "ui_touch.h"
#include "bt_a2dp_source.h"
#include "audio_pipeline.h"
#include "audio_eq.h"
#include "audio_dsp.h"
#include "hearing_cal.h"
#include "dsp_test.h"

static const char *TAG = "main";

// Track list — global so audio_pipeline can access it via extern
track_list_t tracks;

// ---------------------------------------------------------------------------
// Screen navigation
// ---------------------------------------------------------------------------
typedef enum {
    SCREEN_BT_CONNECT,   // BT discovery + device list
    SCREEN_MAIN_MENU,    // Menu: Now Playing / Calibrate / EQ
    SCREEN_NOW_PLAYING,  // Transport controls + EQ quick adjust
    SCREEN_CALIBRATE,    // Hearing test
    SCREEN_EQ_SETTINGS,  // Full EQ band view
} screen_t;

static screen_t s_screen = SCREEN_BT_CONNECT;
static bool     s_screen_dirty = true;  // Redraw needed

// EQ profile cycling: Flat → EDM → Harman OE → Harman IE → Hearing → Flat → ...
typedef enum {
    PROFILE_FLAT,
    PROFILE_EDM,
    PROFILE_HARMAN_OE,
    PROFILE_HARMAN_IE,
    PROFILE_HEARING,
    PROFILE_COUNT,
} profile_slot_t;

static profile_slot_t s_active_profile = PROFILE_FLAT;
static eq_profile_t   s_hearing_profile;
static bool           s_has_hearing = false;

static void apply_profile(profile_slot_t slot)
{
    s_active_profile = slot;
    switch (slot) {
    case PROFILE_FLAT:
        audio_eq_preset_flat();
        audio_eq_load_profile(audio_eq_get_profile());
        break;
    case PROFILE_EDM:
        audio_eq_preset_edm();
        break;
    case PROFILE_HARMAN_OE:
        audio_eq_preset_harman_oe();
        break;
    case PROFILE_HARMAN_IE:
        audio_eq_preset_harman_ie();
        break;
    case PROFILE_HEARING:
        if (s_has_hearing) {
            audio_eq_load_profile(&s_hearing_profile);
        }
        break;
    default:
        break;
    }
}

static void cycle_profile(void)
{
    profile_slot_t next = (s_active_profile + 1) % PROFILE_COUNT;
    // Skip hearing if not calibrated
    if (next == PROFILE_HEARING && !s_has_hearing) {
        next = PROFILE_FLAT;
    }
    apply_profile(next);
}

static const char *profile_name(void)
{
    switch (s_active_profile) {
    case PROFILE_FLAT:      return "FLAT";
    case PROFILE_EDM:       return "EDM";
    case PROFILE_HARMAN_OE: return "HARMAN OE";
    case PROFILE_HARMAN_IE: return "HARMAN IE";
    case PROFILE_HEARING:   return "HEARING";
    default:                return "???";
    }
}

static void set_screen(screen_t scr)
{
    s_screen = scr;
    s_screen_dirty = true;
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------

static void draw_header(const char *title)
{
    ui_display_fill(COLOR_BLACK);
    int w = (int)strlen(title) * 8;
    ui_display_string((DISPLAY_WIDTH - w) / 2, 5, title, COLOR_CYAN, COLOR_BLACK);
    ui_display_fill_rect(20, 22, 280, 2, COLOR_PURPLE);
}

static void draw_back_button(void)
{
    ui_display_fill_rect(5, 5, 40, 16, COLOR_BLUE);
    ui_display_string(9, 9, "<MENU", COLOR_WHITE, COLOR_BLUE);
}

static bool is_back_tap(touch_point_t *pt)
{
    return (pt->x < 50 && pt->y < 25);
}

// ---------------------------------------------------------------------------
// BT Connect screen
// ---------------------------------------------------------------------------
static void draw_bt_connect(void)
{
    draw_header("ESP32 EDM Player");

    int count;
    const bt_device_t *devs = bt_a2dp_get_devices(&count);

    bt_state_t st = bt_a2dp_get_state();
    if (st == BT_STATE_DISCOVERING) {
        ui_display_string(20, 100, "Scanning for BT devices...", COLOR_WHITE, COLOR_BLACK);
    } else if (count == 0) {
        ui_display_string(20, 100, "No devices found.", COLOR_WHITE, COLOR_BLACK);
    } else {
        ui_display_string(20, 35, "Tap a device to connect:", COLOR_WHITE, COLOR_BLACK);
        char buf[48];
        for (int i = 0; i < count && i < 7; i++) {
            int y = 52 + i * 16;
            snprintf(buf, sizeof(buf), " %d. %.28s", i + 1, devs[i].name);
            ui_display_string(20, y, buf, COLOR_CYAN, COLOR_BLACK);
        }
    }

    if (st == BT_STATE_CONNECTING) {
        ui_display_string(60, 210, "Connecting...", COLOR_YELLOW, COLOR_BLACK);
    } else if (st == BT_STATE_CONNECTED || st == BT_STATE_STREAMING) {
        ui_display_string(40, 210, "Connected! Starting...", COLOR_GREEN, COLOR_BLACK);
    }
}

// ---------------------------------------------------------------------------
// Main menu screen
// ---------------------------------------------------------------------------
static void draw_main_menu(void)
{
    draw_header("ESP32 EDM Player");

    // Three large menu buttons
    ui_display_fill_rect(40, 50, 240, 40, COLOR_BLUE);
    ui_display_string(100, 64, "Now Playing", COLOR_WHITE, COLOR_BLUE);

    ui_display_fill_rect(40, 110, 240, 40, COLOR_PURPLE);
    ui_display_string(76, 124, "Hearing Calibrate", COLOR_WHITE, COLOR_PURPLE);

    ui_display_fill_rect(40, 170, 240, 40, RGB565(0, 128, 128));
    ui_display_string(100, 184, "EQ Settings", COLOR_WHITE, RGB565(0, 128, 128));
}

// ---------------------------------------------------------------------------
// Now Playing screen
// ---------------------------------------------------------------------------
static void draw_now_playing(void)
{
    draw_header("Now Playing");
    draw_back_button();

    int idx = audio_pipeline_get_current_track();
    if (idx >= 0 && idx < tracks.count) {
        char name[40];
        snprintf(name, sizeof(name), "%.38s", tracks.tracks[idx].filename);
        char *dot = strrchr(name, '.');
        if (dot) *dot = '\0';
        ui_display_string(10, 35, name, COLOR_YELLOW, COLOR_BLACK);

        char info[32];
        snprintf(info, sizeof(info), "Track %d / %d", idx + 1, tracks.count);
        ui_display_string(10, 55, info, COLOR_WHITE, COLOR_BLACK);
    }

    // Transport buttons
    ui_display_fill_rect(20, 80, 80, 30, COLOR_BLUE);
    ui_display_string(36, 89, "<< PREV", COLOR_WHITE, COLOR_BLUE);

    ui_display_fill_rect(120, 80, 80, 30, COLOR_GREEN);
    audio_state_t st = audio_pipeline_get_state();
    if (st == AUDIO_STATE_PAUSED) {
        ui_display_string(140, 89, "PLAY", COLOR_BLACK, COLOR_GREEN);
    } else {
        ui_display_string(136, 89, "PAUSE", COLOR_BLACK, COLOR_GREEN);
    }

    ui_display_fill_rect(220, 80, 80, 30, COLOR_BLUE);
    ui_display_string(236, 89, "NEXT >>", COLOR_WHITE, COLOR_BLUE);

    // --- EQ Profile cycle button (big, prominent) ---
    ui_display_fill_rect(20, 118, 280, 2, COLOR_PURPLE);

    // Profile button — tap to cycle FLAT → EDM → HEARING
    uint16_t prof_col;
    switch (s_active_profile) {
    case PROFILE_FLAT:    prof_col = RGB565(80, 80, 80); break;
    case PROFILE_EDM:     prof_col = COLOR_PURPLE;       break;
    case PROFILE_HEARING: prof_col = COLOR_CYAN;         break;
    default:              prof_col = COLOR_WHITE;         break;
    }
    ui_display_fill_rect(20, 128, 280, 40, prof_col);

    char prof_label[32];
    snprintf(prof_label, sizeof(prof_label), "EQ: %s", profile_name());
    int pw = (int)strlen(prof_label) * 8;
    ui_display_string((DISPLAY_WIDTH - pw) / 2, 136, prof_label, COLOR_WHITE, prof_col);
    ui_display_string(20, 152, "tap to switch", COLOR_BLACK, prof_col);

    // --- DSP toggle button ---
    bool xf = audio_dsp_get_crossfeed();
    ui_display_fill_rect(60, 175, 200, 25, xf ? COLOR_CYAN : RGB565(60, 60, 60));
    ui_display_string(100, 181, xf ? "CROSSFEED ON" : "CROSSFEED OFF", COLOR_WHITE,
                      xf ? COLOR_CYAN : RGB565(60, 60, 60));

    // Band visualization (compact)
    const eq_profile_t *prof = audio_eq_get_profile();
    if (prof->band_count > 0) {
        for (int b = 0; b < prof->band_count && b < 10; b++) {
            int bx = 10 + b * 30;
            float g = prof->bands[b].gain_db;
            int bar_h = (int)(fabsf(g) * 1.5f);
            if (bar_h > 15) bar_h = 15;
            uint16_t col = (g > 0) ? COLOR_GREEN : COLOR_RED;
            int bar_y = (g > 0) ? 215 - bar_h : 215;
            if (bar_h > 0)
                ui_display_fill_rect(bx, bar_y, 20, bar_h, col);
            ui_display_fill_rect(bx, 215, 20, 1, RGB565(60, 60, 60));
        }
    }

    // Stats row
    char stats[48];
    uint32_t ur, total;
    audio_pipeline_get_stats(&ur, &total);
    size_t heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    snprintf(stats, sizeof(stats), "Heap:%uKB Ur:%lu/%lu",
             (unsigned)(heap / 1024), (unsigned long)ur, (unsigned long)total);
    ui_display_string(10, 230, stats, COLOR_GREEN, COLOR_BLACK);
}

// ---------------------------------------------------------------------------
// Calibration screen
// ---------------------------------------------------------------------------
static void draw_calibrate(void)
{
    draw_header("Hearing Calibration");
    draw_back_button();

    const cal_status_t *cal = hearing_cal_get_status();

    if (cal->state == CAL_STATE_IDLE) {
        ui_display_string(20, 60, "This test plays tones at", COLOR_WHITE, COLOR_BLACK);
        ui_display_string(20, 75, "different frequencies.", COLOR_WHITE, COLOR_BLACK);
        ui_display_string(20, 95, "Tap 'I HEAR IT' as soon as", COLOR_WHITE, COLOR_BLACK);
        ui_display_string(20, 110, "you can hear the tone.", COLOR_WHITE, COLOR_BLACK);

        ui_display_fill_rect(60, 150, 200, 45, COLOR_GREEN);
        ui_display_string(100, 166, "START TEST", COLOR_BLACK, COLOR_GREEN);

    } else if (cal->state == CAL_STATE_PLAYING || cal->state == CAL_STATE_PAUSE) {
        // Progress
        char prog[32];
        snprintf(prog, sizeof(prog), "Frequency %d / %d",
                 cal->current_freq_idx + 1, CAL_NUM_FREQS);
        ui_display_string(80, 40, prog, COLOR_WHITE, COLOR_BLACK);

        // Current frequency
        char freq[16];
        if (CAL_TEST_FREQS[cal->current_freq_idx] >= 1000)
            snprintf(freq, sizeof(freq), "%.0f kHz",
                     CAL_TEST_FREQS[cal->current_freq_idx] / 1000);
        else
            snprintf(freq, sizeof(freq), "%.0f Hz",
                     CAL_TEST_FREQS[cal->current_freq_idx]);

        // Big frequency display
        int fw = (int)strlen(freq) * 16;
        // Use 2x text for frequency
        ui_display_string((DISPLAY_WIDTH - fw) / 2 + 30, 75, freq, COLOR_CYAN, COLOR_BLACK);

        // Volume ramp bar
        ui_display_fill_rect(30, 110, 260, 20, RGB565(40, 40, 40));
        float vol_pct = (cal->current_volume - 0.002f) / (0.5f - 0.002f);
        if (vol_pct < 0) vol_pct = 0;
        if (vol_pct > 1) vol_pct = 1;
        int bar_w = (int)(vol_pct * 256);
        uint16_t bar_col = (vol_pct < 0.5f) ? COLOR_GREEN :
                           (vol_pct < 0.8f) ? COLOR_YELLOW : COLOR_RED;
        ui_display_fill_rect(32, 112, bar_w, 16, bar_col);

        ui_display_string(100, 135, "Volume ramping up...", COLOR_WHITE, COLOR_BLACK);

        // "I hear it" button — big target for easy tapping
        ui_display_fill_rect(30, 160, 260, 60, COLOR_GREEN);
        ui_display_string(100, 183, "I HEAR IT!", COLOR_BLACK, COLOR_GREEN);

        // Done frequencies shown as dots
        for (int i = 0; i < CAL_NUM_FREQS; i++) {
            int dx = 40 + i * 32;
            uint16_t c = cal->freq_done[i] ? COLOR_GREEN :
                        (i == cal->current_freq_idx) ? COLOR_YELLOW : RGB565(60,60,60);
            ui_display_fill_rect(dx, 230, 16, 8, c);
        }

    } else if (cal->state == CAL_STATE_DONE) {
        ui_display_string(40, 45, "Calibration Complete!", COLOR_GREEN, COLOR_BLACK);

        // Show results
        for (int i = 0; i < CAL_NUM_FREQS; i++) {
            char line[40];
            const char *unit = (CAL_TEST_FREQS[i] >= 1000) ? "kHz" : "Hz";
            float fval = (CAL_TEST_FREQS[i] >= 1000) ?
                          CAL_TEST_FREQS[i] / 1000 : CAL_TEST_FREQS[i];
            snprintf(line, sizeof(line), "%.0f%s: %+.1f dB",
                     fval, unit, cal->thresholds[i]);
            uint16_t col = (cal->thresholds[i] > 20) ? COLOR_RED :
                          (cal->thresholds[i] > 10) ? COLOR_YELLOW : COLOR_GREEN;
            ui_display_string(20, 70 + i * 16, line, col, COLOR_BLACK);
        }

        // Apply button
        ui_display_fill_rect(30, 175, 120, 40, COLOR_GREEN);
        ui_display_string(44, 189, "APPLY & SAVE", COLOR_BLACK, COLOR_GREEN);

        // Redo button
        ui_display_fill_rect(170, 175, 120, 40, COLOR_BLUE);
        ui_display_string(200, 189, "REDO", COLOR_WHITE, COLOR_BLUE);
    }
}

// ---------------------------------------------------------------------------
// EQ Settings screen
// ---------------------------------------------------------------------------
static void draw_eq_settings(void)
{
    draw_header("EQ Settings");
    draw_back_button();

    const eq_profile_t *prof = audio_eq_get_profile();
    char label[48];
    snprintf(label, sizeof(label), "Profile: %s", prof->name);
    ui_display_string(60, 35, label, COLOR_WHITE, COLOR_BLACK);

    // Preset buttons
    ui_display_fill_rect(10, 52, 60, 20, COLOR_BLUE);
    ui_display_string(18, 57, "FLAT", COLOR_WHITE, COLOR_BLUE);

    ui_display_fill_rect(80, 52, 60, 20, COLOR_PURPLE);
    ui_display_string(92, 57, "EDM", COLOR_WHITE, COLOR_PURPLE);

    bool bypassed = audio_eq_get_bypass();
    ui_display_fill_rect(220, 52, 90, 20, bypassed ? COLOR_RED : COLOR_GREEN);
    ui_display_string(228, 57, bypassed ? "BYPASSED" : "ACTIVE", COLOR_WHITE,
                      bypassed ? COLOR_RED : COLOR_GREEN);

    // Band display with +/- buttons
    if (prof->band_count > 0) {
        for (int b = 0; b < prof->band_count && b < 10; b++) {
            int row = b / 2;
            int col = b % 2;
            int bx = 10 + col * 160;
            int by = 82 + row * 28;

            char freq_s[10];
            if (prof->bands[b].freq >= 1000)
                snprintf(freq_s, sizeof(freq_s), "%.0fk", prof->bands[b].freq / 1000);
            else
                snprintf(freq_s, sizeof(freq_s), "%.0f", prof->bands[b].freq);

            float g = prof->bands[b].gain_db;

            // [-] button
            ui_display_fill_rect(bx, by, 16, 16, COLOR_RED);
            ui_display_string(bx + 4, by + 4, "-", COLOR_WHITE, COLOR_RED);

            // Label
            char val[16];
            snprintf(val, sizeof(val), "%s %+.0f", freq_s, g);
            uint16_t vc = (g > 0) ? COLOR_GREEN : (g < 0) ? COLOR_RED : COLOR_WHITE;
            ui_display_string(bx + 20, by + 4, val, vc, COLOR_BLACK);

            // [+] button
            ui_display_fill_rect(bx + 110, by, 16, 16, COLOR_GREEN);
            ui_display_string(bx + 114, by + 4, "+", COLOR_BLACK, COLOR_GREEN);
        }
    } else {
        ui_display_string(60, 120, "No EQ bands active", COLOR_WHITE, COLOR_BLACK);
        ui_display_string(30, 140, "Run calibration or pick a preset", COLOR_WHITE, COLOR_BLACK);
    }
}

// ---------------------------------------------------------------------------
// App entry
// ---------------------------------------------------------------------------
void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  ESP32 EDM Player");
    ESP_LOGI(TAG, "========================================");

    // --- NVS ---
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // --- SD card ---
    ret = sd_manager_init();
    if (ret != ESP_OK) { ESP_LOGE(TAG, "SD init failed"); return; }
    sd_manager_scan(&tracks);

    // --- Display + Touch ---
    ui_display_init();
    ui_touch_init();

    // --- BT ---
    bt_a2dp_init();

    // --- Audio pipeline + EQ + Calibration ---
    audio_pipeline_init();
    hearing_cal_init();

    // Try loading saved hearing profile
    // Try loading saved hearing profile from SD card
    if (audio_eq_load_profile_file("hearing.txt") == ESP_OK) {
        memcpy(&s_hearing_profile, audio_eq_get_profile(), sizeof(eq_profile_t));
        s_has_hearing = true;
        s_active_profile = PROFILE_HEARING;
        ESP_LOGI(TAG, "Loaded saved hearing profile from SD");
    }

    // --- Start BT discovery ---
    set_screen(SCREEN_BT_CONNECT);
    bt_a2dp_start_discovery();

    // --- Main loop ---
    TickType_t last_touch = 0;
    bool playing_started = false;
    int prev_track = -1;
    audio_state_t prev_audio_state = AUDIO_STATE_IDLE;

    while (1) {
        bt_state_t bt_state = bt_a2dp_get_state();

        // Auto-transition: BT connected → start playing → go to menu
        if (!playing_started && (bt_state == BT_STATE_CONNECTED || bt_state == BT_STATE_STREAMING)) {
            audio_pipeline_cmd(AUDIO_CMD_PLAY, 0);
            playing_started = true;
            vTaskDelay(pdMS_TO_TICKS(500));
            set_screen(SCREEN_MAIN_MENU);
        }

        // Auto-refresh BT screen during discovery
        if (s_screen == SCREEN_BT_CONNECT) {
            static int prev_count = -1;
            static bt_state_t prev_bt = BT_STATE_IDLE;
            int count;
            bt_a2dp_get_devices(&count);
            if (count != prev_count || bt_state != prev_bt) {
                prev_count = count;
                prev_bt = bt_state;
                s_screen_dirty = true;
            }
        }

        // Auto-refresh now-playing when track changes
        if (s_screen == SCREEN_NOW_PLAYING) {
            int ct = audio_pipeline_get_current_track();
            audio_state_t as = audio_pipeline_get_state();
            if (ct != prev_track || as != prev_audio_state) {
                prev_track = ct;
                prev_audio_state = as;
                s_screen_dirty = true;
            }
        }

        // Auto-refresh calibration screen — but only every 500ms to keep touch responsive
        if (s_screen == SCREEN_CALIBRATE) {
            const cal_status_t *cal = hearing_cal_get_status();
            if (cal->state == CAL_STATE_PLAYING) {
                static TickType_t last_cal_redraw = 0;
                TickType_t now_t = xTaskGetTickCount();
                if ((now_t - last_cal_redraw) > pdMS_TO_TICKS(500)) {
                    last_cal_redraw = now_t;
                    s_screen_dirty = true;
                }
            }
        }

        // --- Redraw if needed ---
        if (s_screen_dirty) {
            s_screen_dirty = false;
            switch (s_screen) {
            case SCREEN_BT_CONNECT:  draw_bt_connect();  break;
            case SCREEN_MAIN_MENU:   draw_main_menu();   break;
            case SCREEN_NOW_PLAYING: draw_now_playing();  break;
            case SCREEN_CALIBRATE:   draw_calibrate();    break;
            case SCREEN_EQ_SETTINGS: draw_eq_settings();  break;
            }
        }

        // --- Handle touch ---
        touch_point_t pt;
        TickType_t now = xTaskGetTickCount();
        if (ui_touch_read(&pt) && (now - last_touch) > pdMS_TO_TICKS(150)) {
            last_touch = now;

            switch (s_screen) {
            case SCREEN_BT_CONNECT: {
                int count;
                bt_a2dp_get_devices(&count);
                if (bt_state == BT_STATE_IDLE && count > 0) {
                    int idx = (pt.y - 52) / 16;
                    if (idx >= 0 && idx < count && pt.y >= 52) {
                        bt_a2dp_connect(idx);
                        s_screen_dirty = true;
                    }
                }
                break;
            }

            case SCREEN_MAIN_MENU:
                if (pt.y >= 50 && pt.y < 90)       set_screen(SCREEN_NOW_PLAYING);
                else if (pt.y >= 110 && pt.y < 150) set_screen(SCREEN_CALIBRATE);
                else if (pt.y >= 170 && pt.y < 210) set_screen(SCREEN_EQ_SETTINGS);
                break;

            case SCREEN_NOW_PLAYING:
                if (is_back_tap(&pt)) {
                    set_screen(SCREEN_MAIN_MENU);
                } else if (pt.y >= 80 && pt.y <= 110) {
                    // Transport buttons
                    if (pt.x < 100)      audio_pipeline_cmd(AUDIO_CMD_PREV, 0);
                    else if (pt.x < 200) {
                        if (audio_pipeline_get_state() == AUDIO_STATE_PLAYING)
                            audio_pipeline_cmd(AUDIO_CMD_PAUSE, 0);
                        else
                            audio_pipeline_cmd(AUDIO_CMD_RESUME, 0);
                    }
                    else                 audio_pipeline_cmd(AUDIO_CMD_NEXT, 0);
                    s_screen_dirty = true;
                } else if (pt.y >= 125 && pt.y <= 170) {
                    // EQ profile cycle button
                    cycle_profile();
                    ESP_LOGI(TAG, "EQ profile: %s", profile_name());
                    s_screen_dirty = true;
                } else if (pt.y >= 175 && pt.y <= 200) {
                    // Crossfeed toggle
                    audio_dsp_set_crossfeed(!audio_dsp_get_crossfeed());
                    ESP_LOGI(TAG, "Crossfeed: %s", audio_dsp_get_crossfeed() ? "ON" : "OFF");
                    s_screen_dirty = true;
                }
                break;

            case SCREEN_CALIBRATE: {
                const cal_status_t *cal = hearing_cal_get_status();
                if (is_back_tap(&pt)) {
                    set_screen(SCREEN_MAIN_MENU);
                } else if (cal->state == CAL_STATE_IDLE && pt.y >= 150 && pt.y < 195) {
                    // START button
                    hearing_cal_start();
                    s_screen_dirty = true;
                } else if (cal->state == CAL_STATE_PLAYING && pt.y >= 155 && pt.y < 225) {
                    // "I HEAR IT" button
                    hearing_cal_confirm();
                    s_screen_dirty = true;
                } else if (cal->state == CAL_STATE_DONE) {
                    if (pt.x < 160 && pt.y >= 175 && pt.y < 215) {
                        // Apply & Save — also cache for profile cycling
                        hearing_cal_apply();
                        hearing_cal_save();
                        memcpy(&s_hearing_profile, audio_eq_get_profile(), sizeof(eq_profile_t));
                        s_has_hearing = true;
                        s_active_profile = PROFILE_HEARING;
                        set_screen(SCREEN_NOW_PLAYING);
                    } else if (pt.x >= 170 && pt.y >= 175 && pt.y < 215) {
                        // Redo
                        hearing_cal_start();
                        s_screen_dirty = true;
                    }
                }
                break;
            }

            case SCREEN_EQ_SETTINGS: {
                const eq_profile_t *prof = audio_eq_get_profile();
                if (is_back_tap(&pt)) {
                    set_screen(SCREEN_MAIN_MENU);
                } else if (pt.y >= 52 && pt.y < 72) {
                    // Preset buttons
                    if (pt.x >= 10 && pt.x < 70) {
                        audio_eq_preset_flat();
                        audio_eq_load_profile(audio_eq_get_profile());
                        s_screen_dirty = true;
                    } else if (pt.x >= 80 && pt.x < 140) {
                        audio_eq_preset_edm();
                        s_screen_dirty = true;
                    } else if (pt.x >= 220) {
                        audio_eq_set_bypass(!audio_eq_get_bypass());
                        s_screen_dirty = true;
                    }
                } else if (pt.y >= 82 && prof->band_count > 0) {
                    // Band +/- buttons
                    int row = (pt.y - 82) / 28;
                    int col = (pt.x >= 170) ? 1 : 0;
                    int b = row * 2 + col;
                    if (b < prof->band_count) {
                        int bx = 10 + col * 160;
                        if (pt.x >= bx && pt.x < bx + 16) {
                            audio_eq_set_band_gain(b, prof->bands[b].gain_db - 2.0f);
                            s_screen_dirty = true;
                        } else if (pt.x >= bx + 110 && pt.x < bx + 126) {
                            audio_eq_set_band_gain(b, prof->bands[b].gain_db + 2.0f);
                            s_screen_dirty = true;
                        }
                    }
                }
                break;
            }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
