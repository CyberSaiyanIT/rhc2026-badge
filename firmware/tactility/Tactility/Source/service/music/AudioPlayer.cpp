#include <Tactility/service/music/AudioPlayer.h>

#include <Tactility/MountPoints.h>

#include <tactility/device.h>
#include <tactility/drivers/audio_stream.h>
#include <tactility/drivers/i2s_controller.h>
#include <tactility/log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>

// audio_element.h must come first: the codec headers below use audio_element_handle_t
// without including it themselves.
#include <audio_element.h>
#include <Tactility/service/music/AudioEffects.h>

#include <audio_alc.h>
#include <audio_event_iface.h>
#include <audio_pipeline.h>
#include <audio_sonic.h>
#include <aac_decoder.h>
#include <esp_heap_caps.h>
#include <fatfs_stream.h>
#include <mp3_decoder.h>
#include <raw_stream.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <sys/stat.h>

namespace tt::service::music {

constexpr auto* TAG = "AudioPlayer";

const uint16_t EQUALIZER_BAND_FREQUENCIES[EQUALIZER_BANDS] = {
    31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000
};

namespace {

// Half a megabyte of decoded PCM: about 3 seconds at 44100/16/2, enough to ride out an SD card
// read that stalls behind another task's flash access.
constexpr size_t RING_BYTES = 512 * 1024;
// Playback waits for the ring to be half full. Only what is needed to start: the pump refills
// far faster than realtime, so a larger ring buys only silence after each press of play.
constexpr size_t PREBUFFER_BYTES = RING_BYTES / 8;
constexpr size_t PUMP_CHUNK_BYTES = 4096;
// How long the prebuffer gate waits before it either starts under-filled or reports a failure.
constexpr uint32_t BUFFERING_TIMEOUT_MS = 8000;

constexpr uint32_t DEFAULT_SAMPLE_RATE = 44100;
constexpr uint8_t DEFAULT_CHANNELS = 2;
constexpr uint8_t BITS_PER_SAMPLE = 16;
// Quietest level the meter shows. Below this a track's noise floor would keep the bar alive.
constexpr float LEVEL_FLOOR_DB = -48.0f;
// Corners of the three colour bands. One-pole filters, so these are where each band is 3 dB
// down rather than hard edges - the overlap is what makes the colour crossfade smoothly.
constexpr float BASS_CORNER_HZ = 200.0f;
constexpr float TREBLE_CORNER_HZ = 4000.0f;

bool hasExtension(const std::string& path, const char* extension) {
    const auto length = std::strlen(extension);
    if (path.size() < length) {
        return false;
    }
    return strcasecmp(path.c_str() + path.size() - length, extension) == 0;
}

}

struct AudioPlayer::Impl {
    Device* audioDevice = nullptr;
    /**
     * The controller the codec is wired to, held only to read its underrun count. Found by type
     * rather than followed from the codec; a board with two I2S controllers would need that route.
     */
    Device* i2sDevice = nullptr;
    AudioStreamHandle output = nullptr;

    audio_pipeline_handle_t pipeline = nullptr;
    audio_element_handle_t reader = nullptr;
    audio_element_handle_t decoder = nullptr;
    audio_element_handle_t sonic = nullptr;
    audio_element_handle_t alc = nullptr;
    audio_element_handle_t raw = nullptr;
    audio_event_iface_handle_t events = nullptr;

    StreamBufferHandle_t ring = nullptr;
    uint8_t* ringStorage = nullptr;
    StaticStreamBuffer_t ringControl {};

    TaskHandle_t pumpTask = nullptr;
    TaskHandle_t outputTask = nullptr;
    std::atomic<bool> running { false };
    std::atomic<bool> paused { false };
    /** Set by the pump task when the decoder reports end of stream. */
    std::atomic<bool> sourceDrained { false };

    std::atomic<uint32_t> sampleRate { DEFAULT_SAMPLE_RATE };
    std::atomic<uint32_t> channels { DEFAULT_CHANNELS };
    /** Raised by the event thread; the output task reopens the stream when it differs. */
    std::atomic<uint32_t> openedSampleRate { 0 };
    std::atomic<uint32_t> openedChannels { 0 };

    std::atomic<State> state { State::Stopped };
    std::atomic<uint32_t> underruns { 0 };
    std::atomic<uint32_t> droppedBytes { 0 };
    std::atomic<uint8_t> bufferLowPercent { 100 };
    /** The controller's count when this track started, since the count itself is never reset. */
    std::atomic<uint32_t> dmaUnderrunBaseline { 0 };
    std::atomic<uint64_t> bytesPlayed { 0 };
    std::atomic<uint32_t> durationSeconds { 0 };
    std::atomic<uint32_t> averageBitrate { 0 };

    /** Byte offset the reader should restart at, or UINT32_MAX for "no seek pending". */
    std::atomic<uint32_t> seekToByte { UINT32_MAX };

    uint64_t fileSize = 0;
    std::string currentPath;
    /** What the bands are set to, which outlives any one pipeline. */
    int bandGains[EQUALIZER_BANDS] = {};

    /** Sonic takes both at once, so each is kept to pass alongside the other. */
    float pitch = 1.0f;
    float speed = 1.0f;

    /**
     * Everything downstream of the ring buffer. Not ESP-ADF elements: a change here is heard
     * within one chunk, where an element's change sits behind the whole ring buffer.
     */
    AudioEffects effects;
    /** Whether the end-of-track fade has already been asked for, so it is only asked for once. */
    bool fadingOut = false;

    mutable std::mutex mutex;

    // Plain members, not atomics: set once before the first play(), then only read.
    AudioLevelCallback levelCallback = nullptr;
    void* levelContext = nullptr;
    // Band-splitter state, carried across chunks. Touched only by the output task.
    float lowPassBass = 0.0f;
    float lowPassTreble = 0.0f;

    void applySonic();
    bool buildPipeline(const std::string& path);
    void teardownPipeline();
    bool openOutput(uint32_t rate, uint32_t channelCount);
    void applyMusicInfo();

    static void pumpTaskMain(void* context);
    static void outputTaskMain(void* context);
};

bool AudioPlayer::Impl::buildPipeline(const std::string& path) {
    audio_pipeline_cfg_t pipeline_config = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_config);
    if (pipeline == nullptr) {
        LOG_E(TAG, "Failed to create pipeline");
        return false;
    }

    fatfs_stream_cfg_t reader_config = FATFS_STREAM_CFG_DEFAULT();
    reader_config.type = AUDIO_STREAM_READER;
    // ESP-ADF's 4096 covers its own element loop, not what the loop calls into: every read runs
    // f_read -> FATFS -> ff_sdmmc_read -> the SDMMC driver on this same stack.
    reader_config.task_stack = 8192;
    reader = fatfs_stream_init(&reader_config);

    // Element task stacks stay in internal RAM. ESP-ADF defaults them to external, which needs an
    // IDF patch only ADF ships; without it every element silently fails to start.
    if (hasExtension(path, ".mp3")) {
        mp3_decoder_cfg_t decoder_config = DEFAULT_MP3_DECODER_CONFIG();
        decoder_config.stack_in_ext = false;
        decoder = mp3_decoder_init(&decoder_config);
    } else {
        aac_decoder_cfg_t decoder_config = DEFAULT_AAC_DECODER_CONFIG();
        decoder_config.stack_in_ext = false;
        decoder = aac_decoder_init(&decoder_config);
    }

    sonic_cfg_t sonic_config = DEFAULT_SONIC_CONFIG();
    sonic_config.sonic_info.samplerate = (int) sampleRate.load();
    sonic_config.sonic_info.channel = (int) channels.load();
    sonic_config.stack_in_ext = false;
    sonic = sonic_init(&sonic_config);

    alc_volume_setup_cfg_t alc_config = DEFAULT_ALC_VOLUME_SETUP_CONFIG();
    alc_config.stack_in_ext = false;
    alc = alc_volume_setup_init(&alc_config);

    raw_stream_cfg_t raw_config = RAW_STREAM_CFG_DEFAULT();
    raw_config.type = AUDIO_STREAM_READER;
    raw = raw_stream_init(&raw_config);

    if (reader == nullptr || decoder == nullptr || sonic == nullptr ||
        alc == nullptr || raw == nullptr) {
        LOG_E(TAG, "Failed to create a pipeline element");
        return false;
    }

    audio_pipeline_register(pipeline, reader, "file");
    audio_pipeline_register(pipeline, decoder, "dec");
    audio_pipeline_register(pipeline, sonic, "sonic");
    audio_pipeline_register(pipeline, alc, "alc");
    audio_pipeline_register(pipeline, raw, "raw");

    const char* order[] = { "file", "dec", "sonic", "alc", "raw" };
    audio_pipeline_link(pipeline, order, 5);

    applySonic();

    audio_element_set_uri(reader, path.c_str());

    // Without a bounded wait, raw_stream_read() blocks forever if an element upstream stalls, and
    // the event poll that would report the failure never gets a turn.
    audio_element_set_input_timeout(raw, pdMS_TO_TICKS(200));

    audio_event_iface_cfg_t event_config = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    events = audio_event_iface_init(&event_config);
    audio_pipeline_set_listener(pipeline, events);

    return true;
}

// Sonic reads a zero as "leave this one alone", so both values are always sent together.
void AudioPlayer::Impl::applySonic() {
    if (sonic != nullptr) {
        sonic_set_pitch_and_speed_info(sonic, pitch, speed);
    }
}

void AudioPlayer::Impl::teardownPipeline() {
    if (pipeline == nullptr) {
        return;
    }

    audio_pipeline_stop(pipeline);
    audio_pipeline_wait_for_stop(pipeline);
    audio_pipeline_terminate(pipeline);

    // buildPipeline() leaves unreached elements null and play() still tears down, so each is
    // checked: audio_element_deinit() dereferences the handle before testing it.
    for (audio_element_handle_t element : { reader, decoder, sonic, alc, raw }) {
        if (element != nullptr) {
            audio_pipeline_unregister(pipeline, element);
        }
    }

    if (events != nullptr) {
        audio_pipeline_remove_listener(pipeline);
        audio_event_iface_destroy(events);
        events = nullptr;
    }

    audio_pipeline_deinit(pipeline);
    for (audio_element_handle_t element : { reader, decoder, sonic, alc, raw }) {
        if (element != nullptr) {
            audio_element_deinit(element);
        }
    }

    pipeline = nullptr;
    reader = decoder = sonic = alc = raw = nullptr;
}

bool AudioPlayer::Impl::openOutput(uint32_t rate, uint32_t channelCount) {
    if (output != nullptr) {
        audio_stream_close(output);
        output = nullptr;
    }

    AudioStreamConfig config {
        .sample_rate = rate,
        .bits_per_sample = BITS_PER_SAMPLE,
        .channels = (uint8_t) channelCount
    };

    if (audio_stream_open_output(audioDevice, &config, &output) != ERROR_NONE) {
        LOG_E(TAG, "Failed to open audio output at %lu Hz", rate);
        output = nullptr;
        return false;
    }

    openedSampleRate = rate;
    openedChannels = channelCount;
    return true;
}

// The decoder only knows the stream's real format once it has parsed a frame, so the elements
// downstream of it are configured here rather than at construction.
void AudioPlayer::Impl::applyMusicInfo() {
    audio_element_info_t info {};
    audio_element_getinfo(decoder, &info);
    if (info.sample_rates <= 0 || info.channels <= 0) {
        return;
    }

    sampleRate = (uint32_t) info.sample_rates;
    channels = (uint32_t) info.channels;

    sonic_set_info(sonic, info.sample_rates, info.channels);

    if (info.bps > 0) {
        averageBitrate = (uint32_t) info.bps;
        if (fileSize > 0) {
            durationSeconds = (uint32_t) (fileSize * 8 / (uint64_t) info.bps);
        }
    }

    LOG_I(TAG, "Source: %d Hz, %d ch, %d bps", info.sample_rates, info.channels, info.bps);
}

// Drains decoded PCM out of the pipeline into the ring buffer. Kept separate from the output task
// so a slow read never blocks the write to the codec.
void AudioPlayer::Impl::pumpTaskMain(void* context) {
    auto* impl = static_cast<Impl*>(context);
    // External RAM: this task only copies pipeline output into the ring, so the slower access
    // does not matter, and internal RAM is where app stacks come from.
    auto* chunk = (uint8_t*) heap_caps_malloc(PUMP_CHUNK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (chunk == nullptr) {
        LOG_E(TAG, "Failed to allocate pump chunk");
        impl->state = State::Error;
        impl->running = false;
        // Cleared like every other exit from this task: stop() waits on this handle going null.
        impl->pumpTask = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    while (impl->running.load()) {
        audio_event_iface_msg_t message {};
        if (audio_event_iface_listen(impl->events, &message, 0) == ESP_OK &&
            message.source_type == AUDIO_ELEMENT_TYPE_ELEMENT) {
            if (message.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO && message.source == (void*) impl->decoder) {
                impl->applyMusicInfo();
            } else if (message.cmd == AEL_MSG_CMD_REPORT_STATUS) {
                const auto status = (audio_element_status_t) (intptr_t) message.data;
                auto* element = (audio_element_handle_t) message.source;
                if (status == AEL_STATUS_ERROR_OPEN || status == AEL_STATUS_ERROR_INPUT ||
                    status == AEL_STATUS_ERROR_PROCESS || status == AEL_STATUS_ERROR_OUTPUT ||
                    status == AEL_STATUS_ERROR_CLOSE || status == AEL_STATUS_ERROR_TIMEOUT ||
                    status == AEL_STATUS_ERROR_UNKNOWN) {
                    LOG_E(TAG, "Element %s failed with status %d",
                        audio_element_get_tag(element), (int) status);
                    impl->sourceDrained = true;
                    break;
                }
                LOG_D(TAG, "Element %s status %d", audio_element_get_tag(element), (int) status);
            }
        }

        const int read = raw_stream_read(impl->raw, (char*) chunk, PUMP_CHUNK_BYTES);
        if (read > 0) {
            size_t offset = 0;
            while (offset < (size_t) read && impl->running.load()) {
                offset += xStreamBufferSend(
                    impl->ring, chunk + offset, (size_t) read - offset, pdMS_TO_TICKS(100)
                );
            }
            continue;
        }

        if (read == 0) {
            // End of stream: let the ring drain before the track is reported as finished.
            impl->sourceDrained = true;
            break;
        }

        // A timeout only means no data was ready yet. Treating it as fatal kills this task and
        // leaves playback buffering for ever.
        if (read != AEL_IO_TIMEOUT) {
            LOG_E(TAG, "Pipeline read failed (%d)", read);
            impl->sourceDrained = true;
            break;
        }
    }

    heap_caps_free(chunk);
    impl->pumpTask = nullptr;
    vTaskDelete(nullptr);
}

/** @param count number of samples summed, not bytes */
uint8_t level_from_sum(uint64_t sum_squares, size_t count) {
    if (count == 0) {
        return 0;
    }
    const float rms = sqrtf((float) ((double) sum_squares / (double) count));
    if (rms < 1.0f) {
        return 0;
    }
    // Linear amplitude spends most of its range in the top few dB and reads as a dead meter,
    // so the bar is driven by dBFS across LEVEL_FLOOR_DB..0.
    const float db = 20.0f * log10f(rms / 32768.0f);
    if (db <= LEVEL_FLOOR_DB) {
        return 0;
    }
    return (uint8_t) std::clamp((db - LEVEL_FLOOR_DB) / -LEVEL_FLOOR_DB * 255.0f, 0.0f, 255.0f);
}

// Metered here, not at the decoder: the ring holds seconds of audio, so a level taken where the
// PCM is produced would run that far ahead of what is heard.
/** @return the per-sample coefficient of a one-pole low-pass at @a corner_hz */
float one_pole_coefficient(float corner_hz, uint32_t sample_rate) {
    if (sample_rate == 0) {
        return 1.0f;
    }
    return 1.0f - expf(-2.0f * (float) M_PI * corner_hz / (float) sample_rate);
}

void report_levels(const uint8_t* chunk, size_t bytes, bool stereo, uint32_t sample_rate,
                   float* low_pass_bass, float* low_pass_treble,
                   AudioLevelCallback callback, void* context) {
    const auto* samples = reinterpret_cast<const int16_t*>(chunk);
    const size_t count = bytes / sizeof(int16_t);

    const float bass_coefficient = one_pole_coefficient(BASS_CORNER_HZ, sample_rate);
    const float treble_coefficient = one_pole_coefficient(TREBLE_CORNER_HZ, sample_rate);

    uint64_t sum_left = 0;
    uint64_t sum_right = 0;
    size_t count_left = 0;
    size_t count_right = 0;
    double sum_bass = 0.0;
    double sum_mid = 0.0;
    double sum_treble = 0.0;
    size_t count_mono = 0;

    for (size_t index = 0; index < count; index++) {
        const int32_t sample = samples[index];
        const uint64_t square = (uint64_t) (sample * sample);
        if (stereo && (index & 1u)) {
            sum_right += square;
            count_right++;
        } else {
            sum_left += square;
            count_left++;
        }

        // The band split runs on one channel's worth of samples: doubling the work for a
        // stereo difference that a three-colour mix could not show anyway.
        if (!stereo || (index & 1u) == 0) {
            const float mono = (float) sample;
            *low_pass_bass += bass_coefficient * (mono - *low_pass_bass);
            *low_pass_treble += treble_coefficient * (mono - *low_pass_treble);
            const float bass = *low_pass_bass;
            const float mid = *low_pass_treble - *low_pass_bass;
            const float treble = mono - *low_pass_treble;
            sum_bass += (double) bass * bass;
            sum_mid += (double) mid * mid;
            sum_treble += (double) treble * treble;
            count_mono++;
        }
    }

    AudioLevels levels {};
    levels.left = level_from_sum(sum_left, count_left);
    levels.right = stereo ? level_from_sum(sum_right, count_right) : levels.left;
    levels.bass = level_from_sum((uint64_t) sum_bass, count_mono);
    levels.mid = level_from_sum((uint64_t) sum_mid, count_mono);
    levels.treble = level_from_sum((uint64_t) sum_treble, count_mono);
    callback(levels, context);
}

uint32_t readDmaUnderruns(Device* i2sDevice) {
    uint32_t count = 0;
    if (i2sDevice == nullptr || i2s_controller_get_tx_underrun_count(i2sDevice, &count) != ERROR_NONE) {
        return 0;
    }
    return count;
}

// Holds playback until the ring reaches PREBUFFER_BYTES, then writes to the codec.
void AudioPlayer::Impl::outputTaskMain(void* context) {
    auto* impl = static_cast<Impl*>(context);
    // Deliberately internal, unlike the pump's: every effect walks this buffer sample by sample,
    // and doing that over external RAM is the one place the slower access would be heard.
    auto* chunk = (uint8_t*) heap_caps_malloc(PUMP_CHUNK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (chunk == nullptr) {
        LOG_E(TAG, "Failed to allocate output chunk");
        impl->state = State::Error;
        impl->running = false;
        // Cleared like every other exit from this task: stop() waits on this handle going null.
        impl->outputTask = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    impl->effects.configure(impl->sampleRate.load(), (uint8_t) impl->channels.load());
    impl->effects.startFade(true);

    TickType_t buffering_since = xTaskGetTickCount();

    while (impl->running.load()) {
        if (impl->state.load() == State::Buffering) {
            const size_t available = xStreamBufferBytesAvailable(impl->ring);
            const bool timed_out = (xTaskGetTickCount() - buffering_since) > pdMS_TO_TICKS(BUFFERING_TIMEOUT_MS);

            if (available < PREBUFFER_BYTES && !impl->sourceDrained.load() && !timed_out) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }

            // A source too slow to prefill still plays, but one producing nothing at all is a
            // failure worth reporting rather than an indefinite wait.
            if (timed_out && available == 0) {
                LOG_E(TAG, "No audio produced within %d ms", (int) BUFFERING_TIMEOUT_MS);
                impl->state = State::Error;
                break;
            }
            if (timed_out) {
                LOG_W(TAG, "Starting under-filled at %d bytes", (int) available);
            }

            impl->state = impl->paused.load() ? State::Paused : State::Playing;
        }

        if (impl->paused.load()) {
            // An I2S DMA with nothing to send repeats its last buffer, which buzzes, so a paused
            // stream is fed silence rather than left idle.
            std::memset(chunk, 0, PUMP_CHUNK_BYTES);
            size_t written = 0;
            if (impl->output != nullptr) {
                audio_stream_write(impl->output, chunk, PUMP_CHUNK_BYTES, &written, pdMS_TO_TICKS(200));
            } else {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            continue;
        }

        const uint32_t rate = impl->sampleRate.load();
        const uint32_t channelCount = impl->channels.load();
        if (rate != impl->openedSampleRate.load() || channelCount != impl->openedChannels.load()) {
            // Reopening is how a rate change is handled: the kernel audio stream resamples from
            // whatever rate it is told to the codec's own, so the rate must be the source's.
            if (!impl->openOutput(rate, channelCount)) {
                impl->state = State::Error;
                break;
            }
            // Every stage is built for one format, so it follows a mid-track change too.
            impl->effects.configure(rate, (uint8_t) channelCount);
        }

        // The tail of the track, started while there is still enough left in the ring to run
        // the fade over. Nothing after this point can refill it.
        if (impl->sourceDrained.load() && !impl->fadingOut) {
            const size_t frame_bytes = channelCount * (BITS_PER_SAMPLE / 8);
            if (xStreamBufferBytesAvailable(impl->ring) <= impl->effects.fadeFrameCount() * frame_bytes) {
                impl->effects.startFade(false);
                impl->fadingOut = true;
            }
        }

        // Not while the tail of the track is draining: the ring empties to nothing at the end of
        // every healthy track, which would leave the mark reading zero and meaning nothing.
        if (!impl->sourceDrained.load()) {
            const auto remaining = (uint8_t) (xStreamBufferBytesAvailable(impl->ring) * 100 / RING_BYTES);
            if (remaining < impl->bufferLowPercent.load()) {
                impl->bufferLowPercent = remaining;
            }
        }

        const size_t received = xStreamBufferReceive(impl->ring, chunk, PUMP_CHUNK_BYTES, pdMS_TO_TICKS(100));
        if (received == 0) {
            if (impl->sourceDrained.load()) {
                break;
            }
            impl->underruns++;
            continue;
        }

        impl->effects.process(reinterpret_cast<int16_t*>(chunk),
            received / (channelCount * (BITS_PER_SAMPLE / 8)));

        // Metered after the effects, so what the lights show is what the speaker is doing.
        if (impl->levelCallback != nullptr) {
            report_levels(chunk, received, impl->channels.load() == 2, impl->sampleRate.load(),
                &impl->lowPassBass, &impl->lowPassTreble, impl->levelCallback, impl->levelContext);
        }

        size_t written = 0;
        if (impl->output != nullptr) {
            audio_stream_write(impl->output, chunk, received, &written, pdMS_TO_TICKS(1000));
        }
        impl->bytesPlayed += written;
        // A write that timed out part way leaves the rest of the chunk unplayed. Silently
        // dropping it is what made a stutter look like a clean run.
        if (written < received) {
            impl->droppedBytes += (uint32_t) (received - written);
        }
    }

    if (impl->output != nullptr) {
        std::memset(chunk, 0, PUMP_CHUNK_BYTES);
        size_t written = 0;
        audio_stream_write(impl->output, chunk, PUMP_CHUNK_BYTES, &written, pdMS_TO_TICKS(200));
    }

    impl->effects.release();
    heap_caps_free(chunk);
    if (impl->sourceDrained.load()) {
        impl->state = State::Stopped;
    }
    impl->outputTask = nullptr;
    vTaskDelete(nullptr);
}

AudioPlayer::AudioPlayer() : impl(new Impl()) {
    device_get_first_active_by_type(&AUDIO_STREAM_TYPE, &impl->audioDevice);
    // Optional: without it the hardware underrun count simply reads zero.
    device_get_first_active_by_type(&I2S_CONTROLLER_TYPE, &impl->i2sDevice);
    if (impl->audioDevice == nullptr) {
        LOG_W(TAG, "No audio stream device");
        return;
    }

    impl->ringStorage = (uint8_t*) heap_caps_malloc(RING_BYTES + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (impl->ringStorage == nullptr) {
        LOG_E(TAG, "Failed to allocate %d byte ring buffer", (int) RING_BYTES);
        return;
    }

    // xStreamBufferCreateStatic wants one byte more than the usable capacity.
    impl->ring = xStreamBufferCreateStatic(RING_BYTES, 1, impl->ringStorage, &impl->ringControl);
}

AudioPlayer::~AudioPlayer() {
    stop();
    if (impl->ring != nullptr) {
        vStreamBufferDelete(impl->ring);
    }
    if (impl->ringStorage != nullptr) {
        heap_caps_free(impl->ringStorage);
    }
    if (impl->audioDevice != nullptr) {
        device_put(impl->audioDevice);
    }
    if (impl->i2sDevice != nullptr) {
        device_put(impl->i2sDevice);
    }
    delete impl;
}

bool AudioPlayer::isAvailable() const {
    return impl->audioDevice != nullptr && impl->ring != nullptr;
}

bool AudioPlayer::play(const std::string& path) {
    if (!isAvailable()) {
        return false;
    }

    stop();

    std::lock_guard lock(impl->mutex);

    struct stat file_stat {};
    impl->fileSize = stat(path.c_str(), &file_stat) == 0 ? (uint64_t) file_stat.st_size : 0;
    impl->currentPath = path;
    impl->bytesPlayed = 0;
    impl->underruns = 0;
    impl->droppedBytes = 0;
    impl->bufferLowPercent = 100;
    impl->dmaUnderrunBaseline = readDmaUnderruns(impl->i2sDevice);
    impl->durationSeconds = 0;
    impl->sourceDrained = false;
    impl->paused = false;
    // Not carried across tracks: the filters would start the next one holding the last one's tail.
    impl->lowPassBass = 0.0f;
    impl->lowPassTreble = 0.0f;
    impl->fadingOut = false;
    impl->sampleRate = DEFAULT_SAMPLE_RATE;
    impl->channels = DEFAULT_CHANNELS;

    xStreamBufferReset(impl->ring);

    if (!impl->buildPipeline(path)) {
        impl->teardownPipeline();
        impl->state = State::Error;
        return false;
    }

    if (!impl->openOutput(DEFAULT_SAMPLE_RATE, DEFAULT_CHANNELS)) {
        impl->teardownPipeline();
        impl->state = State::Error;
        return false;
    }

    audio_stream_set_enabled(impl->audioDevice, AUDIO_CODEC_DIR_OUTPUT, true);

    impl->state = State::Buffering;
    impl->running = true;

    if (audio_pipeline_run(impl->pipeline) != ESP_OK) {
        LOG_E(TAG, "Failed to start pipeline");
        impl->running = false;
        impl->teardownPipeline();
        impl->state = State::Error;
        return false;
    }

    // Both stacks come out of internal RAM, which is the resource most likely to be exhausted here.
    // Left unchecked, a failed create leaves running == true with nothing driving the pipeline.
    if (xTaskCreate(Impl::pumpTaskMain, "mp_pump", 4096, impl, 6, &impl->pumpTask) != pdPASS) {
        LOG_E(TAG, "Failed to create pump task");
        impl->pumpTask = nullptr;
        impl->running = false;
        impl->teardownPipeline();
        impl->state = State::Error;
        return false;
    }
    if (xTaskCreate(Impl::outputTaskMain, "mp_out", 4096, impl, 6, &impl->outputTask) != pdPASS) {
        LOG_E(TAG, "Failed to create output task");
        impl->outputTask = nullptr;
        impl->running = false;
        // The pump task is already live and owns its own exit; it clears pumpTask on the way out.
        while (impl->pumpTask != nullptr) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        impl->teardownPipeline();
        impl->state = State::Error;
        return false;
    }

    LOG_I(TAG, "Playing %s", path.c_str());
    return true;
}

void AudioPlayer::stop() {
    if (!impl->running.load() && impl->pipeline == nullptr) {
        return;
    }

    impl->running = false;
    impl->paused = false;

    while (impl->pumpTask != nullptr || impl->outputTask != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    std::lock_guard lock(impl->mutex);

    impl->teardownPipeline();

    if (impl->output != nullptr) {
        audio_stream_close(impl->output);
        impl->output = nullptr;
        impl->openedSampleRate = 0;
    }

    xStreamBufferReset(impl->ring);
    impl->state = State::Stopped;
}

void AudioPlayer::pause() {
    if (impl->state.load() == State::Playing) {
        impl->paused = true;
        impl->state = State::Paused;
    }
}

void AudioPlayer::resume() {
    if (impl->state.load() == State::Paused) {
        impl->paused = false;
        impl->state = State::Playing;
        impl->effects.startFade(true);
    }
}

bool AudioPlayer::seek(uint32_t seconds) {
    const uint32_t bitrate = impl->averageBitrate.load();
    if (bitrate == 0 || impl->currentPath.empty()) {
        return false;
    }

    const auto path = impl->currentPath;
    const uint64_t offset = (uint64_t) seconds * bitrate / 8;
    if (offset >= impl->fileSize) {
        return false;
    }

    // The pipeline has no in-place seek, so the track is restarted at a byte offset.
    if (!play(path)) {
        return false;
    }

    std::lock_guard lock(impl->mutex);
    audio_element_set_byte_pos(impl->reader, (int64_t) offset);
    impl->bytesPlayed = (uint64_t) seconds * impl->sampleRate.load() * impl->channels.load() * (BITS_PER_SAMPLE / 8);
    return true;
}

void AudioPlayer::setGainDb(int gainDb) {
    if (impl->alc != nullptr) {
        alc_volume_setup_set_volume(impl->alc, std::clamp(gainDb, -64, 63));
    }
}

void AudioPlayer::setSpeed(float speed) {
    impl->speed = std::clamp(speed, 0.5f, 2.0f);
    impl->applySonic();
}

void AudioPlayer::setPitch(float pitch) {
    impl->pitch = std::clamp(pitch, 0.5f, 2.0f);
    impl->applySonic();
}

void AudioPlayer::setMonoDownmix(bool enabled) {
    impl->effects.setMonoDownmix(enabled);
}

void AudioPlayer::setDynamics(Dynamics value) {
    impl->effects.setDynamics(value);
}

void AudioPlayer::setBalancePercent(int percent) {
    impl->effects.setBalancePercent(percent);
}

void AudioPlayer::setFade(Fade value) {
    impl->effects.setFade(value);
}

void AudioPlayer::setEqualizerBand(int band, int gainDb) {
    if (band < 0 || band >= EQUALIZER_BANDS) {
        return;
    }

    impl->bandGains[band] = std::clamp(gainDb, -13, 13);
    impl->effects.setBandGains(impl->bandGains);
}

void AudioPlayer::setLevelCallback(AudioLevelCallback callback, void* context) {
    impl->levelCallback = callback;
    impl->levelContext = context;
}

Telemetry AudioPlayer::getTelemetry() const {
    Telemetry telemetry {};
    telemetry.state = impl->state.load();
    telemetry.underruns = impl->underruns.load();
    telemetry.dmaUnderruns = readDmaUnderruns(impl->i2sDevice) - impl->dmaUnderrunBaseline.load();
    telemetry.droppedBytes = impl->droppedBytes.load();
    telemetry.bufferLowPercent = impl->bufferLowPercent.load();
    telemetry.sourceSampleRate = impl->sampleRate.load();
    telemetry.sourceChannels = (uint8_t) impl->channels.load();
    telemetry.durationSeconds = impl->durationSeconds.load();
    telemetry.finished = impl->sourceDrained.load() && impl->state.load() == State::Stopped;
    telemetry.clipping = impl->effects.isClipping();

    if (impl->ring != nullptr) {
        telemetry.bufferPercent = (uint8_t) (xStreamBufferBytesAvailable(impl->ring) * 100 / RING_BYTES);
    }

    const uint32_t bytes_per_second = impl->sampleRate.load() * impl->channels.load() * (BITS_PER_SAMPLE / 8);
    if (bytes_per_second > 0) {
        telemetry.positionSeconds = (uint32_t) (impl->bytesPlayed.load() / bytes_per_second);
    }

    return telemetry;
}

}
