#include <unity.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "recorder/media/voice_processor.h"

using namespace cardputer_recorder;

namespace {

constexpr std::uint32_t kRate = 16000;
constexpr double kPi = 3.14159265358979;

// The firmware's Voice preset (app/recording_flow.cpp), and one without
// the expander.
const VoiceProcessor::Config kVoice = {100.0F, 3.0F, 9.0F, 8.0F};
const VoiceProcessor::Config kNoExpander = {80.0F, 0.0F, 9.0F, 0.0F};
const VoiceProcessor::Config kAllOff = {0.0F, 0.0F, 0.0F, 0.0F};

// Deterministic noise, uniform in [-amplitude, amplitude].
struct Noise {
    std::uint32_t state = 12345;
    double next(double amplitude)
    {
        state = state * 1664525U + 1013904223U;
        return (static_cast<double>(state >> 8) / 8388608.0 - 1.0) *
               amplitude;
    }
};

std::vector<std::int16_t> tone(double hz, double amplitude, double seconds,
                               double noiseAmplitude = 0.0)
{
    Noise noise;
    std::vector<std::int16_t> out(
        static_cast<std::size_t>(seconds * kRate));
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::int16_t>(
            amplitude * std::sin(2.0 * kPi * hz * i / kRate) +
            noise.next(noiseAmplitude));
    }
    return out;
}

// Speech-like: 200 ms bursts of the tone with 80 ms gaps, over room noise.
std::vector<std::int16_t> speech(double hz, double amplitude, double seconds,
                                 double noiseAmplitude)
{
    Noise noise;
    std::vector<std::int16_t> out(
        static_cast<std::size_t>(seconds * kRate));
    const std::size_t period = kRate * 280 / 1000;
    const std::size_t on = kRate * 200 / 1000;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const double voiced = i % period < on ? amplitude : 0.0;
        out[i] = static_cast<std::int16_t>(
            voiced * std::sin(2.0 * kPi * hz * i / kRate) +
            noise.next(noiseAmplitude));
    }
    return out;
}

double rms(const std::vector<std::int16_t>& samples, double fromSeconds,
           double toSeconds)
{
    const std::size_t from = static_cast<std::size_t>(fromSeconds * kRate);
    const std::size_t to = static_cast<std::size_t>(toSeconds * kRate);
    double total = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        total += static_cast<double>(samples[i]) * samples[i];
    }
    return std::sqrt(total / static_cast<double>(to - from));
}

int peak(const std::vector<std::int16_t>& samples)
{
    int result = 0;
    for (std::int16_t sample : samples) {
        const int magnitude = sample < 0 ? -sample : sample;
        if (magnitude > result) {
            result = magnitude;
        }
    }
    return result;
}

// Feeds the processor in capture-sized buffers, as the writer task does.
void run(VoiceProcessor& processor, std::vector<std::int16_t>& samples)
{
    for (std::size_t offset = 0; offset < samples.size(); offset += 4096) {
        const std::size_t count = samples.size() - offset < 4096
                                      ? samples.size() - offset
                                      : 4096;
        processor.process(samples.data() + offset, count);
    }
}

void append(std::vector<std::int16_t>& to,
            const std::vector<std::int16_t>& from)
{
    to.insert(to.end(), from.begin(), from.end());
}

}  // namespace

void test_moderate_speech_is_brought_to_target()
{
    // Bursts at about -28 dBFS RMS: within the Voice preset's 9 dB reach.
    std::vector<std::int16_t> samples = speech(1000.0, 1800.0, 8.0, 6.0);
    VoiceProcessor processor;
    processor.reset(kRate, kVoice);
    run(processor, samples);
    // The bursts go to -20 dBFS (3277); with the gaps the average is about
    // 1.5 dB lower. Allow 2.5 dB either way.
    const double level = rms(samples, 5.2, 8.0);
    TEST_ASSERT_TRUE(level > 2050.0);
    TEST_ASSERT_TRUE(level < 3700.0);
}

void test_gain_stops_at_the_preset_ceiling()
{
    // Very quiet speech (about -46 dBFS): wants 26 dB, Voice allows 9.
    std::vector<std::int16_t> samples = speech(1000.0, 230.0, 8.0, 2.0);
    VoiceProcessor processor;
    processor.reset(kRate, kVoice);
    run(processor, samples);
    const float gainDb = processor.stats().gainDb;
    TEST_ASSERT_FLOAT_WITHIN(0.5F, 9.0F, gainDb);
}

void test_loud_input_never_exceeds_limiter()
{
    std::vector<std::int16_t> samples = tone(700.0, 30000.0, 3.0);
    VoiceProcessor processor;
    processor.reset(kRate, kVoice);
    run(processor, samples);
    TEST_ASSERT_TRUE(peak(samples) <= 28001);
}

void test_rumble_is_removed()
{
    // 30 Hz at the same amplitude as the 1 kHz reference.
    std::vector<std::int16_t> rumble = tone(30.0, 2000.0, 4.0);
    std::vector<std::int16_t> voice = tone(1000.0, 2000.0, 4.0);
    VoiceProcessor first;
    first.reset(kRate, kVoice);
    run(first, rumble);
    VoiceProcessor second;
    second.reset(kRate, kVoice);
    run(second, voice);
    TEST_ASSERT_TRUE(rms(rumble, 3.0, 4.0) * 10.0 < rms(voice, 3.0, 4.0));
}

void test_voice_preset_turns_pauses_down()
{
    // Speech, two seconds of room noise only, speech again.
    std::vector<std::int16_t> samples = speech(800.0, 1500.0, 3.08, 12.0);
    append(samples, tone(800.0, 0.0, 2.0, 12.0));
    append(samples, speech(800.0, 1500.0, 1.0, 12.0));
    const std::vector<std::int16_t> original = samples;

    VoiceProcessor processor;
    processor.reset(kRate, kVoice);
    run(processor, samples);

    const double speechGain = rms(samples, 2.0, 3.0) / rms(original, 2.0, 3.0);
    const double pauseGain = rms(samples, 4.2, 5.0) / rms(original, 4.2, 5.0);
    // 8 dB less, minus what the presence shelf adds to white noise.
    TEST_ASSERT_TRUE(pauseGain < speechGain * 0.6);
    // Speech after the pause is back at once.
    const double afterGain = rms(samples, 5.2, 6.0) / rms(original, 5.2, 6.0);
    TEST_ASSERT_TRUE(afterGain > speechGain * 0.8);
}

void test_without_expander_pauses_are_left_alone()
{
    std::vector<std::int16_t> samples = speech(800.0, 1500.0, 3.08, 12.0);
    append(samples, tone(800.0, 0.0, 2.0, 12.0));
    const std::vector<std::int16_t> original = samples;

    VoiceProcessor processor;
    processor.reset(kRate, kNoExpander);
    run(processor, samples);

    const double speechGain = rms(samples, 2.0, 3.0) / rms(original, 2.0, 3.0);
    const double pauseGain = rms(samples, 4.2, 5.0) / rms(original, 4.2, 5.0);
    // No expander, and the gain is frozen (not pumped up) in the pause.
    TEST_ASSERT_TRUE(pauseGain > speechGain * 0.8);
    TEST_ASSERT_TRUE(pauseGain < speechGain * 1.15);
}

void test_start_click_does_not_duck_the_recording()
{
    // Full-scale burst in the first 100 ms (codec settling, the Enter key),
    // then moderate speech.
    std::vector<std::int16_t> samples = tone(300.0, 32767.0, 0.1);
    append(samples, speech(1000.0, 1800.0, 6.0, 6.0));
    VoiceProcessor processor;
    processor.reset(kRate, kVoice);
    run(processor, samples);
    TEST_ASSERT_EQUAL_UINT32(0, processor.stats().clippedInputSamples);
    TEST_ASSERT_TRUE(rms(samples, 3.3, 6.1) > 2050.0);
}

void test_clipped_input_is_counted()
{
    std::vector<std::int16_t> samples = tone(500.0, 1000.0, 1.0);
    for (std::int16_t& sample : samples) {
        sample = sample >= 0 ? 32767 : -32767;
    }
    VoiceProcessor processor;
    processor.reset(kRate, kVoice);
    run(processor, samples);
    TEST_ASSERT_TRUE(processor.stats().clippedInputSamples > 1000);
}

void test_all_stages_off_passes_samples_through()
{
    const std::vector<std::int16_t> original =
        tone(440.0, 9000.0, 1.0, 50.0);
    std::vector<std::int16_t> samples = original;
    VoiceProcessor processor;
    processor.reset(kRate, kAllOff);
    run(processor, samples);
    // After the 200 ms fade-in, unchanged.
    for (std::size_t i = kRate / 5; i < samples.size(); ++i) {
        TEST_ASSERT_EQUAL_INT16(original[i], samples[i]);
    }
}

void test_silence_stays_silent()
{
    std::vector<std::int16_t> samples(kRate * 2, 0);
    VoiceProcessor processor;
    processor.reset(kRate, kVoice);
    run(processor, samples);
    TEST_ASSERT_EQUAL_INT(0, peak(samples));
}

void setUp() {}
void tearDown() {}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_moderate_speech_is_brought_to_target);
    RUN_TEST(test_gain_stops_at_the_preset_ceiling);
    RUN_TEST(test_loud_input_never_exceeds_limiter);
    RUN_TEST(test_rumble_is_removed);
    RUN_TEST(test_voice_preset_turns_pauses_down);
    RUN_TEST(test_without_expander_pauses_are_left_alone);
    RUN_TEST(test_start_click_does_not_duck_the_recording);
    RUN_TEST(test_clipped_input_is_counted);
    RUN_TEST(test_all_stages_off_passes_samples_through);
    RUN_TEST(test_silence_stays_silent);
    return UNITY_END();
}
