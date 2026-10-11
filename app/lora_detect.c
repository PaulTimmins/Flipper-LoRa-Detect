#include <furi.h>
#include <furi/core/memmgr_heap.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include "chat_keyboard.h"
#include "capture_codec.h"
#include <gui/view_dispatcher.h>
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
#include "protocol.h"
#include "transmit.h"
#include <toolbox/saved_struct.h>

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
    uint8_t query_reply;
    char name[33], detail[48];
} Heard;
typedef struct {
    Heard heard[HISTORY];
    unsigned count, scroll, requested, active;
    bool survey, logging, log_error, radio_error, stop, details, decode_available;
    uint32_t drops, rate, headroom, decode_started, decode_skipped;
    unsigned decode_progress;
    bool decoding, sound, mt_crypto, mc_crypto, backlight, settings_open, settings_error,
        settings_wait_release;
    uint8_t decode_mode, setting_row;
    bool tx_menu, tx_pending, tx_busy, tx_cancel, flood, tx_review;
    char tx_preview[LORA_MT_MESSAGE_MAX + 1];
    uint8_t tx_row, mc_hop_bytes;
    bool identity_ready, name_invalid;
    char node_name[LORA_NAME_MAX + 1];
    uint32_t tx_sequence, tx_node, listen_until, query_tag, query_replies, query_tentative;
    char tx_status[40];
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
    Model model, draw_model;
    unsigned profile;
    LoraTxWave tx_wave;
    ViewDispatcher* keyboard;
    bool keyboard_saved;
    char draft[LORA_MT_MESSAGE_MAX + 1];
    LoraIdentity identity;
    uint32_t next_tx, tx_cooldown;
    int32_t utc_offset;
    uint32_t generation;
    struct {
        uint32_t pre[PRE_WORDS];
        uint16_t data[CAPTURE_WORDS];
        unsigned pre_write, pre_count, count;
        uint64_t time_us, base_us, end_us, until_us;
        uint32_t frequency, bandwidth, generation, deadline;
        uint8_t decode_mode;
        bool mt_crypto, mc_crypto;
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

typedef struct {
    uint8_t logging, sound, decode_mode, mt_crypto, backlight, scanning, mc_crypto;
} Settings;
#define SETTINGS_COUNT 11
static const uint8_t setting_ids[] = {11, 7, 8, 9, 10, 0, 1, 2, 3, 6, 4};
static const LoraTxAction tx_actions[] =
    {LoraTxMcText, LoraTxMcQuery, LoraTxMcCompose, LoraTxMtText, LoraTxMtQuery, LoraTxMtCompose};
static const char* tx_names[] = {
    "MC Public test",
    "MC local repeaters",
    "MC write message",
    "MT LongFast test",
    "MT local node query",
    "MT write message"};
static const char* decode_names[] = {"Off", "Quick", "Thorough"};
static const NotificationMessage packet_tone = {
    .type = NotificationMessageTypeSoundOn,
    .data.sound = {.frequency = 880, .volume = 0.5f}};
static const NotificationSequence packet_sound =
    {&packet_tone, &message_delay_100, &message_sound_off, NULL};
static int32_t generate_identity(void* context) {
    lora_identity_generate(context);
    return 0;
}
static void load_identity(App* a) {
    const char* path = EXT_PATH("apps_data/lora_detect/identity.bin");
    uint8_t uid[12] = {0};
    size_t size = furi_hal_version_uid_size();
    if(size > sizeof(uid)) size = sizeof(uid);
    memcpy(uid, furi_hal_version_uid(), size);
    bool exists = storage_file_exists(a->storage, path);
    bool loaded = saved_struct_load(path, &a->identity, sizeof(a->identity), 0xBA, 1);
    /* A corrupt existing identity must never silently reset keys or packet IDs. */
    if(exists && !loaded) return;
    bool copied = loaded && memcmp(uid, a->identity.device_uid, sizeof(uid));
    if(!loaded || copied) {
        memset(&a->identity, 0, sizeof(a->identity));
        memcpy(a->identity.device_uid, uid, sizeof(uid));
        uint32_t node = 0;
        if(!copied)
            saved_struct_load(EXT_PATH("apps_data/lora_detect/node.bin"), &node, 4, 0xB9, 1);
        if(node < 4 || node == UINT32_MAX || node == 0xf11f0001) {
            const uint8_t* mac = furi_hal_version_get_ble_mac();
            node = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) |
                   mac[5];
            while(node < 4 || node == UINT32_MAX)
                node = furi_hal_random_get();
        }
        a->identity.mt_node = node;
        /* Key generation has a larger stack, released before RX/decoder startup. */
        FuriThread* generator =
            furi_thread_alloc_ex("LoRaKeys", 8192, generate_identity, &a->identity);
        furi_thread_start(generator);
        furi_thread_join(generator);
        furi_thread_free(generator);
        if(!saved_struct_save(path, &a->identity, sizeof(a->identity), 0xBA, 1)) return;
    }
    if(a->identity.mt_node < 4 || a->identity.mt_node == UINT32_MAX || !a->identity.mc_public[0] ||
       a->identity.mc_public[0] == 255)
        return;
    a->model.tx_node = a->identity.mt_node;
    a->model.identity_ready = true;
    char name[LORA_NAME_MAX + 1] = {0};
    if(!copied &&
       saved_struct_load(EXT_PATH("apps_data/lora_detect/name.bin"), name, sizeof(name), 0xBB, 1) &&
       name[LORA_NAME_MAX] == 0 && lora_name_valid(name)) {
        memcpy(a->model.node_name, name, sizeof(name));
    } else {
        snprintf(
            a->model.node_name,
            sizeof(a->model.node_name),
            "Flipper %02X%02X%02X%02X",
            a->identity.mc_public[0],
            a->identity.mc_public[1],
            a->identity.mc_public[2],
            a->identity.mc_public[3]);
        if(!saved_struct_save(
               EXT_PATH("apps_data/lora_detect/name.bin"),
               a->model.node_name,
               sizeof(name),
               0xBB,
               1))
            a->model.identity_ready = false;
    }
}
static void load_settings(App* a) {
    Settings saved = {1, 1, 2, 1, 1, 0, 1};
    Settings loaded;
    if(saved_struct_load(
           EXT_PATH("apps_data/lora_detect/settings.bin"), &loaded, sizeof(loaded), 0xAD, 4) &&
       loaded.logging <= 1 && loaded.sound <= 1 && loaded.decode_mode <= 2 &&
       loaded.mt_crypto <= 1 && loaded.backlight <= 1 && loaded.scanning <= 1 &&
       loaded.mc_crypto <= 1) {
        saved = loaded;
    } else {
        /* Migrate older settings while leaving scanning off by default. */
        uint8_t old[6] = {0};
        bool v3 =
            saved_struct_load(EXT_PATH("apps_data/lora_detect/settings.bin"), old, 6, 0xAD, 3);
        bool v2 = !v3 && saved_struct_load(
                             EXT_PATH("apps_data/lora_detect/settings.bin"), old, 5, 0xAD, 2);
        bool valid =
            v3 || v2 ||
            saved_struct_load(EXT_PATH("apps_data/lora_detect/settings.bin"), old, 4, 0xAD, 1);
        if(valid && old[0] <= 1 && old[1] <= 1 && old[2] <= 2 && old[3] <= 1 &&
           (!(v2 || v3) || old[4] <= 1) && (!v3 || old[5] <= 1)) {
            saved.logging = old[0];
            saved.sound = old[1];
            saved.decode_mode = old[2];
            saved.mt_crypto = old[3];
            if(v2 || v3) saved.backlight = old[4];
            if(v3) saved.scanning = old[5];
        }
    }
    a->model.logging = saved.logging;
    a->model.sound = saved.sound;
    a->model.decode_mode = saved.decode_mode;
    a->model.mt_crypto = saved.mt_crypto;
    a->model.backlight = saved.backlight;
    a->model.survey = saved.scanning;
    a->model.mc_crypto = saved.mc_crypto;
    uint8_t radio[3] = {0, 1, 2};
    if(!saved_struct_load(EXT_PATH("apps_data/lora_detect/radio.bin"), radio, 3, 0xAE, 2)) {
        /* Preserve channel/route from v1; new width defaults to two bytes. */
        radio[0] = 0;
        radio[1] = 1;
        saved_struct_load(EXT_PATH("apps_data/lora_detect/radio.bin"), radio, 2, 0xAE, 1);
        radio[2] = 2;
    }
    a->model.mc_hop_bytes = radio[2] >= 1 && radio[2] <= 3 ? radio[2] : 2;
    a->model.requested = radio[0] < LORA_PROFILE_COUNT ? radio[0] : 0;
    a->profile = a->model.requested;
    a->model.flood = radio[1] == 1;
    a->next_tx = furi_hal_random_get() & 0x7fffffffU;
    saved_struct_load(EXT_PATH("apps_data/lora_tx_bench/sequence.bin"), &a->next_tx, 4, 0xB7, 1);
    saved_struct_load(EXT_PATH("apps_data/lora_detect/sequence.bin"), &a->next_tx, 4, 0xB7, 1);
    if(!a->next_tx) a->next_tx = 1;
    saved_struct_load(
        EXT_PATH("apps_data/lora_tx_bench/utc_offset.bin"), &a->utc_offset, 4, 0xB8, 1);
    if(a->utc_offset < -50400 || a->utc_offset > 50400) a->utc_offset = 0;
    load_identity(a);
    snprintf(a->model.tx_status, sizeof(a->model.tx_status), "Select action, OK sends once");
}
static void save_settings(App* a) {
    furi_mutex_acquire(a->mutex, FuriWaitForever);
    Settings saved = {
        a->model.logging,
        a->model.sound,
        a->model.decode_mode,
        a->model.mt_crypto,
        a->model.backlight,
        a->model.survey,
        a->model.mc_crypto};
    uint8_t radio[3] = {a->model.requested, a->model.flood, a->model.mc_hop_bytes};
    furi_mutex_release(a->mutex);
    bool ok = storage_simply_mkdir(a->storage, EXT_PATH("apps_data/lora_detect")) &&
              saved_struct_save(
                  EXT_PATH("apps_data/lora_detect/settings.bin"), &saved, sizeof(saved), 0xAD, 4);
    furi_mutex_acquire(a->mutex, FuriWaitForever);
    a->model.settings_error =
        !ok || !saved_struct_save(EXT_PATH("apps_data/lora_detect/radio.bin"), radio, 3, 0xAE, 2);
    furi_mutex_release(a->mutex);
}

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
        ok = storage_file_open(
            file, EXT_PATH("apps_data/lora_detect/detections.csv"), FSAM_WRITE, FSOM_OPEN_APPEND);
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

#define traffic_type lora_traffic_type

/* Stream bounded chunks to avoid putting a maximum packet dump on the stack. */
static bool write_packet_hex(File* file, const LoraPayloadResult* r, bool lines) {
    char hex[65];
    for(size_t offset = 0; offset < r->decoded_bytes; offset += 32) {
        size_t n = lora_report_hex(hex, sizeof(hex), r, offset);
        if(!n || storage_file_write(file, hex, n) != n) return false;
        if(lines && storage_file_write(file, "\n", 1) != 1) return false;
    }
    return true;
}

static bool log_packet_hex(App* a, const LoraPayloadResult* r, int rssi, uint32_t timestamp) {
    File* file = storage_file_alloc(a->storage);
    bool ok = storage_file_open(
        file, EXT_PATH("apps_data/lora_detect/packets.csv"), FSAM_WRITE, FSOM_OPEN_APPEND);
    if(ok && storage_file_size(file) == 0) {
        const char* header =
            "timestamp,frequency_hz,bandwidth_hz,sf,coding_rate,network,rssi_dbm,detection_us,status,complete,crc_present,received_crc,computed_crc,declared_bytes,recovered_bytes,combined_copies,hex\n";
        ok = storage_file_write(file, header, strlen(header)) == strlen(header);
    }
    if(ok) {
        char row[256], signal[16] = "";
        if(rssi != 1000) snprintf(signal, sizeof(signal), "%d", rssi);
        int n = snprintf(
            row,
            sizeof(row),
            "%lu,%lu,%lu,%u,%u,%s,%s,%llu,%s,%u,%u,%04X,%04X,%u,%u,%u,",
            (unsigned long)timestamp,
            (unsigned long)a->capture.frequency,
            (unsigned long)a->capture.bandwidth,
            r->sf,
            r->coding_rate,
            r->meshtastic ? "MT" : "MC",
            signal,
            (unsigned long long)r->detection_time,
            r->status == LoraPayloadCrcOk        ? "crc_ok" :
            r->status == LoraPayloadHeaderFailed ? "header_failed" :
            !r->payload_complete                 ? "incomplete" :
            r->crc_present                       ? "crc_failed" :
                                                   "no_crc",
            r->payload_complete,
            r->crc_present,
            r->received_crc,
            r->computed_crc,
            r->length,
            r->decoded_bytes,
            r->combined_copies);
        ok = n > 0 && n < (int)sizeof(row) && storage_file_write(file, row, n) == (size_t)n;
        if(ok) ok = write_packet_hex(file, r, false);
        if(ok) ok = storage_file_write(file, "\n", 1) == 1;
    }
    storage_file_close(file);
    storage_file_free(file);
    return ok;
}

/* Plain UTF-8 text for the stock iPhone app's file viewer. */
static bool
    log_phone_report(App* a, const LoraPayloadResult* result, int rssi, uint32_t timestamp) {
    File* file = storage_file_alloc(a->storage);
    bool ok =
        storage_simply_mkdir(a->storage, EXT_PATH("apps_data/lora_detect")) &&
        storage_file_open(
            file, EXT_PATH("apps_data/lora_detect/LoRa Logs.txt"), FSAM_WRITE, FSOM_OPEN_APPEND);
    if(ok && storage_file_size(file) == 0) {
        const char* header = "LoRa Detect - traffic log\n\n"
                             "Logging defaults on. Hold OK opens settings.\n"
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
        if(ok && result->text[0]) {
            const char* label = result->public_mac_ok ?
                                    "Public channel MAC matched; sender unverified\nChat: " :
                                    "Chat: ";
            ok = storage_file_write(file, label, strlen(label)) == strlen(label);
            size_t len = strlen(result->text);
            if(ok) ok = storage_file_write(file, result->text, len) == len;
            if(ok) ok = storage_file_write(file, "\n\n", 2) == 2;
        }
        if(ok) {
            int used = snprintf(
                entry,
                sizeof(entry),
                "Payload hex: %u/%u bytes recovered%s\n",
                result->decoded_bytes,
                result->length,
                result->status == LoraPayloadHeaderFailed ? " (declared length untrusted)" : "");
            ok = used > 0 && used < (int)sizeof(entry) &&
                 storage_file_write(file, entry, used) == (size_t)used;
            if(ok) ok = write_packet_hex(file, result, true);
            if(ok) ok = storage_file_write(file, "\n", 1) == 1;
        }
    }
    storage_file_close(file);
    storage_file_free(file);
    return ok;
}

static void log_payload(App* a, const LoraPayloadResult* r, int rssi, uint32_t timestamp) {
    File* file = storage_file_alloc(a->storage);
    bool ok =
        storage_simply_mkdir(a->storage, EXT_PATH("apps_data/lora_detect")) &&
        storage_file_open(
            file, EXT_PATH("apps_data/lora_detect/traffic.csv"), FSAM_WRITE, FSOM_OPEN_APPEND);
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
    bool hex_ok = log_packet_hex(a, r, rssi, timestamp);
    ok = ok && report_ok && hex_ok;
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

static void payload_decoded(const LoraPayloadResult* raw, void* context) {
    App* a = context;
    LoraPayloadResult parsed = *raw;
    if(parsed.meshtastic && parsed.payload_complete)
        lora_mt_describe(&parsed, a->capture.mt_crypto);
    else if(!parsed.meshtastic)
        lora_mc_public_describe(&parsed, a->capture.mc_crypto);
    const LoraPayloadResult* result = &parsed;
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
        if(h->event.time_us == result->detection_time && h->generation == a->capture.generation &&
           h->frequency == a->capture.frequency) {
            unsigned reply = lora_query_reply_state(result, a->model.query_tag);
            if(reply > h->query_reply) {
                if(h->query_reply == 1 && a->model.query_tentative) --a->model.query_tentative;
                if(reply == 2)
                    ++a->model.query_replies;
                else
                    ++a->model.query_tentative;
                h->query_reply = reply;
            }
            h->payload_state = result->status == LoraPayloadCrcOk   ? 4 :
                               result->status == LoraPayloadDamaged ? 3 :
                                                                      2;
            h->payload_length = result->length;
            h->packet_type = result->packet_type;
            h->copies = result->combined_copies;
            memcpy(h->name, result->name, sizeof(h->name));
            memcpy(h->detail, result->detail, sizeof(h->detail));
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
    furi_mutex_acquire(a->mutex, FuriWaitForever);
    bool decode = a->model.decode_mode != 0, sound = a->model.sound;
    bool backlight = a->model.backlight;
    furi_mutex_release(a->mutex);
    if(backlight) notification_message(a->notifications, &sequence_display_backlight_on);
    if(sound) notification_message(a->notifications, &packet_sound);
    if(a->payload && decode && lora_payload_supported(event)) h.payload_state = capture_start(a);
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
        a->capture.decode_mode = a->model.decode_mode;
        a->capture.mt_crypto = a->model.mt_crypto;
        a->capture.mc_crypto = a->model.mc_crypto;
    }
    furi_mutex_release(a->mutex);
    if(busy) return a->capture.capturing ? 6 : 7;
    a->capture.count = 0;
    uint64_t span = 0;
    for(unsigned j = 0; j < a->capture.pre_count; ++j) {
        uint32_t word =
            a->capture
                .pre[(a->capture.pre_write + PRE_WORDS - a->capture.pre_count + j) % PRE_WORDS];
        furi_check(capture_pack(a->capture.data, &a->capture.count, CAPTURE_WORDS, word));
        span += word & 0x7fffffffu;
    }
    a->capture.base_us = a->capture.time_us - span;
    a->capture.frequency = lora_profiles[a->profile].frequency;
    a->capture.bandwidth = lora_profiles[a->profile].bandwidth;
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
        bool stored = capture_pack(a->capture.data, &a->capture.count, CAPTURE_WORDS, word);
        if(!stored || a->capture.count == CAPTURE_WORDS ||
           a->capture.time_us >= a->capture.until_us)
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
    bool stop = a->model.stop || a->model.decode_mode == 0 || a->model.tx_pending ||
                a->model.tx_busy;
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
        static const float phases[] = {0, -0.5f, -0.25f, 0.25f, 0.5f};
        unsigned passes = a->capture.decode_mode == 2 ? COUNT_OF(phases) : 1;
        for(unsigned phase = 0; phase < passes; ++phase) {
            lora_payload_init(a->payload, payload_decoded, a);
            a->payload->phase_bias = phases[phase] * 1000000.0f / a->capture.bandwidth;
            a->payload->cancelled = decode_cancelled;
            a->payload->time_us = a->capture.base_us;
            LoraDetector detector;
            lora_detector_init(&detector, a->capture.bandwidth, replay_detected, a);
            detector.time_us = a->capture.base_us;
            unsigned pulses = 0;
            for(unsigned i = 0; i < a->capture.count;) {
                if((pulses++ & 31) == 0) {
                    furi_mutex_acquire(a->mutex, FuriWaitForever);
                    stop = a->model.stop || a->model.decode_mode == 0 || a->model.tx_pending ||
                           a->model.tx_busy;
                    a->model.decode_progress =
                        (phase * a->capture.count + i) * 100 / (passes * a->capture.count);
                    furi_mutex_release(a->mutex);
                    if(stop) break;
                    furi_delay_tick(1);
                }
                uint32_t word;
                if(!capture_unpack(a->capture.data, a->capture.count, &i, &word)) break;
                lora_payload_feed(a->payload, word >> 31, word & 0x7fffffffu);
                lora_detector_feed(&detector, word >> 31, word & 0x7fffffffu);
            }
            if(stop) break;
            lora_payload_end(a->payload);
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
                h->payload_state = stop ? 5 : 8;
        }
        a->model.decoding = false;
        a->model.decode_progress = 100;
        a->capture.busy = false;
        bool wake_display = !stop && !a->model.stop && a->model.backlight;
        furi_mutex_release(a->mutex);
        if(wake_display) notification_message(a->notifications, &sequence_display_backlight_on);
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

/* TX and RX share one radio worker. No decoder may access borrowed RAM until
 * TX completes; the decoder clears capture.busy after its final log write. */
static bool tx_cancelled(void* context) {
    App* a = context;
    furi_delay_tick(1);
    furi_mutex_acquire(a->mutex, FuriWaitForever);
    bool cancel = a->model.stop || a->model.tx_cancel;
    furi_mutex_release(a->mutex);
    return cancel;
}
static LevelDuration tx_next(void* context) {
    App* a = context;
    bool level;
    uint32_t duration = lora_tx_next(&a->tx_wave, &level);
    return duration ? level_duration_make(level, duration) : level_duration_reset();
}
static bool tx_frequency(uint32_t frequency) {
    if(furi_hal_region_is_frequency_allowed(frequency)) {
        furi_hal_subghz_set_frequency_and_path(frequency);
        return true;
    }
    const FuriHalRegion* original = furi_hal_region_get();
    if(!original || strcmp(original->country_code, "US") || original->bands_count > 16 ||
       frequency < 902300000U || frequency > 927700000U)
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
        .start = frequency, .end = frequency, .power_limit = -10, .duty_cycle = 50};
    furi_hal_region_set(temporary);
    furi_hal_subghz_set_frequency_and_path(frequency);
    furi_hal_region_set(saved);
    return true;
}
static void transmit_once(App* a) {
    furi_mutex_acquire(a->mutex, FuriWaitForever);
    LoraTxAction action = tx_actions[a->model.tx_row];
    char message[LORA_MT_MESSAGE_MAX + 1];
    snprintf(message, sizeof(message), "%s", a->draft);
    bool flood = a->model.flood;
    uint8_t mc_hop_bytes = a->model.mc_hop_bytes;
    uint32_t node = a->model.tx_node;
    bool identity_ready = a->model.identity_ready;
    char name[LORA_NAME_MAX + 1];
    memcpy(name, a->model.node_name, sizeof(name));
    a->model.tx_busy = true;
    furi_mutex_release(a->mutex);
    const char* outcome = "Cancelled";
    bool sent = false;
    uint8_t* tx_extra = NULL;
    uint32_t sequence = a->next_tx,
             timestamp = (uint32_t)((int64_t)furi_hal_rtc_get_timestamp() + a->utc_offset);
    bool mt = action == LoraTxMtText || action == LoraTxMtQuery || action == LoraTxMtCompose;
    bool query = action == LoraTxMcQuery || action == LoraTxMtQuery;
    bool composed = action == LoraTxMcCompose || action == LoraTxMtCompose;
    uint32_t carrier = mt ? 906875000U : 910483750U;
    if(!identity_ready) {
        outcome = "Identity save/load failed";
        goto done;
    }
    if((int32_t)(furi_get_tick() - a->tx_cooldown) < 0) {
        outcome = "Cooldown; try again shortly";
        goto done;
    }
    /* Cancel an unfinished capture, then wait for any decoder job to release
     * both buffers. The pending flag also cancels inside slow symbol searches. */
    if(a->capture.capturing) {
        a->capture.capturing = false;
        cancel_pending(a);
        furi_mutex_acquire(a->mutex, FuriWaitForever);
        a->capture.busy = false;
        furi_mutex_release(a->mutex);
    }
    for(;;) {
        furi_mutex_acquire(a->mutex, FuriWaitForever);
        bool busy = a->capture.busy;
        furi_mutex_release(a->mutex);
        if(!busy) break;
        if(tx_cancelled(a)) goto done;
    }
    if(tx_cancelled(a)) goto done;
    a->tx_wave = (LoraTxWave){
        .first = (uint8_t*)a->capture.data,
        .first_size = sizeof(a->capture.data),
        .second = (uint8_t*)a->payload,
        .second_size = a->payload ? sizeof(*a->payload) : 0,
        .cancelled = tx_cancelled,
        .context = a};
    uint8_t packet[64];
    size_t length =
        composed ?
            lora_tx_message(
                packet, mt, message, sequence, timestamp, node, flood, mc_hop_bytes, name) :
            lora_tx_packet(packet, action, sequence, timestamp, node, flood, mc_hop_bytes, name);
    /* Only longer MT packets need more than the shared 16 KB + decoder area.
     * This transient extension is freed before RX/logging resumes. */
    if(mt && length > 48) {
        if(memmgr_get_free_heap() < 2048 + 3072 || memmgr_heap_get_max_free_block() < 2048) {
            outcome = "TX memory unavailable";
            goto done;
        }
        tx_extra = malloc(2048);
        a->tx_wave.extra = tx_extra;
        a->tx_wave.extra_size = 2048;
    }
    if(!length || !lora_tx_waveform(&a->tx_wave, packet, length, mt)) {
        outcome = "Waveform failed / cancelled";
        goto done;
    }
    uint32_t next = sequence + 1;
    if(!next ||
       !saved_struct_save(EXT_PATH("apps_data/lora_detect/sequence.bin"), &next, 4, 0xB7, 1)) {
        outcome = "Sequence save failed; no TX";
        goto done;
    }
    a->next_tx = next;
    if(tx_cancelled(a)) goto done;
    static const uint8_t preset[] = {0x02, 0x0D, 0x03, 0x07, 0x08, 0x32, 0x0B, 0x06, 0x10,
                                     0x0E, 0x11, 0x83, 0x12, 0x30, 0x13, 0,    0x14, 0,
                                     0x18, 0x18, 0x1D, 0x91, 0x21, 0xB6, 0x22, 0x11, 0,
                                     0,    0,    0x27, 0,    0,    0,    0,    0,    0};
    furi_hal_subghz_load_custom_preset(preset);
    if(!tx_frequency(carrier)) {
        outcome = "Region blocks TX";
        goto done;
    }
    if(!furi_hal_subghz_start_async_tx(tx_next, a)) {
        outcome = "Radio TX failed";
        goto restore;
    }
    furi_mutex_acquire(a->mutex, FuriWaitForever);
    snprintf(
        a->model.tx_status, sizeof(a->model.tx_status), "Sending #%lu...", (unsigned long)sequence);
    furi_mutex_release(a->mutex);
    uint32_t started = furi_get_tick();
    while(!furi_hal_subghz_is_async_tx_complete() && !tx_cancelled(a) &&
          furi_get_tick() - started < furi_ms_to_ticks(2000)) {
    }
    sent = furi_hal_subghz_is_async_tx_complete();
    furi_hal_subghz_stop_async_tx();
    outcome = sent ? "Sent; listening (no ACK)" : "TX stopped / timed out";
    a->tx_cooldown = furi_get_tick() + furi_ms_to_ticks(query ? 60000 : 5000);
restore:
    /* Clear cached exception before RX restores its normal preset/frequency. */
    furi_hal_subghz_set_frequency(carrier);
done:
    free(tx_extra);
    a->tx_wave.extra = NULL;
    a->tx_wave.extra_size = 0;
    furi_mutex_acquire(a->mutex, FuriWaitForever);
    a->model.tx_pending = a->model.tx_busy = false;
    a->model.tx_sequence = sequence;
    snprintf(a->model.tx_status, sizeof(a->model.tx_status), "%s", outcome);
    if(sent) {
        a->profile = mt ? 1 : 0;
        a->model.requested = a->profile;
        a->model.survey = false;
        a->model.tx_menu = false;
        a->model.tx_review = false;
        a->model.scroll = 0;
        a->model.listen_until = query ? furi_get_tick() + furi_ms_to_ticks(60000) : 0;
        a->model.query_tag = query ? sequence : 0;
        a->model.query_replies = a->model.query_tentative = 0;
    }
    bool logging = a->model.logging;
    furi_mutex_release(a->mutex);
    tune(a); /* Listen before doing any filesystem work. */
    if(logging) {
        File* file = storage_file_alloc(a->storage);
        if(storage_file_open(
               file,
               EXT_PATH("apps_data/lora_detect/LoRa Logs.txt"),
               FSAM_WRITE,
               FSOM_OPEN_APPEND)) {
            char line[160];
            int n = snprintf(
                line,
                sizeof(line),
                "TX action=%u id=%lu UTC=%lu: %s; %s\n\n",
                action,
                (unsigned long)sequence,
                (unsigned long)timestamp,
                outcome,
                query ? "local query" :
                flood ? "flood test" :
                        "local test");
            storage_file_write(file, line, n);
            if(sent && composed) {
                n = snprintf(
                    line,
                    sizeof(line),
                    "Sent on %s: %s\n\n",
                    mt ? "LongFast" : "MC Public",
                    message);
                storage_file_write(file, line, n);
            }
        }
        storage_file_free(file);
    }
    if(sent) save_settings(a);
}

/* Called only by RX, before processing captures or TX. Allocate after core
 * services have started; retry when USB/loader temporary allocations disappear.
 * Reserve 12 KB for the decoder thread plus GUI, storage and TX allocations. */
static void try_decoder_alloc(App* a) {
    if(a->payload || memmgr_get_free_heap() < sizeof(LoraPayload) + 12288 ||
       memmgr_heap_get_max_free_block() < sizeof(LoraPayload))
        return;
    a->payload = malloc(sizeof(LoraPayload));
    a->decode_jobs = furi_message_queue_alloc(1, sizeof(uint8_t));
    a->decode_worker = furi_thread_alloc_ex("LoRaDecode", 4096, decode_worker, a);
    furi_thread_set_priority(a->decode_worker, FuriThreadPriorityNormal);
    furi_thread_start(a->decode_worker);
    furi_mutex_acquire(a->mutex, FuriWaitForever);
    a->model.decode_available = true;
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
    uint32_t min_free = STREAM_BYTES, memory_check = 0;
    bool was_survey = false;
    for(;;) {
        furi_mutex_acquire(a->mutex, FuriWaitForever);
        bool stop = a->model.stop, survey = a->model.survey, decode = a->model.decode_mode != 0;
        unsigned requested = a->model.requested;
        bool tx = a->model.tx_pending;
        furi_mutex_release(a->mutex);
        if(stop) break;
        if(!a->payload && furi_get_tick() - memory_check >= furi_ms_to_ticks(1000)) {
            memory_check = furi_get_tick();
            try_decoder_alloc(a);
        }
        if(tx) {
            subghz_devices_stop_async_rx(a->radio);
            transmit_once(a);
            hop = furi_get_tick();
            continue;
        }
        if(!decode && a->capture.capturing) {
            a->capture.capturing = false;
            cancel_pending(a);
            furi_mutex_acquire(a->mutex, FuriWaitForever);
            a->capture.busy = false;
            furi_mutex_release(a->mutex);
        }
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
                if(a->payload) capture_feed(a, words[i]);
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
    /* GUI service has a small stack: keep the history snapshot in app storage. */
    Model* m = &a->draw_model;
    furi_mutex_acquire(a->mutex, FuriWaitForever);
    *m = a->model;
    furi_mutex_release(a->mutex);
    if(m->tx_menu) {
        canvas_set_font(canvas, FontSecondary);
        if(m->tx_review) {
            canvas_draw_str(canvas, 0, 8, tx_names[m->tx_row]);
            char line[23];
            for(unsigned i = 0; i < 3; ++i) {
                size_t offset = i * 22, n = strlen(m->tx_preview);
                snprintf(line, sizeof(line), "%.22s", offset < n ? m->tx_preview + offset : "");
                canvas_draw_str(canvas, 0, 23 + i * 10, line);
            }
            canvas_draw_str(canvas, 0, 54, m->flood ? "Route: Flood" : "Route: Local");
            canvas_draw_str(canvas, 0, 64, "OK send  Back edit");
            return;
        }
        canvas_draw_str(canvas, 0, 8, "Transmit / compose");
        char row[48];
        unsigned first = m->tx_row < 4 ? 0 : m->tx_row - 3;
        for(unsigned i = first; i < first + 4 && i < COUNT_OF(tx_actions); ++i) {
            snprintf(row, sizeof(row), "%c %s", m->tx_row == i ? '>' : ' ', tx_names[i]);
            canvas_draw_str(canvas, 0, 20 + (i - first) * 9, row);
        }
        canvas_draw_str(canvas, 0, 57, m->tx_status);
        canvas_draw_str(canvas, 0, 64, m->tx_busy ? "Back: cancel" : "Back: listening screen");
        return;
    }
    if(m->settings_open) {
        canvas_set_font(canvas, FontSecondary);
        char title[32];
        snprintf(
            title, sizeof(title), "Settings %u/%u (saved)", m->setting_row + 1, SETTINGS_COUNT);
        canvas_draw_str(canvas, 0, 8, title);
        const char* labels[] = {
            "Logging",
            "Packet sound",
            "Decode",
            "MT LongFast chat",
            "Auto backlight",
            "Scanning",
            "MC Public chat",
            "Channel",
            "Mode",
            "TX route",
            "MC hop bytes",
            "Name"};
        const char* values[] = {
            m->logging ? "On" : "Off",
            m->sound ? "On" : "Off",
            decode_names[m->decode_mode],
            m->mt_crypto ? "On" : "Off",
            m->backlight ? "On" : "Off",
            m->survey ? "On" : "Off",
            m->mc_crypto ? "On" : "Off",
            lora_profiles[m->requested].name,
            m->survey ? "Scan" : "Watch",
            m->flood ? "Flood" : "Local",
            m->mc_hop_bytes == 1 ? "1" :
            m->mc_hop_bytes == 2 ? "2" :
                                   "3",
            m->identity_ready ? m->node_name : "Identity error"};
        char row[48];
        unsigned first = m->setting_row < 5 ? 0 : m->setting_row - 4;
        for(unsigned i = first; i < first + 5 && i < SETTINGS_COUNT; ++i) {
            snprintf(
                row,
                sizeof(row),
                "%c %s: %s",
                i == m->setting_row ? '>' : ' ',
                labels[setting_ids[i]],
                values[setting_ids[i]]);
            canvas_draw_str(canvas, 0, 18 + (i - first) * 9, row);
        }
        canvas_draw_str(
            canvas,
            0,
            64,
            m->name_invalid   ? "No colon or edge spaces" :
            m->settings_error ? "Save failed - check SD" :
                                "Right/OK change  Back close");
        return;
    }
    const LoraProfile* p = &lora_profiles[m->active];
    char text[80];
    canvas_set_font(canvas, FontSecondary);
    snprintf(
        text,
        sizeof(text),
        "%lu.%04lu %s%s",
        (unsigned long)(p->frequency / 1000000),
        (unsigned long)((p->frequency % 1000000) / 100),
        m->survey ? "SCAN" : "WATCH",
        m->log_error ? " ERR" : (m->logging ? " REC" : ""));
    canvas_draw_str(canvas, 0, 8, text);
    canvas_draw_str(canvas, 0, 18, p->name);
    canvas_draw_line(canvas, 0, 20, 127, 20);
    if(m->radio_error)
        canvas_draw_str(canvas, 0, 34, "Radio unavailable");
    else if(!m->count) {
        canvas_draw_str(canvas, 0, 32, "Listening for LoRa...");
        snprintf(
            text,
            sizeof(text),
            "%lu edges/s free %lu",
            (unsigned long)m->rate,
            (unsigned long)m->headroom);
        canvas_draw_str(canvas, 0, 43, text);
        snprintf(text, sizeof(text), "Lost pulses: %lu", (unsigned long)m->drops);
        canvas_draw_str(canvas, 0, 54, text);
    } else {
        const Heard* h = &m->heard[m->scroll < m->count ? m->scroll : 0];
        if(m->details && (h->payload_state || h->event.sync == 0x12)) {
            const char* status = h->payload_state == 4 ? "CRC OK / sig unchecked" :
                                 h->payload_state == 3 ? "Partial decode?" :
                                 h->payload_state == 2 ? "Header not recovered" :
                                 h->payload_state == 5 ? "Decode interrupted" :
                                 h->payload_state == 1 ? "Decoding..." :
                                 h->payload_state == 6 ? "Capturing..." :
                                 h->payload_state == 7 ? "Skipped while busy" :
                                 h->payload_state == 8 ? "No payload recovered" :
                                 !m->decode_mode       ? "Decode: Off" :
                                 m->decode_available   ? "Unsupported PHY" :
                                                         "Decode: low memory";
            snprintf(
                text,
                sizeof(text),
                "%s%s",
                h->packet_type >= 0 ? traffic_type(h->packet_type) : status,
                h->packet_type >= 0 && h->payload_state != 4 ? "?" : "");
            canvas_draw_str(canvas, 0, 30, text);
            snprintf(
                text,
                sizeof(text),
                "%.22s",
                h->name[0]          ? h->name :
                h->detail[0]        ? h->detail :
                h->packet_type >= 0 ? "Name not recovered" :
                                      "");
            canvas_draw_str(canvas, 0, 41, text);
            if(h->payload_state == 4)
                snprintf(text, sizeof(text), "%ddBm %uB CRC OK", h->rssi, h->payload_length);
            else
                snprintf(text, sizeof(text), "%ddBm %uB tentative", h->rssi, h->payload_length);
            if(h->packet_type == 16 || h->packet_type == 33) {
                /* Show the preview in place of the generic confidence line. */
                snprintf(text, sizeof(text), "%.22s", h->detail);
            }
            canvas_draw_str(canvas, 0, 52, text);
            if(m->listen_until && (int32_t)(m->listen_until - furi_get_tick()) > 0) {
                snprintf(
                    text,
                    sizeof(text),
                    "Listen %lus %lu OK/%lu?",
                    (unsigned long)(ticks_to_ms(m->listen_until - furi_get_tick()) / 1000),
                    (unsigned long)m->query_replies,
                    (unsigned long)m->query_tentative);
            } else if(m->decoding) {
                snprintf(
                    text,
                    sizeof(text),
                    "Decode %u%% %lus",
                    m->decode_progress,
                    (unsigned long)(ticks_to_ms(furi_get_tick() - m->decode_started) / 1000));
            } else if(m->decode_progress == 100) {
                snprintf(
                    text, sizeof(text), "Done / %lu skipped", (unsigned long)m->decode_skipped);
            } else {
                snprintf(text, sizeof(text), "Hold OK: menu  Right: TX");
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
            m->drops ? " !loss" : "");
        canvas_draw_str(canvas, 0, 53, text);
    }
    if(m->listen_until && (int32_t)(m->listen_until - furi_get_tick()) > 0) {
        snprintf(
            text,
            sizeof(text),
            "Listen %lus %lu OK/%lu?",
            (unsigned long)(ticks_to_ms(m->listen_until - furi_get_tick()) / 1000),
            (unsigned long)m->query_replies,
            (unsigned long)m->query_tentative);
        canvas_draw_str(canvas, 0, 64, text);
    } else
        canvas_draw_str(canvas, 0, 64, "Hold OK: menu  Right: TX");
}

static void keyboard_done(void* context) {
    App* a = context;
    a->keyboard_saved = true;
    view_dispatcher_stop(a->keyboard);
}
static bool keyboard_back(void* context) {
    App* a = context;
    view_dispatcher_stop(a->keyboard);
    return true;
}
static void edit_text(App* a, Gui* gui, char* buffer, size_t limit, const char* title) {
    a->keyboard_saved = false;
    view_port_enabled_set(a->viewport, false);
    View* text = chat_keyboard_alloc(buffer, limit, title, keyboard_done, a);
    a->keyboard = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(a->keyboard, a);
    view_dispatcher_set_navigation_event_callback(a->keyboard, keyboard_back);
    view_dispatcher_attach_to_gui(a->keyboard, gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_add_view(a->keyboard, 0, text);
    view_dispatcher_switch_to_view(a->keyboard, 0);
    view_dispatcher_run(a->keyboard);
    view_dispatcher_remove_view(a->keyboard, 0);
    chat_keyboard_free(text);
    view_dispatcher_free(a->keyboard);
    a->keyboard = NULL;
    view_port_enabled_set(a->viewport, true);
}
static void edit_name(App* a, Gui* gui) {
    char name[LORA_NAME_MAX + 1];
    furi_mutex_acquire(a->mutex, FuriWaitForever);
    memcpy(name, a->model.node_name, sizeof(name));
    furi_mutex_release(a->mutex);
    edit_text(a, gui, name, LORA_NAME_MAX, "Node name");
    if(!a->keyboard_saved) return;
    bool valid = lora_name_valid(name);
    bool ok =
        valid &&
        saved_struct_save(EXT_PATH("apps_data/lora_detect/name.bin"), name, sizeof(name), 0xBB, 1);
    furi_mutex_acquire(a->mutex, FuriWaitForever);
    if(ok) memcpy(a->model.node_name, name, sizeof(name));
    a->model.settings_error = !ok;
    a->model.name_invalid = !valid;
    furi_mutex_release(a->mutex);
}
static void compose_message(App* a, Gui* gui) {
    bool mt = tx_actions[a->model.tx_row] == LoraTxMtCompose;
    size_t limit = mt ? LORA_MT_MESSAGE_MAX : lora_mc_message_limit(a->model.node_name);
    a->draft[limit] = 0;
    edit_text(a, gui, a->draft, limit, mt ? "LongFast" : "MC Public");
    furi_mutex_acquire(a->mutex, FuriWaitForever);
    a->model.tx_review = a->keyboard_saved && a->draft[0];
    snprintf(a->model.tx_preview, sizeof(a->model.tx_preview), "%s", a->draft);
    furi_mutex_release(a->mutex);
    view_port_enabled_set(a->viewport, true);
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
    storage_simply_mkdir(a->storage, EXT_PATH("apps_data/lora_detect"));
    load_settings(a);
    a->model.log_error = !log_phone_report(a, NULL, 1000, 0);
    a->model.details = true;
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
            bool save = false, preview_sound = false, wake_screen = false, compose = false,
                 rename = false;
            furi_mutex_acquire(a->mutex, FuriWaitForever);
            Model* m = &a->model;
            if(m->tx_menu) {
                if(event.type == InputTypeShort) {
                    if(event.key == InputKeyBack) {
                        if(m->tx_busy || m->tx_pending)
                            m->tx_cancel = true;
                        else if(m->tx_review) {
                            m->tx_review = false;
                            compose = true;
                        } else
                            m->tx_menu = false;
                    } else if(!m->tx_busy && !m->tx_pending) {
                        if(!m->tx_review && event.key == InputKeyUp)
                            m->tx_row =
                                (m->tx_row + COUNT_OF(tx_actions) - 1) % COUNT_OF(tx_actions);
                        if(!m->tx_review && event.key == InputKeyDown)
                            m->tx_row = (m->tx_row + 1) % COUNT_OF(tx_actions);
                        if(event.key == InputKeyOk) {
                            LoraTxAction action = tx_actions[m->tx_row];
                            if(!m->tx_review &&
                               (action == LoraTxMcCompose || action == LoraTxMtCompose))
                                compose = true;
                            else {
                                m->tx_pending = true;
                                m->tx_review = false;
                            }
                            m->tx_cancel = false;
                            snprintf(m->tx_status, sizeof(m->tx_status), "Preparing...");
                        }
                    }
                }
            } else if(m->settings_open) {
                if(m->settings_wait_release) {
                    if(event.type == InputTypeRelease && event.key == InputKeyOk)
                        m->settings_wait_release = false;
                } else if(event.type == InputTypeShort || event.type == InputTypeRepeat) {
                    if(event.key == InputKeyBack)
                        m->settings_open = false;
                    else if(event.key == InputKeyUp)
                        m->setting_row = (m->setting_row + SETTINGS_COUNT - 1) % SETTINGS_COUNT;
                    else if(event.key == InputKeyDown)
                        m->setting_row = (m->setting_row + 1) % SETTINGS_COUNT;
                    else if(event.key == InputKeyRight || event.key == InputKeyOk) {
                        if(setting_ids[m->setting_row] == 0) m->logging = !m->logging;
                        if(setting_ids[m->setting_row] == 1) {
                            m->sound = !m->sound;
                            preview_sound = m->sound;
                        }
                        if(setting_ids[m->setting_row] == 2)
                            m->decode_mode = (m->decode_mode + 1) % 3;
                        if(setting_ids[m->setting_row] == 3) m->mt_crypto = !m->mt_crypto;
                        if(setting_ids[m->setting_row] == 4) m->backlight = !m->backlight;
                        if(setting_ids[m->setting_row] == 5) m->survey = !m->survey;
                        if(setting_ids[m->setting_row] == 6) m->mc_crypto = !m->mc_crypto;
                        if(setting_ids[m->setting_row] == 7)
                            m->requested = (m->requested + 1) % LORA_PROFILE_COUNT;
                        if(setting_ids[m->setting_row] == 8) m->survey = !m->survey;
                        if(setting_ids[m->setting_row] == 9) m->flood = !m->flood;
                        if(setting_ids[m->setting_row] == 10)
                            m->mc_hop_bytes = m->mc_hop_bytes % 3 + 1;
                        if(setting_ids[m->setting_row] == 11)
                            rename = true;
                        else
                            save = true;
                    }
                }
            } else if(event.type == InputTypeShort || event.type == InputTypeRepeat) {
                switch(event.key) {
                case InputKeyBack:
                    running = false;
                    break;
                case InputKeyLeft:
                    /* Deliberately inactive: no accidental channel changes. */
                    break;
                case InputKeyRight:
                    if(event.type == InputTypeShort) m->tx_menu = true;
                    wake_screen = true;
                    break;
                case InputKeyOk:
                    if(event.type == InputTypeShort) wake_screen = true;
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
            } else if(event.type == InputTypeLong && event.key == InputKeyOk) {
                m->settings_open = true;
                m->settings_wait_release = true;
            } else if(event.type == InputTypeLong && event.key == InputKeyUp) {
                m->details = !m->details;
            }
            furi_mutex_release(a->mutex);
            if(compose) compose_message(a, gui);
            if(rename) edit_name(a, gui);
            if(wake_screen) notification_message(a->notifications, &sequence_display_backlight_on);
            if(save) save_settings(a);
            if(preview_sound) notification_message(a->notifications, &packet_sound);
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
