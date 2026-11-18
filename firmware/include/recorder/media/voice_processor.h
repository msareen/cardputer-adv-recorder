#pragma once

#include <cstddef>
#include <cstdint>

namespace cardputer_recorder {

// Optional clean-up of captured PCM before it is written to the WAV, used
// by the Voice mic mode (Natural writes the samples untouched).
// Pure arithmetic (no Arduino), so it is host-tested.
//
// Chain, per sample: high-pass (rumble, handling thumps) -> high shelf
// (presence) -> automatic gain that adapts only on speech -> downward
// expander for the pauses -> peak limiter. Each stage can be off.
class VoiceProcessor {
public:
    // No default member initializers: the device build is gnu++11, where
    // they would forbid brace-initializing presets.
    struct Config {
        float highPassHz;   // 0: off
        float presenceDb;   // shelf above 3 kHz; 0: off
        float maxGainDb;    // automatic gain ceiling; 0: fixed unity gain
        float expanderDb;   // how far pauses are turned down; 0: off
    };

    struct Stats {
        float gainDb = 0.0F;
        float noiseFloorDbfs = -96.0F;
        std::uint32_t clippedInputSamples = 0;
        std::uint32_t limitedSamples = 0;
    };

    void reset(std::uint32_t sampleRate, const Config& config);

    // In place. The first 200 ms after reset() are faded in and not
    // measured (codec settling, the key that started the recording).
    void process(std::int16_t* samples, std::size_t count);

    Stats stats() const;

private:
    struct Biquad {
        float b0 = 1.0F;
        float b1 = 0.0F;
        float b2 = 0.0F;
        float a1 = 0.0F;
        float a2 = 0.0F;
        float z1 = 0.0F;
        float z2 = 0.0F;

        float run(float input)
        {
            const float output = b0 * input + z1;
            z1 = b1 * input - a1 * output + z2;
            z2 = b2 * input - a2 * output;
            return output;
        }
    };

    void finishBlock();

    Biquad highPass_;
    Biquad presence_;
    bool highPassOn_ = false;
    bool presenceOn_ = false;
    float maxGain_ = 1.0F;
    float minGateGain_ = 1.0F;
    std::size_t blockSize_ = 160;
    std::size_t blockFill_ = 0;
    float blockEnergy_ = 0.0F;
    std::uint32_t warmupLength_ = 0;
    std::uint32_t warmupLeft_ = 0;
    float noiseFloor_ = 0.0F;
    float speechLevel_ = 0.0F;
    bool floorSeeded_ = false;
    std::uint32_t blocksSinceSpeech_ = 0;
    std::uint32_t speechBlocks_ = 0;
    float gain_ = 1.0F;
    float gainTarget_ = 1.0F;
    float gateGain_ = 1.0F;
    float gateTarget_ = 1.0F;
    float limiterGain_ = 1.0F;
    float gainRiseStep_ = 0.0F;
    float gainFallStep_ = 0.0F;
    float gateOpenStep_ = 0.0F;
    float gateCloseStep_ = 0.0F;
    float limiterReleaseStep_ = 0.0F;
    std::uint32_t clippedInputSamples_ = 0;
    std::uint32_t limitedSamples_ = 0;
};

}  // namespace cardputer_recorder
