#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include <subghz/devices/devices.h>
#include <subghz/devices/cc1101_int/cc1101_int_interconnect.h>
#include <stdio.h>
#include <string.h>
#include "detector.h"
#include "payload.h"
#include "profiles.h"
#include "report.h"

#define HISTORY       8
#define STREAM_BYTES  8192
#define CAPTURE_WORDS 8192
#define PRE_WORDS     256

typedef struct {
    LoraDetection event;
    uint32_t frequency, count, tick, generation;
    int rssi;
    uint8_t payload_state, payload_length, copies;
    int8_t packet_type;
    char name[33];
} Heard;
typedef struct {
    Heard heard[HISTORY];
    unsigned count, scroll, requested, active;
    bool survey, logging, log_error, radio_error, stop, details, decode_available;
    uint32_t drops, rate, headroom, decode_started, decode_skipped;
    unsigned decode_progress;
    bool decoding;
} Model;
typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* input;
    FuriStreamBuffer* stream;
    FuriThread* worker;
    FuriThread* decode_worker;
    FuriMessageQueue* decode_jobs;
    ViewPort* viewport;
    NotificationApp* notifications;
    Storage* storage;
    const SubGhzDevice* radio;
    LoraDetector detector;
    LoraPayload* payload;
    Model model;
    unsigned profile;
    uint32_t generation;
    struct {
        uint32_t pre[PRE_WORDS], data[CAPTURE_WORDS];
        unsigned pre_write, pre_count, count;
        uint64_t time_us, base_us, end_us, until_us;
        uint32_t frequency, generation, deadline;
        struct {
            uint64_t time_us;
            int rssi;
            uint32_t timestamp;
        } signals[8];
        unsigned signal_count;
        LoraPayloadResult best[8];
        unsigned best_count;
        bool capturing, busy;
    } capture;
    /* ISR writes; worker snapshots only with interrupts masked. Once loss is
     * latched, discard until RX is stopped and the whole stream is reset. */
    volatile bool overrun;
    volatile uint32_t dropped, pulses;
} App;

static uint32_t ticks_to_ms(uint32_t ticks) {
    return (uint32_t)((uint64_t)ticks * 1000 / furi_kernel_get_tick_frequency());
}

static void rx_callback(bool level, uint32_t duration, void* context) {
    App* a = context;
    ++a->pulses;
    uint32_t word = (level ? 0x80000000u : 0) | (duration & 0x7fffffffu);
    if(a->overrun || duration > 0x7fffffffu ||
       furi_stream_buffer_send(a->stream, &word, sizeof(word), 0) != sizeof(word)) {
        a->overrun = true;
        ++a->dropped;
    }
}

static void log_event(App* a, const Heard* h) {
    bool ok = storage_simply_mkdir(a->storage, EXT_PATH("apps_data/lora_detect"));
    File* file = storage_file_alloc(a->storage);
    if(ok)
        ok =
            storage_file_open(file, EXT_PATH("apps_data/lora_detect/detections.csv"), FSAM_WRITE, FSOM_OPEN_APPEND);
    if(ok && storage_file_size(file) == 0) {
        const char* header =
            "uptime_ms,frequency_hz,bandwidth_hz,sf,sync,preamble,period_us,rssi_dbm,label\n";
        ok = storage_file_write(file, header, strlen(header)) == strlen(header);
    }
    if(ok) {
        char line[180];
        int n = snprintf(
            line,
            sizeof(line),
            "%lu,%lu,%lu,%u,%d,%lu,%.3f,%d,%s\n",
            (unsigned long)ticks_to_ms(h->tick),
            (unsigned long)h->frequency,
            (unsigned long)h->event.bandwidth,
            h->event.sf,
            h->event.sync,
            (unsigned long)h->event.preamble_symbols,
            (double)h->event.period_us,
            h->rssi,
            lora_detection_label(&h->event));
        ok = n > 0 && n < (int)sizeof(line) && storage_file_write(file, line, n) == (size_t)n;
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_mutex_acquire(a->mutex, FuriWaitForever);
    a->model.log_error = !ok;
    furi_mutex_release(a->mutex);
}

static const char* traffic_type(int type) {
    static const char* names[] = {
        "Request",
        "Response",
        "Direct message",
        "ACK",
        "Advertisement",
        "Channel message",
        "Channel data",
        "Anon request",
        "Path",
        "Trace",
        "Multipart",
        "Control"};
    return type >= 0 && type < 12 ? names[type] : "Unknown traffic";
}

/* Plain UTF-8 text for the stock iPhone app's file viewer. */
static bool
    log_phone_report(App* a, const LoraPayloadResult* result, int rssi, uint32_t timestamp) {
    File* file = storage_file_alloc(a->storage);
    bool ok =
        storage_simply_mkdir(a->storage, EXT_PATH("apps_data/lora_detect")) &&
        storage_file_open(file, EXT_PATH("apps_data/lora_detect/LoRa Logs.txt"), FSAM_WRITE, FSOM_OPEN_APPEND);
    if(ok && storage_file_size(file) == 0) {
        const char* header = "LoRa Detect - traffic log\n\n"
                             "Hold OK in LoRa Detect to enable REC.\n"
                             "Finished decode results appear below, oldest first.\n"
                             "Reopen this file on your phone to refresh.\n"
                             "Dates use the Flipper's clock at detection.\n"
                             "Names/types marked UNVERIFIED are tentative.\n"
                             "No node signatures are verified.\n\n";
        ok = storage_file_write(file, header, strlen(header)) == strlen(header);
    }
    if(ok && result) {
        char when[40] = "Detection time unavailable", entry[512];
        if(timestamp) {
            DateTime dt;
            datetime_timestamp_to_datetime(timestamp, &dt);
            snprintf(
                when,
                sizeof(when),
                "%04u-%02u-%02u %02u:%02u:%02u",
                dt.year,
                dt.month,
                dt.day,
                dt.hour,
                dt.minute,
                dt.second);
        }
        size_t n = lora_report_format(
            entry,
            sizeof(entry),
            result,
            when,
            a->capture.frequency,
            rssi,
            traffic_type(result->packet_type));
        ok = n && storage_file_write(file, entry, n) == n;
    }
    storage_file_close(file);
    storage_file_free(file);
    return ok;
}

static void log_payload(App* a, const LoraPayloadResult* r, int rssi, uint32_t timestamp) {
    File* file = storage_file_alloc(a->storage);
    bool ok = storage_simply_mkdir(a->storage, EXT_PATH("apps_data/lora_detect")) &&
              storage_file_open(file, EXT_PATH("apps_data/lora_detect/traffic.csv"), FSAM_WRITE, FSOM_OPEN_APPEND);
    if(ok && storage_file_size(file) == 0) {
        const char* header =
            "decoded_uptime_ms,frequency_hz,rssi_dbm,length,status,type,name,combined_copies\n";
        ok = storage_file_write(file, header, strlen(header)) == strlen(header);
    }
    if(ok) {
        char name[67], line[256], signal[16] = "";
        if(rssi != 1000) snprintf(signal, sizeof(signal), "%d", rssi);
        unsigned j = 0;
        for(unsigned i = 0; r->name[i] && i < sizeof(r->name) - 1; ++i) {
            if(r->name[i] == '"') name[j++] = '"';
            name[j++] = r->name[i];
        }
        name[j] = 0;
        int n = snprintf(
            line,
            sizeof(line),
            "%lu,%lu,%s,%u,%s,%s,\"%s\",%u\n",
            (unsigned long)ticks_to_ms(furi_get_tick()),
            (unsigned long)a->capture.frequency,
            signal,
            r->length,
            r->status == LoraPayloadCrcOk   ? "crc_ok_signature_unchecked" :
            r->status == LoraPayloadDamaged ? "unverified" :
                                              "header_failed",
            traffic_type(r->packet_type),
            name,
            r->combined_copies);
        ok = n > 0 && n < (int)sizeof(line) && storage_file_write(file, line, n) == (size_t)n;
    }
    storage_file_close(file);
    storage_file_free(file);
    bool report_ok = log_phone_report(a, r, rssi, timestamp);
    ok = ok && report_ok;
    furi_mutex_acquire(a->mutex, FuriWaitForever);
    a->model.log_error = !ok;
    furi_mutex_release(a->mutex);
}

static void cancel_pending(App* a) {
    furi_mutex_acquire(a->mutex, FuriWaitForever);
    for(unsigned i = 0; i < a->model.count; ++i)
        if(a->model.heard[i].payload_state == 6) a->model.heard[i].payload_state = 5;
    furi_mutex_release(a->mutex);
}

static void payload_decoded(const LoraPayloadResult* result, void* context) {
    App* a = context;
    unsigned best = 0;
    for(; best < a->capture.best_count; ++best)
        if(a->capture.best[best].detection_time == result->detection_time) break;
    if(best == a->capture.best_count) {
        if(best == COUNT_OF(a->capture.best)) return;
        ++a->capture.best_count;
    } else if(!lora_payload_better(result, &a->capture.best[best]))
        return;
    a->capture.best[best] = *result;
    furi_mutex_acquire(a->mutex, FuriWaitForever);
    for(unsigned i = 0; i < a->model.count; ++i) {
        Heard* h = &a->model.heard[i];
        if(h->event.time_us == result->detection_time && h->event.sf == 7 &&
           h->generation == a->capture.generation && h->frequency == a->capture.frequency) {
            h->payload_state = result->status == LoraPayloadCrcOk   ? 4 :
                               result->status == LoraPayloadDamaged ? 3 :
                                                                      2;
            h->payload_length = result->length;
            h->packet_type = result->packet_type;
            h->copies = result->combined_copies;
            memcpy(h->name, result->name, sizeof(h->name));
            break;
        }
    }
    furi_mutex_release(a->mutex);
}

static uint8_t capture_start(App* a);

static void detected(const LoraDetection* event, void* context) {
    App* a = context;
    Heard h = {
        .event = *event,
        .frequency = lora_profiles[a->profile].frequency,
        .count = 1,
        .tick = furi_get_tick(),
        .rssi = (int)subghz_devices_get_rssi(a->radio)};
    h.packet_type = -1;
    h.generation = a->generation;
    if(a->payload && event->sf == 7 && event->bandwidth == 62500 && event->sync == 0x12)
        h.payload_state = capture_start(a);
    if(h.payload_state == 6 && a->capture.signal_count < COUNT_OF(a->capture.signals)) {
        a->capture.signals[a->capture.signal_count].time_us = event->time_us;
        a->capture.signals[a->capture.signal_count].timestamp = furi_hal_rtc_get_timestamp();
        a->capture.signals[a->capture.signal_count++].rssi = h.rssi;
    }
    furi_mutex_acquire(a->mutex, FuriWaitForever);
    Model* m = &a->model;
    /* Busy packets were not captured: do not let them evict the in-flight
     * packet or its results. They are still counted and logged below. */
    if(h.payload_state == 7) {
        ++m->decode_skipped;
        bool logging = m->logging;
        furi_mutex_release(a->mutex);
        if(logging) log_event(a, &h);
        return;
    }
    unsigned index = 0;
    for(; index < m->count; ++index) {
        Heard* old = &m->heard[index];
        if(!h.payload_state && old->frequency == h.frequency && old->event.sf == event->sf &&
           old->event.bandwidth == event->bandwidth && old->event.sync == event->sync) {
            h.count = old->count + 1;
            break;
        }
    }
    if(index == m->count && m->count < HISTORY) ++m->count;
    if(index >= HISTORY) index = HISTORY - 1;
    memmove(&m->heard[1], &m->heard[0], index * sizeof(Heard));
    m->heard[0] = h;
    bool logging = m->logging;
    furi_mutex_release(a->mutex);
    notification_message(a->notifications, &sequence_blink_green_10);
    if(logging) log_event(a, &h);
}

/* Radio worker records bounded snippets; it never performs payload decoding. */
static uint8_t capture_start(App* a) {
    furi_mutex_acquire(a->mutex, FuriWaitForever);
    bool busy = a->capture.busy;
    if(!busy) {
        a->capture.busy = true;
        a->model.decode_progress = 0;
    }
    furi_mutex_release(a->mutex);
    if(busy) return a->capture.capturing ? 6 : 7;
    a->capture.count = a->capture.pre_count;
    uint64_t span = 0;
    for(unsigned j = 0; j < a->capture.pre_count; ++j) {
        uint32_t word =
            a->capture
                .pre[(a->capture.pre_write + PRE_WORDS - a->capture.pre_count + j) % PRE_WORDS];
        a->capture.data[j] = word;
        span += word & 0x7fffffffu;
    }
    a->capture.base_us = a->capture.time_us - span;
    a->capture.frequency = lora_profiles[a->profile].frequency;
    a->capture.generation = a->generation;
    a->capture.until_us = a->capture.time_us + 2500000;
    a->capture.deadline = furi_get_tick() + furi_ms_to_ticks(2500);
    a->capture.signal_count = 0;
    a->capture.capturing = true;
    return 6;
}

static void capture_finish(App* a) {
    a->capture.capturing = false;
    a->capture.end_us = a->capture.time_us;
    furi_mutex_acquire(a->mutex, FuriWaitForever);
    a->model.decoding = true;
    a->model.decode_progress = 0;
    a->model.decode_started = furi_get_tick();
    a->model.decode_skipped = 0;
    for(unsigned i = 0; i < a->model.count; ++i)
        if(a->model.heard[i].payload_state == 6) a->model.heard[i].payload_state = 1;
    furi_mutex_release(a->mutex);
    uint8_t job = 1;
    furi_check(furi_message_queue_put(a->decode_jobs, &job, 0) == FuriStatusOk);
}

static void capture_feed(App* a, uint32_t word) {
    a->capture.time_us += word & 0x7fffffffu;
    a->capture.pre[a->capture.pre_write] = word;
    a->capture.pre_write = (a->capture.pre_write + 1) % PRE_WORDS;
    if(a->capture.pre_count < PRE_WORDS) ++a->capture.pre_count;
    if(a->capture.capturing) {
        a->capture.data[a->capture.count++] = word;
        if(a->capture.count == CAPTURE_WORDS || a->capture.time_us >= a->capture.until_us)
            capture_finish(a);
    }
}

static void replay_detected(const LoraDetection* e, void* context) {
    App* a = context;
    lora_payload_start(a->payload, e);
}

static bool decode_cancelled(void* context) {
    /* Let the GUI, USB and RX threads run between expensive symbols. */
    furi_delay_tick(1);
    App* a = context;
    furi_mutex_acquire(a->mutex, FuriWaitForever);
    bool stop = a->model.stop;
    furi_mutex_release(a->mutex);
    return stop;
}

static int32_t decode_worker(void* context) {
    App* a = context;
    for(;;) {
        furi_mutex_acquire(a->mutex, FuriWaitForever);
        bool stop = a->model.stop;
        furi_mutex_release(a->mutex);
        if(stop) break;
        uint8_t job;
        if(furi_message_queue_get(a->decode_jobs, &job, furi_ms_to_ticks(100)) != FuriStatusOk)
            continue;
        a->capture.best_count = 0;
        static const float phases[] = {0, -8, -4, 4, 8};
        for(unsigned phase = 0; phase < COUNT_OF(phases); ++phase) {
            lora_payload_init(a->payload, payload_decoded, a);
            a->payload->phase_bias = phases[phase];
            a->payload->cancelled = decode_cancelled;
            a->payload->time_us = a->capture.base_us;
            LoraDetector detector;
            lora_detector_init(&detector, 62500, replay_detected, a);
            detector.time_us = a->capture.base_us;
            for(unsigned i = 0; i < a->capture.count; ++i) {
                if((i & 31) == 0) {
                    furi_mutex_acquire(a->mutex, FuriWaitForever);
                    stop = a->model.stop;
                    a->model.decode_progress = (phase * a->capture.count + i) * 100 /
                                               (COUNT_OF(phases) * a->capture.count);
                    furi_mutex_release(a->mutex);
                    if(stop) break;
                    furi_delay_tick(1);
                }
                uint32_t word = a->capture.data[i];
                lora_payload_feed(a->payload, word >> 31, word & 0x7fffffffu);
                lora_detector_feed(&detector, word >> 31, word & 0x7fffffffu);
            }
            if(stop) break;
        }
        furi_mutex_acquire(a->mutex, FuriWaitForever);
        bool logging = a->model.logging && !a->model.stop;
        furi_mutex_release(a->mutex);
        if(logging)
            for(unsigned i = 0; i < a->capture.best_count; ++i) {
                const LoraPayloadResult* result = &a->capture.best[i];
                int rssi = 1000;
                uint32_t timestamp = 0;
                for(unsigned j = 0; j < a->capture.signal_count; ++j)
                    if(a->capture.signals[j].time_us == result->detection_time) {
                        rssi = a->capture.signals[j].rssi;
                        timestamp = a->capture.signals[j].timestamp;
                    }
                log_payload(a, result, rssi, timestamp);
            }
        furi_mutex_acquire(a->mutex, FuriWaitForever);
        for(unsigned i = 0; i < a->model.count; ++i) {
            Heard* h = &a->model.heard[i];
            if(h->generation == a->capture.generation && h->payload_state == 1)
                h->payload_state = 8;
        }
        a->model.decoding = false;
        a->model.decode_progress = 100;
        a->capture.busy = false;
        furi_mutex_release(a->mutex);
    }
    return 0;
}

/* Called only with RX stopped (also true at initial setup). */
static void tune(App* a) {
    const LoraProfile* p = &lora_profiles[a->profile];
    subghz_devices_idle(a->radio);
    furi_stream_buffer_reset(a->stream);
    a->overrun = false;
    lora_detector_init(&a->detector, p->bandwidth, detected, a);
    if(a->capture.capturing) {
        a->capture.capturing = false;
        cancel_pending(a);
        furi_mutex_acquire(a->mutex, FuriWaitForever);
        a->capture.busy = false;
        furi_mutex_release(a->mutex);
    }
    a->capture.pre_count = a->capture.pre_write = 0;
    a->capture.time_us = 0;
    ++a->generation;
    subghz_devices_load_preset(a->radio, FuriHalSubGhzPresetCustom, lora_presets[p->preset]);
    subghz_devices_set_frequency(a->radio, p->frequency);
    subghz_devices_flush_rx(a->radio);
    subghz_devices_start_async_rx(a->radio, rx_callback, a);
    furi_mutex_acquire(a->mutex, FuriWaitForever);
    a->model.active = a->profile;
    furi_mutex_release(a->mutex);
}

static int32_t worker(void* context) {
    App* a = context;
    subghz_devices_init();
    a->radio = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);
    if(!a->radio) {
        furi_mutex_acquire(a->mutex, FuriWaitForever);
        a->model.radio_error = true;
        furi_mutex_release(a->mutex);
        subghz_devices_deinit();
        return 0;
    }
    subghz_devices_reset(a->radio);
    tune(a);
    uint32_t hop = furi_get_tick(), stats = hop, previous_pulses = 0;
    uint32_t min_free = STREAM_BYTES;
    bool was_survey = false;
    for(;;) {
        furi_mutex_acquire(a->mutex, FuriWaitForever);
        bool stop = a->model.stop, survey = a->model.survey;
        unsigned requested = a->model.requested;
        furi_mutex_release(a->mutex);
        if(stop) break;
        uint32_t now = furi_get_tick();
        if(a->capture.capturing && (int32_t)(now - a->capture.deadline) >= 0) capture_finish(a);
        if(survey && !was_survey) hop = now;
        was_survey = survey;
        if(requested != a->profile || (survey && now - hop >= furi_ms_to_ticks(10000))) {
            if(requested == a->profile) {
                requested = (a->profile + 1) % LORA_PROFILE_COUNT;
                furi_mutex_acquire(a->mutex, FuriWaitForever);
                /* Do not overwrite a manual selection arriving during this hop. */
                if(a->model.requested == a->profile) a->model.requested = requested;
                furi_mutex_release(a->mutex);
            }
            subghz_devices_stop_async_rx(a->radio);
            a->profile = requested;
            tune(a);
            hop = now;
        }
        uint32_t words[64];
        size_t free_bytes = furi_stream_buffer_spaces_available(a->stream);
        if(free_bytes < min_free) min_free = free_bytes;
        size_t bytes =
            furi_stream_buffer_receive(a->stream, words, sizeof(words), furi_ms_to_ticks(20));
        FURI_CRITICAL_ENTER();
        bool lost = a->overrun;
        uint32_t drops = a->dropped, pulses = a->pulses;
        FURI_CRITICAL_EXIT();
        if(lost) {
            subghz_devices_stop_async_rx(a->radio);
            tune(a);
            min_free = 0;
        } else {
            for(size_t i = 0; i < bytes / sizeof(uint32_t); ++i) {
                if(a->payload && lora_profiles[a->profile].bandwidth == 62500)
                    capture_feed(a, words[i]);
                lora_detector_feed(&a->detector, words[i] >> 31, words[i] & 0x7fffffffu);
            }
        }
        if(now - stats >= furi_ms_to_ticks(1000)) {
            furi_mutex_acquire(a->mutex, FuriWaitForever);
            a->model.rate =
                (uint32_t)((uint64_t)(pulses - previous_pulses) * 1000 / ticks_to_ms(now - stats));
            a->model.drops = drops;
            a->model.headroom = min_free;
            furi_mutex_release(a->mutex);
            stats = now;
            previous_pulses = pulses;
            min_free = STREAM_BYTES;
        }
    }
    subghz_devices_stop_async_rx(a->radio);
    subghz_devices_idle(a->radio);
    subghz_devices_sleep(a->radio);
    subghz_devices_deinit();
    return 0;
}

static void draw(Canvas* canvas, void* context) {
    App* a = context;
    Model m;
    furi_mutex_acquire(a->mutex, FuriWaitForever);
    m = a->model;
    furi_mutex_release(a->mutex);
    const LoraProfile* p = &lora_profiles[m.active];
    char text[80];
    canvas_set_font(canvas, FontSecondary);
    snprintf(
        text,
        sizeof(text),
        "%lu.%04lu %s%s",
        (unsigned long)(p->frequency / 1000000),
        (unsigned long)((p->frequency % 1000000) / 100),
        m.survey ? "SCAN" : "WATCH",
        m.log_error ? " ERR" : (m.logging ? " REC" : ""));
    canvas_draw_str(canvas, 0, 8, text);
    canvas_draw_str(canvas, 0, 18, p->name);
    canvas_draw_line(canvas, 0, 20, 127, 20);
    if(m.radio_error)
        canvas_draw_str(canvas, 0, 34, "Radio unavailable");
    else if(!m.count) {
        canvas_draw_str(canvas, 0, 32, "Listening for LoRa...");
        snprintf(
            text,
            sizeof(text),
            "%lu edges/s free %lu",
            (unsigned long)m.rate,
            (unsigned long)m.headroom);
        canvas_draw_str(canvas, 0, 43, text);
        snprintf(text, sizeof(text), "Lost pulses: %lu", (unsigned long)m.drops);
        canvas_draw_str(canvas, 0, 54, text);
    } else {
        const Heard* h = &m.heard[m.scroll < m.count ? m.scroll : 0];
        if(m.details && (h->payload_state || h->event.sync == 0x12)) {
            const char* status = h->payload_state == 4 ? "CRC OK / sig unchecked" :
                                 h->payload_state == 3 ? "UNVERIFIED / CRC bad" :
                                 h->payload_state == 2 ? "Header not recovered" :
                                 h->payload_state == 5 ? "Decode interrupted" :
                                 h->payload_state == 1 ? "Decoding..." :
                                 h->payload_state == 6 ? "Capturing..." :
                                 h->payload_state == 7 ? "Skipped while busy" :
                                 h->payload_state == 8 ? "No payload recovered" :
                                 m.decode_available    ? "Decode: MC US only" :
                                                         "Decode: low memory";
            snprintf(
                text,
                sizeof(text),
                "%s%s",
                h->packet_type >= 0 ? traffic_type(h->packet_type) : status,
                h->packet_type >= 0 && h->payload_state != 4 ? " ?" : "");
            canvas_draw_str(canvas, 0, 30, text);
            snprintf(
                text,
                sizeof(text),
                "%.22s",
                h->name[0]          ? h->name :
                h->packet_type >= 0 ? status :
                                      "");
            canvas_draw_str(canvas, 0, 41, text);
            if(h->payload_state == 4)
                snprintf(text, sizeof(text), "%ddBm %uB CRC OK", h->rssi, h->payload_length);
            else
                snprintf(text, sizeof(text), "UNVERIFIED %ddBm", h->rssi);
            canvas_draw_str(canvas, 0, 52, text);
            if(m.decoding) {
                snprintf(
                    text,
                    sizeof(text),
                    "Decode %u%% %lus",
                    m.decode_progress,
                    (unsigned long)(ticks_to_ms(furi_get_tick() - m.decode_started) / 1000));
            } else if(m.decode_progress == 100) {
                snprintf(
                    text, sizeof(text), "Done / %lu skipped", (unsigned long)m.decode_skipped);
            } else {
                snprintf(text, sizeof(text), "Hold UP: RF  ^v: history");
            }
            canvas_draw_str(canvas, 0, 64, text);
            return;
        }
        snprintf(
            text,
            sizeof(text),
            "%s x%lu",
            lora_detection_label(&h->event),
            (unsigned long)h->count);
        canvas_draw_str(canvas, 0, 31, text);
        snprintf(
            text,
            sizeof(text),
            "SF%u %lu.%luk %ddBm",
            h->event.sf,
            (unsigned long)(h->event.bandwidth / 1000),
            (unsigned long)((h->event.bandwidth % 1000) / 100),
            h->rssi);
        canvas_draw_str(canvas, 0, 42, text);
        snprintf(
            text,
            sizeof(text),
            "%lu.%03lu %lus ago%s",
            (unsigned long)(h->frequency / 1000000),
            (unsigned long)((h->frequency % 1000000) / 1000),
            (unsigned long)(ticks_to_ms(furi_get_tick() - h->tick) / 1000),
            m.drops ? " !loss" : "");
        canvas_draw_str(canvas, 0, 53, text);
    }
    canvas_draw_str(canvas, 0, 64, "<> tune OK scan ^v history");
}

static void input(InputEvent* event, void* context) {
    App* a = context;
    furi_message_queue_put(a->input, event, 0);
}

int32_t lora_detect_app(void* context) {
    UNUSED(context);
    App* a = malloc(sizeof(App));
    memset(a, 0, sizeof(*a));
    a->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    a->input = furi_message_queue_alloc(8, sizeof(InputEvent));
    a->stream = furi_stream_buffer_alloc(STREAM_BYTES, sizeof(uint32_t));
    a->notifications = furi_record_open(RECORD_NOTIFICATION);
    a->storage = furi_record_open(RECORD_STORAGE);
    a->model.log_error = !log_phone_report(a, NULL, 1000, 0);
    /* Leave headroom for GUI, storage and radio service allocations. */
    if(memmgr_get_free_heap() > sizeof(LoraPayload) + 24000) {
        a->payload = malloc(sizeof(LoraPayload));
        a->model.decode_available = true;
    }
    a->model.details = true;
    if(a->payload) {
        a->decode_jobs = furi_message_queue_alloc(1, sizeof(uint8_t));
        a->decode_worker = furi_thread_alloc_ex("LoRaDecode", 4096, decode_worker, a);
        /* Time-share with RX so continuous input cannot starve decoding. */
        furi_thread_set_priority(a->decode_worker, FuriThreadPriorityNormal);
        furi_thread_start(a->decode_worker);
    }
    a->viewport = view_port_alloc();
    view_port_draw_callback_set(a->viewport, draw, a);
    view_port_input_callback_set(a->viewport, input, a);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, a->viewport, GuiLayerFullscreen);
    a->worker = furi_thread_alloc_ex("LoRaRx", 4096, worker, a);
    furi_thread_start(a->worker);
    bool running = true;
    while(running) {
        InputEvent event;
        if(furi_message_queue_get(a->input, &event, furi_ms_to_ticks(100)) == FuriStatusOk) {
            furi_mutex_acquire(a->mutex, FuriWaitForever);
            Model* m = &a->model;
            if(event.type == InputTypeShort || event.type == InputTypeRepeat) {
                switch(event.key) {
                case InputKeyBack:
                    running = false;
                    break;
                case InputKeyLeft:
                    m->requested = (m->requested + LORA_PROFILE_COUNT - 1) % LORA_PROFILE_COUNT;
                    m->survey = false;
                    break;
                case InputKeyRight:
                    m->requested = (m->requested + 1) % LORA_PROFILE_COUNT;
                    m->survey = false;
                    break;
                case InputKeyOk:
                    if(event.type == InputTypeShort) m->survey = !m->survey;
                    break;
                case InputKeyUp:
                    if(m->scroll) --m->scroll;
                    break;
                case InputKeyDown:
                    if(m->scroll + 1 < m->count) ++m->scroll;
                    break;
                default:
                    break;
                }
            } else if(event.type == InputTypeLong && event.key == InputKeyUp) {
                m->details = !m->details;
            } else if(event.type == InputTypeLong && event.key == InputKeyOk) {
                m->logging = !m->logging;
                m->log_error = false;
            }
            furi_mutex_release(a->mutex);
        }
        view_port_update(a->viewport);
    }
    furi_mutex_acquire(a->mutex, FuriWaitForever);
    a->model.stop = true;
    furi_mutex_release(a->mutex);
    furi_thread_join(a->worker);
    furi_thread_free(a->worker);
    if(a->decode_worker) {
        furi_thread_join(a->decode_worker);
        furi_thread_free(a->decode_worker);
        furi_message_queue_free(a->decode_jobs);
    }
    view_port_enabled_set(a->viewport, false);
    gui_remove_view_port(gui, a->viewport);
    view_port_free(a->viewport);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_NOTIFICATION);
    furi_stream_buffer_free(a->stream);
    furi_message_queue_free(a->input);
    furi_mutex_free(a->mutex);
    free(a->payload);
    free(a);
    return 0;
}
