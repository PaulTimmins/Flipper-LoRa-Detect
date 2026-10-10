#include "lora_detect.c"
static void seed(const LoraDetection* e, void* ctx) {
    App* a = ctx;
    if(a->model.count == HISTORY) return;
    Heard* h = &a->model.heard[a->model.count++];
    h->event = *e;
    h->generation = a->capture.generation;
    h->frequency = a->capture.frequency;
    h->payload_state = 1;
    h->packet_type = -1;
}
int32_t lora_check_app(void* ctx) {
    UNUSED(ctx);
    App* a = malloc(sizeof(App));
    memset(a, 0, sizeof(*a));
    a->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    a->stream = furi_stream_buffer_alloc(STREAM_BYTES, sizeof(uint32_t));
    a->decode_jobs = furi_message_queue_alloc(1, sizeof(uint8_t));
    a->storage = furi_record_open(RECORD_STORAGE);
    a->notifications = furi_record_open(RECORD_NOTIFICATION);
    a->payload = malloc(sizeof(LoraPayload));
    a->capture.busy = true;
    File* file = storage_file_alloc(a->storage);
    furi_check(storage_file_open(
        file, EXT_PATH("apps_data/lora_detect/check.bin"), FSAM_READ, FSOM_OPEN_EXISTING));
    a->capture.count = storage_file_read(file, a->capture.data, sizeof(a->capture.data)) / 4;
    storage_file_close(file);
    a->capture.frequency = lora_profiles[0].frequency;
    a->capture.generation = 123456;
    LoraDetector detector;
    lora_detector_init(&detector, 62500, seed, a);
    for(unsigned i = 0; i < a->capture.count; ++i) {
        uint32_t w = a->capture.data[i];
        lora_detector_feed(&detector, w >> 31, w & 0x7fffffff);
    }
    unsigned seeded = a->model.count;
    a->model.decoding = true;
    a->model.decode_started = furi_get_tick();
    a->worker = furi_thread_alloc_ex("LoRaRx", 4096, worker, a);
    furi_thread_start(a->worker);
    furi_delay_ms(200);
    /* Exercise the real busy-arrival path enough times to overflow old history. */
    LoraDetection event = a->model.heard[0].event;
    for(unsigned i = 0; i < 32; ++i)
        detected(&event, a);
    a->decode_worker = furi_thread_alloc_ex("LoRaDecode", 4096, decode_worker, a);
    furi_thread_set_priority(a->decode_worker, FuriThreadPriorityNormal);
    furi_thread_start(a->decode_worker);
    uint8_t job = 1;
    furi_message_queue_put(a->decode_jobs, &job, 0);
    uint32_t begin = furi_get_tick();
    bool done = false;
    while(furi_get_tick() - begin < furi_ms_to_ticks(120000)) {
        furi_delay_ms(250);
        furi_mutex_acquire(a->mutex, FuriWaitForever);
        done = !a->model.decoding;
        furi_mutex_release(a->mutex);
        if(done) break;
    }
    furi_mutex_acquire(a->mutex, FuriWaitForever);
    a->model.stop = true;
    furi_mutex_release(a->mutex);
    furi_thread_join(a->worker);
    furi_thread_join(a->decode_worker);
    furi_check(storage_file_open(
        file, EXT_PATH("apps_data/lora_detect/check.txt"), FSAM_WRITE, FSOM_CREATE_ALWAYS));
    char line[180];
    int n = snprintf(
        line,
        sizeof(line),
        "done=%d progress=%u seeded=%u retained=%u skipped=%lu lost=%lu elapsed_ms=%lu\n",
        done,
        a->model.decode_progress,
        seeded,
        a->model.count,
        (unsigned long)a->model.decode_skipped,
        (unsigned long)a->model.drops,
        (unsigned long)ticks_to_ms(furi_get_tick() - begin));
    storage_file_write(file, line, n);
    for(unsigned i = 0; i < a->model.count; ++i) {
        Heard* h = &a->model.heard[i];
        n = snprintf(
            line,
            sizeof(line),
            "state=%u type=%d name=%s\n",
            h->payload_state,
            h->packet_type,
            h->name);
        storage_file_write(file, line, n);
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_thread_free(a->worker);
    furi_thread_free(a->decode_worker);
    furi_message_queue_free(a->decode_jobs);
    furi_stream_buffer_free(a->stream);
    furi_mutex_free(a->mutex);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_NOTIFICATION);
    free(a->payload);
    free(a);
    return 0;
}
