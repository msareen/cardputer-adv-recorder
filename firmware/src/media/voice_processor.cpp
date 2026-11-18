#include "recorder/media/voice_processor.h"

#include <cmath>

namespace cardputer_recorder {
namespace {

constexpr float kPi = 3.14159265F;
constexpr float kFullScale = 32768.0F;
constexpr float kPresenceHz = 3000.0F;

// Speech is brought toward -20 dBFS RMS; peaks then land near -6 dBFS.
constexpr float kTargetRms = 0.1F * kFullScale;
constexpr float kGainRiseSeconds = 1.0F;
// Slow enough that the gain never moves within a syllable (a fast fall
// modulates the waveform and is heard as crackle).
constexpr float kGainFallSeconds = 0.5F;

// A 10 ms block counts as speech when it is this far above the floor.
constexpr float kSpeechOverFloor = 3.16F;            // +10 dB
constexpr float kGateFullyClosedOverFloor = 1.41F;   // +3 dB
constexpr float kSpeechMinRms = 24.0F;               // about -63 dBFS
constexpr float kSpeechLevelMaxRise = 1.07F;         // about 6 dB / 100 ms
constexpr float kSpeechLevelFallFast = 0.03F;
constexpr float kSpeechLevelFallSlow = 0.004F;
constexpr std::uint32_t kSpeechLearningBlocks = 150; // 1.5 s of speech
constexpr float kFloorMin = 2.0F;
constexpr float kFloorMax = 400.0F;                  // about -38 dBFS
constexpr float kFloorSeedMax = 60.0F;               // about -55 dBFS
constexpr float kFloorRisePerBlock = 1.00115F;       // about +1 dB / s
constexpr std::uint32_t kGateHoldBlocks = 30;        // 300 ms
constexpr float kGateOpenSeconds = 0.01F;
constexpr float kGateCloseSeconds = 0.2F;

constexpr float kLimiterThreshold = 28000.0F;        // -1.4 dBFS
constexpr float kLimiterReleaseSeconds = 0.1F;
constexpr std::int32_t kInputClipLevel = 32000;
constexpr float kWarmupSeconds = 0.2F;

float onePoleStep(float seconds, float sampleRate)
{
    return 1.0F - std::exp(-1.0F / (seconds * sampleRate));
}

float fromDb(float db)
{
    return std::pow(10.0F, db / 20.0F);
}

float toDb(float ratio)
{
    return ratio > 0.0F ? 20.0F * std::log10(ratio) : -120.0F;
}

}  // namespace

void VoiceProcessor::reset(std::uint32_t sampleRate, const Config& config)
{
    const float rate = sampleRate > 0 ? static_cast<float>(sampleRate)
                                      : 16000.0F;

    // RBJ cookbook biquads.
    highPassOn_ = config.highPassHz > 0.0F;
    highPass_ = Biquad();
    if (highPassOn_) {
        const float w0 = 2.0F * kPi * config.highPassHz / rate;
        const float cosw = std::cos(w0);
        const float alpha = std::sin(w0) / (2.0F * 0.7071F);
        const float a0 = 1.0F + alpha;
        highPass_.b0 = (1.0F + cosw) / 2.0F / a0;
        highPass_.b1 = -(1.0F + cosw) / a0;
        highPass_.b2 = (1.0F + cosw) / 2.0F / a0;
        highPass_.a1 = -2.0F * cosw / a0;
        highPass_.a2 = (1.0F - alpha) / a0;
    }
    presenceOn_ = config.presenceDb != 0.0F;
    presence_ = Biquad();
    if (presenceOn_) {
        const float amp = std::pow(10.0F, config.presenceDb / 40.0F);
        const float w0 = 2.0F * kPi * kPresenceHz / rate;
        const float cosw = std::cos(w0);
        const float alpha = std::sin(w0) / 2.0F * std::sqrt(2.0F);
        const float beta = 2.0F * std::sqrt(amp) * alpha;
        const float a0 = (amp + 1.0F) - (amp - 1.0F) * cosw + beta;
        presence_.b0 =
            amp * ((amp + 1.0F) + (amp - 1.0F) * cosw + beta) / a0;
        presence_.b1 =
            -2.0F * amp * ((amp - 1.0F) + (amp + 1.0F) * cosw) / a0;
        presence_.b2 =
            amp * ((amp + 1.0F) + (amp - 1.0F) * cosw - beta) / a0;
        presence_.a1 = 2.0F * ((amp - 1.0F) - (amp + 1.0F) * cosw) / a0;
        presence_.a2 = ((amp + 1.0F) - (amp - 1.0F) * cosw - beta) / a0;
    }

    maxGain_ = config.maxGainDb > 0.0F ? fromDb(config.maxGainDb) : 1.0F;
    minGateGain_ =
        config.expanderDb > 0.0F ? fromDb(-config.expanderDb) : 1.0F;

    blockSize_ = static_cast<std::size_t>(rate / 100.0F);  // 10 ms
    if (blockSize_ == 0) {
        blockSize_ = 1;
    }
    blockFill_ = 0;
    blockEnergy_ = 0.0F;
    warmupLength_ = static_cast<std::uint32_t>(kWarmupSeconds * rate);
    warmupLeft_ = warmupLength_;
    noiseFloor_ = 0.0F;
    floorSeeded_ = false;
    speechBlocks_ = 0;
    blocksSinceSpeech_ = kGateHoldBlocks;
    // Start halfway (in dB) to the ceiling until speech is measured.
    gain_ = std::sqrt(maxGain_);
    gainTarget_ = gain_;
    speechLevel_ = kTargetRms / gain_;
    // The gate starts open so the first word is never cut.
    gateGain_ = 1.0F;
    gateTarget_ = 1.0F;
    limiterGain_ = 1.0F;
    gainRiseStep_ = onePoleStep(kGainRiseSeconds, rate);
    gainFallStep_ = onePoleStep(kGainFallSeconds, rate);
    gateOpenStep_ = onePoleStep(kGateOpenSeconds, rate);
    gateCloseStep_ = onePoleStep(kGateCloseSeconds, rate);
    limiterReleaseStep_ = onePoleStep(kLimiterReleaseSeconds, rate);
    clippedInputSamples_ = 0;
    limitedSamples_ = 0;
}

void VoiceProcessor::process(std::int16_t* samples, std::size_t count)
{
    for (std::size_t index = 0; index < count; ++index) {
        const std::int32_t raw = samples[index];
        float value = static_cast<float>(raw);
        if (highPassOn_) {
            value = highPass_.run(value);
        }
        if (presenceOn_) {
            value = presence_.run(value);
        }

        if (warmupLeft_ > 0) {
            --warmupLeft_;
            value *= 1.0F - static_cast<float>(warmupLeft_) /
                                static_cast<float>(warmupLength_);
        } else {
            if (raw >= kInputClipLevel || raw <= -kInputClipLevel) {
                ++clippedInputSamples_;
            }
            blockEnergy_ += value * value;
            if (++blockFill_ >= blockSize_) {
                finishBlock();
            }
        }

        gain_ += (gainTarget_ - gain_) *
                 (gainTarget_ > gain_ ? gainRiseStep_ : gainFallStep_);
        gateGain_ += (gateTarget_ - gateGain_) *
                     (gateTarget_ > gateGain_ ? gateOpenStep_
                                              : gateCloseStep_);
        value *= gain_ * gateGain_;

        // Release first, then clamp, so the released gain can never let
        // this sample through above the threshold.
        const float magnitude = std::fabs(value);
        limiterGain_ += (1.0F - limiterGain_) * limiterReleaseStep_;
        if (magnitude * limiterGain_ > kLimiterThreshold) {
            limiterGain_ = kLimiterThreshold / magnitude;
            ++limitedSamples_;
        }
        value *= limiterGain_;

        std::int32_t rounded = static_cast<std::int32_t>(
            value >= 0.0F ? value + 0.5F : value - 0.5F);
        if (rounded > 32767) {
            rounded = 32767;
        } else if (rounded < -32767) {
            rounded = -32767;
        }
        samples[index] = static_cast<std::int16_t>(rounded);
    }
}

void VoiceProcessor::finishBlock()
{
    const float rms =
        std::sqrt(blockEnergy_ / static_cast<float>(blockFill_));
    blockEnergy_ = 0.0F;
    blockFill_ = 0;

    // Noise floor: follows quiet blocks down at once, creeps up slowly, so
    // speech (which always has gaps) does not drag it up but a noisier
    // room, or any sound that never pauses, is learned within seconds.
    if (!floorSeeded_) {
        // A recording may start mid-sentence: do not take a first block
        // that loud for room noise.
        noiseFloor_ = rms < kFloorSeedMax ? rms : kFloorSeedMax;
        floorSeeded_ = true;
    } else if (rms < noiseFloor_) {
        noiseFloor_ += (rms - noiseFloor_) * 0.3F;
    } else {
        noiseFloor_ *= kFloorRisePerBlock;
    }
    if (noiseFloor_ < kFloorMin) {
        noiseFloor_ = kFloorMin;
    } else if (noiseFloor_ > kFloorMax) {
        noiseFloor_ = kFloorMax;
    }

    const float overFloor = rms / noiseFloor_;
    const bool speech =
        overFloor >= kSpeechOverFloor && rms >= kSpeechMinRms;

    if (speech) {
        blocksSinceSpeech_ = 0;
        // Loudness of the talker. Rises are capped, so a key click or a
        // bump is over before it can pull the gain down; quieter speech
        // is followed quickly at first, then slowly.
        if (rms > speechLevel_) {
            const float capped = speechLevel_ * kSpeechLevelMaxRise;
            speechLevel_ = rms < capped ? rms : capped;
        } else {
            speechLevel_ += (rms - speechLevel_) *
                            (speechBlocks_ < kSpeechLearningBlocks
                                 ? kSpeechLevelFallFast
                                 : kSpeechLevelFallSlow);
        }
        if (speechBlocks_ < kSpeechLearningBlocks) {
            ++speechBlocks_;
        }
        float wanted = kTargetRms / speechLevel_;
        if (wanted < 1.0F) {
            wanted = 1.0F;
        } else if (wanted > maxGain_) {
            wanted = maxGain_;
        }
        gainTarget_ = wanted;
        gateTarget_ = 1.0F;
        return;
    }

    // Not speech: the gain is frozen (room noise is not pumped up) and the
    // expander closes after the hold time, further the nearer the floor.
    if (blocksSinceSpeech_ < kGateHoldBlocks) {
        ++blocksSinceSpeech_;
        gateTarget_ = 1.0F;
        return;
    }
    if (overFloor <= kGateFullyClosedOverFloor) {
        gateTarget_ = minGateGain_;
    } else {
        const float position =
            std::log(overFloor / kGateFullyClosedOverFloor) /
            std::log(kSpeechOverFloor / kGateFullyClosedOverFloor);
        gateTarget_ = minGateGain_ + (1.0F - minGateGain_) * position;
    }
}

VoiceProcessor::Stats VoiceProcessor::stats() const
{
    Stats result;
    result.gainDb = toDb(gain_);
    result.noiseFloorDbfs = toDb(noiseFloor_ / kFullScale);
    result.clippedInputSamples = clippedInputSamples_;
    result.limitedSamples = limitedSamples_;
    return result;
}

}  // namespace cardputer_recorder
