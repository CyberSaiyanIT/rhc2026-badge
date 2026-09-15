#include <Tactility/service/music/AudioEffects.h>

#include <tactility/log.h>

#include <esp_ae_drc.h>
#include <esp_ae_eq.h>
#include <esp_ae_fade.h>
#include <esp_ae_mbc.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tt::service::music {

namespace {

constexpr auto* TAG = "AudioEffects";

constexpr uint8_t BITS_PER_SAMPLE = 16;

// One octave between neighbouring bands, which is the bandwidth a ten-band graphic equalizer
// covers without leaving holes between the filters or overlapping them into mush.
constexpr float BAND_Q = 1.4f;

// Full scale is 32767. A sample this close to it has either clipped already or is about to.
constexpr int32_t CLIP_THRESHOLD = 32700;

// Long enough for a single overloaded chunk to survive until a reader that polls a few times a
// second can see it.
constexpr uint32_t CLIP_HOLD_MS = 2000;

// Short enough not to be heard as a swell on a track change, long enough to remove the click.
constexpr uint32_t FADE_SHORT_MS = 150;
// Long enough to read as one track giving way to the next rather than stopping.
constexpr uint32_t FADE_LONG_MS = 600;

uint32_t fade_duration_ms(Fade value) {
    switch (value) {
        case Fade::Short: return FADE_SHORT_MS;
        case Fade::Long: return FADE_LONG_MS;
        default: return 0;
    }
}

// A gentle 1.7:1 above -30 dBFS with the loss made back up, which is what lets a quiet passage
// stay audible over the room without the loud one distorting the badge's speaker.
esp_ae_drc_curve_point COMPRESSOR_CURVE[] = {
    { .x = 0.0f, .y = -12.0f },
    { .x = -30.0f, .y = -30.0f },
    { .x = -100.0f, .y = -100.0f },
};

// Crossovers picked for what the badge's speaker does rather than for a studio: everything the
// cone cannot move in the first band, the body of the mix in the second, presence in the third.
constexpr uint32_t MULTIBAND_CROSSOVERS[] = { 200, 1000, 4000 };

// The lowest band takes the hardest ratio: it is the one that runs the amplifier out of headroom
// without ever becoming audible on a speaker this size.
constexpr esp_ae_mbc_para_t MULTIBAND_BANDS[] = {
    { .threshold = -24.0f, .ratio = 4.0f, .makeup_gain = 0.0f, .attack_time = 10, .release_time = 150, .hold_time = 0, .knee_width = 6.0f },
    { .threshold = -20.0f, .ratio = 2.0f, .makeup_gain = 2.0f, .attack_time = 10, .release_time = 150, .hold_time = 0, .knee_width = 6.0f },
    { .threshold = -18.0f, .ratio = 2.0f, .makeup_gain = 3.0f, .attack_time = 5, .release_time = 120, .hold_time = 0, .knee_width = 6.0f },
    { .threshold = -20.0f, .ratio = 2.0f, .makeup_gain = 2.0f, .attack_time = 5, .release_time = 120, .hold_time = 0, .knee_width = 6.0f },
};

} // namespace

AudioEffects::AudioEffects() = default;

AudioEffects::~AudioEffects() {
    closeStages();
}

void AudioEffects::configure(uint32_t rate, uint8_t channelCount) {
    if (rate == sampleRate && channelCount == channels) {
        return;
    }
    closeStages();
    sampleRate = rate;
    channels = channelCount;

    // Taken before the filters are built rather than left to the first process(): opening with a
    // flat curve would disable every filter and start the track with the equalizer doing nothing.
    const uint32_t generation = gainGeneration.load();
    std::memcpy(bandGains, pendingGains, sizeof(bandGains));
    appliedGainGeneration = generation;

    // A peak filter above Nyquist has no meaning, so the top bands simply do not exist at low
    // sample rates. Band numbering is unaffected: the ones that survive are the leading ones.
    filterCount = 0;
    while (filterCount < EQUALIZER_BANDS &&
           EQUALIZER_BAND_FREQUENCIES[filterCount] * 2u < sampleRate) {
        filterCount++;
    }

    openEqualizer();
    openCompressor();
    openMultiband();
    openFade();
}

void AudioEffects::release() {
    closeStages();
    sampleRate = 0;
    channels = 0;
    // Only the processing decays this, so leaving it set would carry a warning into a track that
    // has not played a sample yet.
    clipHoldFrames = 0;
}

void AudioEffects::openEqualizer() {
    if (filterCount == 0) {
        return;
    }

    esp_ae_eq_filter_para_t filters[EQUALIZER_BANDS] = {};
    for (int band = 0; band < filterCount; band++) {
        filters[band].filter_type = ESP_AE_EQ_FILTER_PEAK;
        filters[band].fc = EQUALIZER_BAND_FREQUENCIES[band];
        filters[band].q = BAND_Q;
        filters[band].gain = (float) bandGains[band];
    }

    esp_ae_eq_cfg_t config {
        .sample_rate = sampleRate,
        .channel = channels,
        .bits_per_sample = BITS_PER_SAMPLE,
        .filter_num = (uint8_t) filterCount,
        .para = filters
    };

    if (esp_ae_eq_open(&config, &equalizer) != ESP_AE_ERR_OK) {
        LOG_E(TAG, "Failed to open the equalizer");
        equalizer = nullptr;
        return;
    }

    // A flat band still costs a biquad per channel per sample if it is left running.
    for (int band = 0; band < filterCount; band++) {
        if (bandGains[band] == 0) {
            esp_ae_eq_disable_filter(equalizer, (uint8_t) band);
        } else {
            esp_ae_eq_enable_filter(equalizer, (uint8_t) band);
        }
    }
}

void AudioEffects::openCompressor() {
    if ((Dynamics) appliedDynamics != Dynamics::Compressor) {
        return;
    }

    esp_ae_drc_cfg_t config {
        .sample_rate = sampleRate,
        .channel = channels,
        .bits_per_sample = BITS_PER_SAMPLE,
        .drc_para = {
            .point = COMPRESSOR_CURVE,
            .point_num = (uint8_t) (sizeof(COMPRESSOR_CURVE) / sizeof(COMPRESSOR_CURVE[0])),
            .makeup_gain = 6.0f,
            .knee_width = 6.0f,
            .attack_time = 10,
            .release_time = 150,
            .hold_time = 0
        }
    };

    if (esp_ae_drc_open(&config, &compressor) != ESP_AE_ERR_OK) {
        LOG_E(TAG, "Failed to open the compressor");
        compressor = nullptr;
    }
}

void AudioEffects::openMultiband() {
    if ((Dynamics) appliedDynamics != Dynamics::Multiband) {
        return;
    }

    esp_ae_mbc_config_t config {
        .sample_rate = sampleRate,
        .channel = channels,
        .bits_per_sample = BITS_PER_SAMPLE,
        .fc = { MULTIBAND_CROSSOVERS[0], MULTIBAND_CROSSOVERS[1], MULTIBAND_CROSSOVERS[2] },
        .mbc_para = { MULTIBAND_BANDS[0], MULTIBAND_BANDS[1], MULTIBAND_BANDS[2], MULTIBAND_BANDS[3] }
    };

    if (esp_ae_mbc_open(&config, &multiband) != ESP_AE_ERR_OK) {
        LOG_E(TAG, "Failed to open the multiband compressor");
        multiband = nullptr;
    }
}

void AudioEffects::openFade() {
    const uint32_t duration = fade_duration_ms((Fade) appliedFade);
    if (duration == 0) {
        return;
    }

    esp_ae_fade_cfg_t config {
        .mode = ESP_AE_FADE_MODE_FADE_IN,
        .curve = ESP_AE_FADE_CURVE_QUAD,
        .transit_time = duration,
        .sample_rate = sampleRate,
        .channel = channels,
        .bits_per_sample = BITS_PER_SAMPLE
    };

    if (esp_ae_fade_open(&config, &fader) != ESP_AE_ERR_OK) {
        LOG_E(TAG, "Failed to open the fader");
        fader = nullptr;
    }
}

void AudioEffects::closeStages() {
    if (equalizer != nullptr) {
        esp_ae_eq_close(equalizer);
        equalizer = nullptr;
    }
    if (compressor != nullptr) {
        esp_ae_drc_close(compressor);
        compressor = nullptr;
    }
    if (multiband != nullptr) {
        esp_ae_mbc_close(multiband);
        multiband = nullptr;
    }
    if (fader != nullptr) {
        esp_ae_fade_close(fader);
        fader = nullptr;
    }
    fadeFramesLeft = 0;
}

void AudioEffects::applyPending() {
    const uint32_t generation = gainGeneration.load();
    if (generation != appliedGainGeneration) {
        std::memcpy(bandGains, pendingGains, sizeof(bandGains));
        // Anything that landed while the copy was running raises the generation again, so this
        // records the value read before it rather than after and picks the rest up next chunk.
        appliedGainGeneration = generation;
        if (equalizer != nullptr) {
            for (int band = 0; band < filterCount; band++) {
                esp_ae_eq_filter_para_t filter {
                    .filter_type = ESP_AE_EQ_FILTER_PEAK,
                    .fc = EQUALIZER_BAND_FREQUENCIES[band],
                    .q = BAND_Q,
                    .gain = (float) bandGains[band]
                };
                esp_ae_eq_set_filter_para(equalizer, (uint8_t) band, &filter);
                if (bandGains[band] == 0) {
                    esp_ae_eq_disable_filter(equalizer, (uint8_t) band);
                } else {
                    esp_ae_eq_enable_filter(equalizer, (uint8_t) band);
                }
            }
        }
    }

    const uint8_t wantedDynamics = dynamics.load();
    if (wantedDynamics != appliedDynamics) {
        appliedDynamics = wantedDynamics;
        if (compressor != nullptr) {
            esp_ae_drc_close(compressor);
            compressor = nullptr;
        }
        if (multiband != nullptr) {
            esp_ae_mbc_close(multiband);
            multiband = nullptr;
        }
        if (sampleRate != 0) {
            openCompressor();
            openMultiband();
        }
    }

    const uint8_t wantedFade = fade.load();
    if (wantedFade != appliedFade) {
        appliedFade = wantedFade;
        if (fader != nullptr) {
            esp_ae_fade_close(fader);
            fader = nullptr;
        }
        fadeFramesLeft = 0;
        if (sampleRate != 0) {
            openFade();
        }
    }
}

void AudioEffects::mixAndMeasure(int16_t* samples, size_t frames) {
    const int balance = balancePercent.load();
    const bool mono = monoDownmix.load();
    // 8-bit fixed point, so a centred balance costs a comparison rather than a multiply.
    const int32_t leftScale = balance > 0 ? 256 - balance * 256 / 100 : 256;
    const int32_t rightScale = balance < 0 ? 256 + balance * 256 / 100 : 256;
    const bool panned = leftScale != 256 || rightScale != 256;

    int32_t peak = 0;

    if (channels == 2) {
        for (size_t frame = 0; frame < frames; frame++) {
            int32_t left = samples[frame * 2];
            int32_t right = samples[frame * 2 + 1];
            if (mono) {
                left = right = (left + right) / 2;
            }
            if (panned) {
                left = left * leftScale / 256;
                right = right * rightScale / 256;
            }
            samples[frame * 2] = (int16_t) left;
            samples[frame * 2 + 1] = (int16_t) right;
            peak = std::max({ peak, std::abs(left), std::abs(right) });
        }
    } else {
        for (size_t frame = 0; frame < frames; frame++) {
            peak = std::max(peak, std::abs((int32_t) samples[frame]));
        }
    }

    if (peak >= CLIP_THRESHOLD) {
        clipHoldFrames = CLIP_HOLD_MS * sampleRate / 1000;
    } else {
        const uint32_t held = clipHoldFrames.load();
        clipHoldFrames = held > frames ? held - (uint32_t) frames : 0;
    }
}

void AudioEffects::process(int16_t* samples, size_t frames) {
    applyPending();

    if (equalizer != nullptr) {
        esp_ae_eq_process(equalizer, (uint32_t) frames, samples, samples);
    }
    if (compressor != nullptr) {
        esp_ae_drc_process(compressor, (uint32_t) frames, samples, samples);
    }
    if (multiband != nullptr) {
        esp_ae_mbc_process(multiband, (uint32_t) frames, samples, samples);
    }

    mixAndMeasure(samples, frames);

    const uint8_t request = fadeRequest.exchange(0);
    if (request != 0 && fader != nullptr) {
        esp_ae_fade_set_mode(fader, request == 1 ? ESP_AE_FADE_MODE_FADE_IN : ESP_AE_FADE_MODE_FADE_OUT);
        esp_ae_fade_reset_weight(fader);
        fadeFramesLeft = fadeFrameCount();
    }
    if (fadeFramesLeft > 0 && fader != nullptr) {
        esp_ae_fade_process(fader, (uint32_t) frames, samples, samples);
        fadeFramesLeft -= std::min(fadeFramesLeft, frames);
    }
}

void AudioEffects::setBandGains(const int* gains) {
    std::memcpy(pendingGains, gains, sizeof(pendingGains));
    gainGeneration++;
}

void AudioEffects::setDynamics(Dynamics value) {
    dynamics = (uint8_t) value;
}

void AudioEffects::setBalancePercent(int percent) {
    balancePercent = (int8_t) std::clamp(percent, -100, 100);
}

void AudioEffects::setMonoDownmix(bool enabled) {
    monoDownmix = enabled;
}

void AudioEffects::setFade(Fade value) {
    fade = (uint8_t) value;
}

size_t AudioEffects::fadeFrameCount() const {
    return (size_t) fade_duration_ms((Fade) appliedFade) * sampleRate / 1000;
}

void AudioEffects::startFade(bool fadeIn) {
    fadeRequest = fadeIn ? 1 : 2;
}

bool AudioEffects::isClipping() const {
    return clipHoldFrames.load() > 0;
}

} // namespace tt::service::music
