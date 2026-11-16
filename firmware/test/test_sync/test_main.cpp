#include <unity.h>

#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "recorder/media/recording_chunk.h"
#include "recorder/media/wav_format.h"
#include "recorder/sync/protocol.h"
#include "recorder/sync/sha256.h"
#include "recorder/sync/sync_index.h"

using namespace cardputer_recorder;

namespace {

RecordingIdentity sampleIdentity()
{
    RecordingIdentity identity;
    for (int i = 0; i < 16; ++i) {
        identity.recordingId[i] = static_cast<std::uint8_t>(0xA0 + i);
    }
    for (int i = 0; i < 8; ++i) {
        identity.deviceId[i] = static_cast<std::uint8_t>(0x10 + i);
    }
    identity.startUnixMs = 1763290000123LL;
    return identity;
}

SyncEntry entry(const std::string& id, const std::string& name,
                std::uint32_t size)
{
    SyncEntry e;
    e.id = id;
    e.name = name;
    e.size = size;
    return e;
}

const std::string kIdA(32, 'a');
const std::string kIdB(32, 'b');

}  // namespace

// --- cprc chunk -----------------------------------------------------------

void test_recording_header_round_trip()
{
    const RecordingIdentity identity = sampleIdentity();
    std::vector<std::uint8_t> wav(kRecordingWavHeaderSize + 32000);
    encodeRecordingWavHeader(wav.data(), {16000, 1, 16}, 32000, identity);

    WavInfo info;
    RecordingIdentity decoded;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(WavError::kNone),
        static_cast<int>(parseRecordingWavHeader(
            wav.data(), kRecordingWavHeaderSize,
            static_cast<std::uint32_t>(wav.size()), info, decoded)));
    TEST_ASSERT_EQUAL_UINT32(kRecordingWavHeaderSize, info.dataOffset);
    TEST_ASSERT_EQUAL_UINT32(32000, info.dataSize);
    TEST_ASSERT_EQUAL_UINT32(16000, info.spec.sampleRate);
    TEST_ASSERT_EQUAL_MEMORY(identity.recordingId, decoded.recordingId, 16);
    TEST_ASSERT_EQUAL_MEMORY(identity.deviceId, decoded.deviceId, 8);
    TEST_ASSERT_TRUE(identity.startUnixMs == decoded.startUnixMs);

    // RIFF size covers everything after the first 8 bytes.
    const std::uint32_t riffSize = wav[4] | wav[5] << 8 | wav[6] << 16 |
                                   static_cast<std::uint32_t>(wav[7]) << 24;
    TEST_ASSERT_EQUAL_UINT32(wav.size() - 8, riffSize);
}

void test_generic_reader_skips_cprc_chunk()
{
    // Old readers (and the upstream chunk walker) must still find the audio.
    std::vector<std::uint8_t> wav(kRecordingWavHeaderSize + 100);
    encodeRecordingWavHeader(wav.data(), {16000, 1, 16}, 100,
                             sampleIdentity());
    WavInfo info;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(WavError::kNone),
        static_cast<int>(parseWav(wav.data(), wav.size(), info)));
    TEST_ASSERT_EQUAL_UINT32(kRecordingWavHeaderSize, info.dataOffset);
    TEST_ASSERT_EQUAL_UINT32(100, info.dataSize);

    // The fixed 44-byte fast path rejects it, which sends the device reader
    // to the cprc path instead of misreading the chunk as audio.
    TEST_ASSERT_NOT_EQUAL(
        static_cast<int>(WavError::kNone),
        static_cast<int>(parsePcmWavHeader(wav.data(), kPcmWavHeaderSize,
                                           wav.size(), info)));
    TEST_ASSERT_TRUE(hasCprcAfterFormat(wav.data(), kPcmWavHeaderSize));
}

void test_streaming_recording_header_uses_file_size()
{
    std::array<std::uint8_t, kRecordingWavHeaderSize> header{};
    encodeStreamingRecordingWavHeader(header.data(), {16000, 1, 16},
                                      sampleIdentity());
    WavInfo info;
    RecordingIdentity decoded;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(WavError::kNone),
        static_cast<int>(parseRecordingWavHeader(
            header.data(), header.size(), kRecordingWavHeaderSize + 6400,
            info, decoded)));
    TEST_ASSERT_EQUAL_UINT32(6400, info.dataSize);
}

void test_truncated_recording_is_rejected()
{
    std::array<std::uint8_t, kRecordingWavHeaderSize> header{};
    encodeRecordingWavHeader(header.data(), {16000, 1, 16}, 1000,
                             sampleIdentity());
    WavInfo info;
    RecordingIdentity decoded;
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(WavError::kTruncated),
        static_cast<int>(parseRecordingWavHeader(
            header.data(), header.size(), kRecordingWavHeaderSize + 999,
            info, decoded)));
}

void test_find_identity_in_prefix()
{
    std::vector<std::uint8_t> wav(kRecordingWavHeaderSize);
    const RecordingIdentity identity = sampleIdentity();
    encodeRecordingWavHeader(wav.data(), {16000, 1, 16}, 0, identity);
    RecordingIdentity found;
    TEST_ASSERT_TRUE(findRecordingIdentity(wav.data(), wav.size(), found));
    TEST_ASSERT_EQUAL_MEMORY(identity.recordingId, found.recordingId, 16);

    std::array<std::uint8_t, kPcmWavHeaderSize> legacy{};
    encodePcmWavHeader(legacy.data(), {16000, 1, 16}, 0);
    TEST_ASSERT_FALSE(
        findRecordingIdentity(legacy.data(), legacy.size(), found));
}

// --- SHA-256 --------------------------------------------------------------

void test_sha256_known_vectors()
{
    TEST_ASSERT_EQUAL_STRING(
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        sha256Hex("", 0).c_str());
    TEST_ASSERT_EQUAL_STRING(
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        sha256Hex("abc", 3).c_str());
    const char* two =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    TEST_ASSERT_EQUAL_STRING(
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
        sha256Hex(two, std::strlen(two)).c_str());
}

void test_sha256_incremental_matches_one_shot()
{
    std::vector<std::uint8_t> data(100000);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::uint8_t>(i * 31 + 7);
    }
    Sha256 hash;
    std::size_t offset = 0;
    const std::size_t steps[] = {1, 63, 64, 65, 4096, 32768};
    std::size_t step = 0;
    while (offset < data.size()) {
        std::size_t take = steps[step++ % 6];
        if (take > data.size() - offset) {
            take = data.size() - offset;
        }
        hash.update(data.data() + offset, take);
        offset += take;
    }
    std::uint8_t digest[32];
    hash.finish(digest);
    TEST_ASSERT_EQUAL_STRING(sha256Hex(data.data(), data.size()).c_str(),
                             toHex(digest, 32).c_str());
}

// --- sync index -----------------------------------------------------------

void test_index_cursor_bumps_on_visible_changes()
{
    SyncIndex index;
    index.upsert(entry(kIdA, "REC0001.WAV", 1000));
    TEST_ASSERT_EQUAL_UINT32(1, index.cursor());
    index.upsert(entry(kIdB, "REC0002.WAV", 2000));
    TEST_ASSERT_EQUAL_UINT32(2, index.cursor());
    TEST_ASSERT_TRUE(index.rename("rec0001.wav", "MEETING.WAV"));
    TEST_ASSERT_EQUAL_UINT32(3, index.cursor());
    TEST_ASSERT_EQUAL_UINT32(3, index.findById(kIdA)->changed);
    TEST_ASSERT_TRUE(index.setSha256(kIdA, std::string(64, 'f')));
    TEST_ASSERT_EQUAL_UINT32(3, index.cursor());  // hash cache only
    TEST_ASSERT_TRUE(index.markAcked(kIdB, "host1"));
    TEST_ASSERT_EQUAL_UINT32(4, index.cursor());
    TEST_ASSERT_EQUAL_UINT32(1, index.unsyncedCount("host1"));
    TEST_ASSERT_TRUE(index.removeByName("MEETING.WAV"));
    TEST_ASSERT_EQUAL_UINT32(5, index.cursor());
    TEST_ASSERT_EQUAL_UINT32(1, index.entries().size());
}

void test_index_serialize_round_trip()
{
    SyncIndex index;
    SyncEntry a = entry(kIdA, "MY NOTE.WAV", 64044);
    a.createdMs = 1763290000123LL;
    a.durationMs = 2000;
    a.sha256 = std::string(64, '1');
    a.ackedBy = "host1";
    index.upsert(a);
    index.upsert(entry(kIdB, "REC0002.WAV", 44));

    SyncIndex loaded;
    TEST_ASSERT_TRUE(loaded.parse(index.serialize()));
    TEST_ASSERT_EQUAL_UINT32(index.cursor(), loaded.cursor());
    TEST_ASSERT_EQUAL_UINT32(2, loaded.entries().size());
    const SyncEntry* back = loaded.findById(kIdA);
    TEST_ASSERT_NOT_NULL(back);
    TEST_ASSERT_EQUAL_STRING("MY NOTE.WAV", back->name.c_str());
    TEST_ASSERT_EQUAL_UINT32(64044, back->size);
    TEST_ASSERT_TRUE(back->createdMs == 1763290000123LL);
    TEST_ASSERT_EQUAL_UINT32(2000, back->durationMs);
    TEST_ASSERT_EQUAL_STRING(a.sha256.c_str(), back->sha256.c_str());
    TEST_ASSERT_EQUAL_STRING("host1", back->ackedBy.c_str());
}

void test_index_parse_rejects_bad_header_and_skips_bad_lines()
{
    SyncIndex index;
    TEST_ASSERT_FALSE(index.parse("garbage\n"));
    const std::string text = std::string("CPREC-SYN 1\t7\r\n") +
                             "short\tline\n" + kIdA +
                             "\tREC0001.WAV\t100\t0\t0\t3\t\t\n";
    TEST_ASSERT_TRUE(index.parse(text));
    TEST_ASSERT_EQUAL_UINT32(7, index.cursor());
    TEST_ASSERT_EQUAL_UINT32(1, index.entries().size());
}

void test_index_reconcile()
{
    SyncIndex index;
    index.upsert(entry(kIdA, "REC0001.WAV", 1000));
    index.upsert(entry(kIdB, "REC0002.WAV", 2000));
    const std::uint32_t before = index.cursor();

    // REC0001 is gone, REC0002 changed size, REC0003 is new.
    const std::vector<CardFile> files = {
        {"REC0002.WAV", 2500},
        {"REC0003.WAV", 3000},
    };
    const std::vector<std::string> unknown = index.reconcile(files);
    TEST_ASSERT_EQUAL_UINT32(0, index.entries().size());
    TEST_ASSERT_EQUAL_UINT32(2, unknown.size());
    TEST_ASSERT_EQUAL_STRING("REC0002.WAV", unknown[0].c_str());
    TEST_ASSERT_EQUAL_STRING("REC0003.WAV", unknown[1].c_str());
    TEST_ASSERT_TRUE(index.cursor() > before);

    // Nothing to do on a second pass once files are indexed.
    index.upsert(entry(kIdA, "REC0002.WAV", 2500));
    index.upsert(entry(kIdB, "REC0003.WAV", 3000));
    const std::uint32_t settled = index.cursor();
    TEST_ASSERT_EQUAL_UINT32(0, index.reconcile(files).size());
    TEST_ASSERT_EQUAL_UINT32(settled, index.cursor());
}

void test_index_clear_acks()
{
    SyncIndex index;
    index.upsert(entry(kIdA, "REC0001.WAV", 1000));
    index.markAcked(kIdA, "host1");
    index.clearAcks();
    TEST_ASSERT_EQUAL_UINT32(1, index.unsyncedCount("host1"));
}

void test_legacy_id_is_stable_and_case_insensitive()
{
    const std::string id = legacyRecordingId("rec0001.wav", 64044, 12345);
    TEST_ASSERT_EQUAL_UINT32(32, id.size());
    TEST_ASSERT_TRUE(protocol::isValidRecordingId(id));
    TEST_ASSERT_EQUAL_STRING(
        id.c_str(), legacyRecordingId("REC0001.WAV", 64044, 12345).c_str());
    TEST_ASSERT_TRUE(id != legacyRecordingId("REC0001.WAV", 64045, 12345));
}

// --- protocol -------------------------------------------------------------

void test_routes()
{
    using protocol::Route;
    using protocol::matchRoute;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Route::kInfo),
                          static_cast<int>(matchRoute("GET", "/v1/info").route));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(Route::kMethodNotAllowed),
        static_cast<int>(matchRoute("POST", "/v1/info").route));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Route::kTime),
                          static_cast<int>(matchRoute("POST", "/v1/time").route));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(Route::kManifest),
        static_cast<int>(matchRoute("GET", "/v1/manifest").route));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Route::kDone),
                          static_cast<int>(matchRoute("POST", "/v1/done").route));

    const std::string base = "/v1/recordings/" + kIdA;
    protocol::RouteMatch m = matchRoute("GET", base);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Route::kDownload),
                          static_cast<int>(m.route));
    TEST_ASSERT_EQUAL_STRING(kIdA.c_str(), m.id.c_str());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Route::kDelete),
                          static_cast<int>(matchRoute("DELETE", base).route));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(Route::kSha256),
        static_cast<int>(matchRoute("GET", base + "/sha256").route));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(Route::kAck),
        static_cast<int>(matchRoute("POST", base + "/ack").route));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(Route::kNotFound),
        static_cast<int>(matchRoute("GET", base + "/other").route));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(Route::kNotFound),
        static_cast<int>(
            matchRoute("GET", "/v1/recordings/../RECORDER.SYN").route));
    TEST_ASSERT_EQUAL_INT(
        static_cast<int>(Route::kNotFound),
        static_cast<int>(
            matchRoute("GET", "/v1/recordings/" + std::string(32, 'A'))
                .route));
}

void test_range_parsing()
{
    protocol::ByteRange r;
    TEST_ASSERT_TRUE(protocol::parseRange("bytes=0-", 1000, r));
    TEST_ASSERT_EQUAL_UINT32(0, r.start);
    TEST_ASSERT_EQUAL_UINT32(999, r.end);
    TEST_ASSERT_TRUE(protocol::parseRange("bytes=100-199", 1000, r));
    TEST_ASSERT_EQUAL_UINT32(100, r.start);
    TEST_ASSERT_EQUAL_UINT32(199, r.end);
    TEST_ASSERT_TRUE(protocol::parseRange("bytes=900-5000", 1000, r));
    TEST_ASSERT_EQUAL_UINT32(999, r.end);
    TEST_ASSERT_FALSE(protocol::parseRange("bytes=1000-", 1000, r));
    TEST_ASSERT_FALSE(protocol::parseRange("bytes=5-4", 1000, r));
    TEST_ASSERT_FALSE(protocol::parseRange("bytes=-100", 1000, r));
    TEST_ASSERT_FALSE(protocol::parseRange("bytes=0-1,5-6", 1000, r));
    TEST_ASSERT_FALSE(protocol::parseRange("items=0-", 1000, r));
    TEST_ASSERT_FALSE(protocol::parseRange("bytes=0-", 0, r));
}

void test_json_encoding()
{
    protocol::DeviceInfo info;
    info.deviceId = "0123456789abcdef";
    info.firmware = "1.5.0";
    info.batteryPct = 87;
    info.freeBytes = 4000000000ULL;
    info.unsynced = 2;
    info.clockSet = true;
    TEST_ASSERT_EQUAL_STRING(
        "{\"device_id\":\"0123456789abcdef\",\"firmware\":\"1.5.0\","
        "\"battery_pct\":87,\"free_bytes\":4000000000,\"unsynced\":2,"
        "\"clock_set\":true}",
        protocol::encodeInfo(info).c_str());

    SyncEntry e = entry(kIdA, "A \"B\".WAV", 64044);
    e.durationMs = 2000;
    const std::string json =
        protocol::encodeManifestHead(9) +
        protocol::encodeManifestEntry(e, true, false) +
        protocol::manifestTail();
    TEST_ASSERT_EQUAL_STRING(
        ("{\"cursor\":9,\"recordings\":[{\"id\":\"" + kIdA +
         "\",\"name\":\"A \\\"B\\\".WAV\",\"size\":64044,\"created_ms\":0,"
         "\"duration_ms\":2000,\"locked\":true,\"acked\":false}]}")
            .c_str(),
        json.c_str());
    TEST_ASSERT_EQUAL_STRING(
        "{\"code\":\"not_found\",\"message\":\"no such recording\"}",
        protocol::encodeError("not_found", "no such recording").c_str());
}

void test_json_extraction()
{
    std::string sha;
    TEST_ASSERT_TRUE(protocol::extractJsonString(
        "{ \"sha256\" : \"abc123\" }", "sha256", sha));
    TEST_ASSERT_EQUAL_STRING("abc123", sha.c_str());
    TEST_ASSERT_FALSE(protocol::extractJsonString("{\"sha\":\"x\"}",
                                                  "sha256", sha));
    std::int64_t value = 0;
    TEST_ASSERT_TRUE(protocol::extractJsonInt64(
        "{\"unix_ms\":1763290000123,\"tz_offset_min\":-300}", "unix_ms",
        value));
    TEST_ASSERT_TRUE(value == 1763290000123LL);
    TEST_ASSERT_TRUE(protocol::extractJsonInt64(
        "{\"unix_ms\":1,\"tz_offset_min\":-300}", "tz_offset_min", value));
    TEST_ASSERT_TRUE(value == -300);
    TEST_ASSERT_FALSE(
        protocol::extractJsonInt64("{\"unix_ms\":\"1\"}", "unix_ms", value));
}

int main(int argc, char** argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_recording_header_round_trip);
    RUN_TEST(test_generic_reader_skips_cprc_chunk);
    RUN_TEST(test_streaming_recording_header_uses_file_size);
    RUN_TEST(test_truncated_recording_is_rejected);
    RUN_TEST(test_find_identity_in_prefix);
    RUN_TEST(test_sha256_known_vectors);
    RUN_TEST(test_sha256_incremental_matches_one_shot);
    RUN_TEST(test_index_cursor_bumps_on_visible_changes);
    RUN_TEST(test_index_serialize_round_trip);
    RUN_TEST(test_index_parse_rejects_bad_header_and_skips_bad_lines);
    RUN_TEST(test_index_reconcile);
    RUN_TEST(test_index_clear_acks);
    RUN_TEST(test_legacy_id_is_stable_and_case_insensitive);
    RUN_TEST(test_routes);
    RUN_TEST(test_range_parsing);
    RUN_TEST(test_json_encoding);
    RUN_TEST(test_json_extraction);
    return UNITY_END();
}
