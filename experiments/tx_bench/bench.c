#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <storage/storage.h>
#include <stdio.h>
#include <string.h>
#include "waveforms.h"
#include "tx_packet.h"
#include <toolbox/saved_struct.h>

/* OOK async, ~600 kbaud sampling clock, no packet engine/shaping.
 * PA values are nominal 915MHz settings, not measured radiated power.
 * A scoped US bench exception handles the official 915MHz lower bound. */
static const uint8_t preset[] = {0x02, 0x0D, 0x03, 0x07, 0x08, 0x32, 0x0B, 0x06, 0x10,
                                 0x0E, 0x11, 0x83, 0x12, 0x30, 0x13, 0x00, 0x14, 0x00,
                                 0x18, 0x18, 0x1D, 0x91, 0x21, 0xB6, 0x22, 0x11, 0x00,
                                 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static const int8_t power_dbm[] = {-30, -20, -10};
static const uint8_t power_pa[] = {0x03, 0x0E, 0x27};

typedef struct {
    FuriMessageQueue* input;
    FuriMutex* mutex;
    unsigned selected, power;
    bool sending, flood;
    uint32_t next_sequence, sequence, timestamp;
    int32_t utc_offset;
    uint8_t* dynamic_data;
    BenchWave dynamic_wave;
    char status[32];
    uint32_t actual_frequency;
    const BenchWave* active;
    volatile uint32_t position;
} Bench;

/* The firmware US table starts at 915MHz although Part 15 includes 902-928.
 * Permit only this fixed bench carrier, only on a provisioned US device.
 * The region API owns/frees its argument: both copies must be heap allocated.
 * Restore the exact region contents immediately after tuning, before TX starts.
 * No region file or firmware is changed. Not an emissions compliance test. */
static bool prepare_frequency(Bench* b) {
    if(furi_hal_region_is_frequency_allowed(BENCH_CARRIER)) {
        b->actual_frequency = furi_hal_subghz_set_frequency_and_path(BENCH_CARRIER);
        return true;
    }
    const FuriHalRegion* original = furi_hal_region_get();
    if(!original || strcmp(original->country_code, "US") || original->bands_count > 16 ||
       BENCH_CARRIER < 902100000U || BENCH_CARRIER > 927900000U)
        return false;
    size_t size = sizeof(FuriHalRegion) + original->bands_count * sizeof(FuriHalRegionBand);
    FuriHalRegion* saved = malloc(size);
    FuriHalRegion* temporary = malloc(size + sizeof(FuriHalRegionBand));
    if(!saved || !temporary) {
        free(saved);
        free(temporary);
        return false;
    }
    memcpy(saved, original, size);
    memcpy(temporary, original, size);
    temporary->bands[temporary->bands_count++] = (FuriHalRegionBand){
        .start = BENCH_CARRIER,
        .end = BENCH_CARRIER,
        .power_limit = power_dbm[b->power],
        .duty_cycle = 50};
    furi_hal_region_set(temporary);
    b->actual_frequency = furi_hal_subghz_set_frequency_and_path(BENCH_CARRIER);
    furi_hal_region_set(saved);
    return true;
}
static void finish_radio(void) {
    /* Re-evaluate the original region to clear the HAL's cached TX permission. */
    furi_hal_subghz_set_frequency(BENCH_CARRIER);
    furi_hal_subghz_sleep();
}

static LevelDuration next_pulse(void* context) {
    Bench* b = context;
    uint32_t i = b->position++;
    if(i < b->active->count) return level_duration_make(!(i & 1), b->active->data[i]);
    if(i == b->active->count) return level_duration_make(false, 2000);
    /* Saturate to avoid overflow if the middleware asks repeatedly. */
    b->position = b->active->count + 2;
    return level_duration_reset();
}
static void draw(Canvas* c, void* context) {
    Bench* b = context;
    furi_mutex_acquire(b->mutex, FuriWaitForever);
    canvas_clear(c);
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 0, 10, "LoRa TX Bench");
    canvas_set_font(c, FontSecondary);
    canvas_draw_str(c, 0, 22, waves[b->selected].name);
    canvas_draw_str(c, 0, 32, "Target: 910.525 MHz");
    char line[32];
    snprintf(
        line,
        sizeof(line),
        "%s #%lu  %ddBm",
        b->flood ? "Flood" : "Direct",
        (unsigned long)b->next_sequence,
        power_dbm[b->power]);
    canvas_draw_str(c, 0, 42, line);
    canvas_draw_str(c, 0, 52, b->status);
    canvas_draw_str(c, 0, 63, b->sending ? "Back: stop" : "<> test  ^v power  OK TX");
    furi_mutex_release(b->mutex);
}
static void input(InputEvent* e, void* context) {
    Bench* b = context;
    furi_message_queue_put(b->input, e, 0);
}
static void log_result(Bench* b, const char* outcome, uint32_t elapsed) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, EXT_PATH("apps_data/lora_tx_bench"));
    File* f = storage_file_alloc(storage);
    if(storage_file_open(
           f, EXT_PATH("apps_data/lora_tx_bench/bench.txt"), FSAM_WRITE, FSOM_OPEN_APPEND)) {
        char line[256];
        int n = snprintf(
            line,
            sizeof(line),
            "%s: %s; test=%lu; timestamp=%lu; route=%s; carrier=%lu; target=910525000; waveform_us=%lu; elapsed_ticks=%lu; callback_runs=%lu/%lu; PA=0x%02X (%ddBm); RF reception NOT verified\n",
            b->active->name,
            outcome,
            (unsigned long)b->sequence,
            (unsigned long)b->timestamp,
            b->flood ? "flood" : "direct",
            (unsigned long)b->actual_frequency,
            (unsigned long)b->active->duration_us,
            (unsigned long)elapsed,
            (unsigned long)b->position,
            (unsigned long)b->active->count,
            power_pa[b->power],
            power_dbm[b->power]);
        if(n > 0)
            storage_file_write(f, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
    }
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
}
static void log_region(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, EXT_PATH("apps_data/lora_tx_bench"));
    File* f = storage_file_alloc(storage);
    if(storage_file_open(
           f, EXT_PATH("apps_data/lora_tx_bench/region.txt"), FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        const FuriHalRegion* r = furi_hal_region_get();
        char line[128];
        int n = snprintf(
            line,
            sizeof(line),
            "Region: %s; provisioned=%u; carrier_allowed=%u; target_allowed=%u\n",
            furi_hal_region_get_name(),
            furi_hal_region_is_provisioned(),
            furi_hal_region_is_frequency_allowed(BENCH_CARRIER),
            furi_hal_region_is_frequency_allowed(910525000));
        storage_file_write(f, line, n);
        if(r)
            for(unsigned i = 0; i < r->bands_count; ++i) {
                n = snprintf(
                    line,
                    sizeof(line),
                    "%lu-%lu Hz; power_limit=%d; duty_cycle=%u\n",
                    (unsigned long)r->bands[i].start,
                    (unsigned long)r->bands[i].end,
                    r->bands[i].power_limit,
                    r->bands[i].duty_cycle);
                storage_file_write(f, line, n);
            }
    }
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
}
int32_t lora_tx_bench_app(void* context) {
    UNUSED(context);
    log_region();
    Bench* b = calloc(1, sizeof(Bench));
    b->next_sequence = 2;
    uint32_t saved_sequence;
    if(saved_struct_load(
           EXT_PATH("apps_data/lora_tx_bench/sequence.bin"),
           &saved_sequence,
           sizeof(saved_sequence),
           0xB7,
           1) &&
       saved_sequence >= 2)
        b->next_sequence = saved_sequence;
    int32_t offset;
    if(saved_struct_load(
           EXT_PATH("apps_data/lora_tx_bench/utc_offset.bin"), &offset, sizeof(offset), 0xB8, 1) &&
       offset >= -14 * 3600 && offset <= 14 * 3600)
        b->utc_offset = offset;
    b->dynamic_data = malloc(TX_WAVE_CAPACITY);
    b->power = 2; /* -10dBm, 100x the initial -30dBm bench setting. */
    b->selected = 2; /* MC packet CR5 */
    b->input = furi_message_queue_alloc(8, sizeof(InputEvent));
    b->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    snprintf(b->status, sizeof(b->status), "Experimental; idle");
    ViewPort* v = view_port_alloc();
    view_port_draw_callback_set(v, draw, b);
    view_port_input_callback_set(v, input, b);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, v, GuiLayerFullscreen);
    /* Startup is receive/transmit-idle; only OK starts a bounded burst. */
    bool running = true;
    uint32_t started = 0, next_allowed = 0;
    while(running) {
        InputEvent e;
        bool event = furi_message_queue_get(b->input, &e, 20) == FuriStatusOk;
        bool abort = event && e.key == InputKeyBack && e.type == InputTypeShort;
        if(b->sending) {
            bool done = furi_hal_subghz_is_async_tx_complete();
            bool timeout = furi_get_tick() - started > furi_ms_to_ticks(1500);
            if(done || abort || timeout) {
                furi_hal_subghz_stop_async_tx();
                finish_radio();
                furi_mutex_acquire(b->mutex, FuriWaitForever);
                b->sending = false;
                snprintf(
                    b->status,
                    sizeof(b->status),
                    "%s",
                    done  ? "Burst done; check receiver" :
                    abort ? "Stopped" :
                            "TX timeout");
                furi_mutex_release(b->mutex);
                log_result(
                    b,
                    done  ? "complete" :
                    abort ? "stopped" :
                            "timeout",
                    furi_get_tick() - started);
                next_allowed = furi_get_tick() + furi_ms_to_ticks(1000);
            }
        } else if(abort)
            running = false;
        else if(event && e.type == InputTypeLong && e.key == InputKeyUp) {
            furi_mutex_acquire(b->mutex, FuriWaitForever);
            b->flood = !b->flood;
            snprintf(b->status, sizeof(b->status), "Hold Up: Direct / Flood");
            furi_mutex_release(b->mutex);
        } else if(event && e.type == InputTypeShort) {
            furi_mutex_acquire(b->mutex, FuriWaitForever);
            if(e.key == InputKeyRight) b->selected = (b->selected + 1) % COUNT_OF(waves);
            if(e.key == InputKeyLeft)
                b->selected = (b->selected + COUNT_OF(waves) - 1) % COUNT_OF(waves);
            if(e.key == InputKeyUp && b->power + 1 < COUNT_OF(power_dbm)) ++b->power;
            if(e.key == InputKeyDown && b->power) --b->power;
            furi_mutex_release(b->mutex);
            if(e.key == InputKeyOk && (int32_t)(furi_get_tick() - next_allowed) >= 0) {
                b->active = &waves[b->selected];
                b->sequence = 0;
                b->timestamp = 0;
                if(b->selected >= 2) {
                    furi_mutex_acquire(b->mutex, FuriWaitForever);
                    snprintf(b->status, sizeof(b->status), "Preparing fresh packet...");
                    b->sequence = b->next_sequence;
                    b->timestamp =
                        (uint32_t)((int64_t)furi_hal_rtc_get_timestamp() + b->utc_offset);
                    furi_mutex_release(b->mutex);
                    view_port_update(v);
                    uint8_t packet[64];
                    size_t length = bench_packet(packet, b->sequence, b->timestamp, b->flood);
                    b->dynamic_wave =
                        (BenchWave){.name = waves[b->selected].name, .data = b->dynamic_data};
                    if(b->dynamic_data && length)
                        b->dynamic_wave.count = bench_waveform(
                            packet,
                            length,
                            b->selected == 2 ? 1 : 4,
                            b->dynamic_data,
                            TX_WAVE_CAPACITY,
                            &b->dynamic_wave.duration_us);
                    uint32_t next = b->sequence + 1;
                    bool ready = b->dynamic_wave.count && next &&
                                 saved_struct_save(
                                     EXT_PATH("apps_data/lora_tx_bench/sequence.bin"),
                                     &next,
                                     sizeof(next),
                                     0xB7,
                                     1);
                    furi_mutex_acquire(b->mutex, FuriWaitForever);
                    if(ready)
                        b->next_sequence = next;
                    else
                        snprintf(b->status, sizeof(b->status), "Prepare/save failed; no TX");
                    furi_mutex_release(b->mutex);
                    if(!ready) {
                        view_port_update(v);
                        continue;
                    }
                    b->active = &b->dynamic_wave;
                }
                b->position = 0;
                furi_hal_subghz_load_custom_preset(preset);
                uint8_t pa[8] = {0};
                pa[1] = power_pa[b->power];
                furi_hal_subghz_load_patable(pa);
                bool prepared = prepare_frequency(b);
                started = furi_get_tick();
                bool ok = prepared && furi_hal_subghz_start_async_tx(next_pulse, b);
                furi_mutex_acquire(b->mutex, FuriWaitForever);
                b->sending = ok;
                snprintf(
                    b->status,
                    sizeof(b->status),
                    "%s",
                    ok ? "Sending one burst..." : "Region blocks TX");
                furi_mutex_release(b->mutex);
                if(!ok) {
                    finish_radio();
                    log_result(b, "region blocked", 0);
                }
            }
        }
        view_port_update(v);
    }
    view_port_enabled_set(v, false);
    gui_remove_view_port(gui, v);
    view_port_free(v);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(b->input);
    furi_mutex_free(b->mutex);
    free(b->dynamic_data);
    free(b);
    return 0;
}
