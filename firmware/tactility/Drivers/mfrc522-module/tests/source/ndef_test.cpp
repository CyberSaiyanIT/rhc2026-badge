#include "doctest.h"

#include <drivers/ndef.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr const char* UUID = "f0180065-5192-4995-a852-09f78861836b";

/** Wraps @a text in a short Text record with an "en" language code. */
std::vector<uint8_t> textRecord(const std::string& text, const std::string& language = "en") {
    std::vector<uint8_t> record;
    record.push_back(0xD1); // MB | ME | SR | TNF=well-known
    record.push_back(0x01); // type length
    record.push_back(static_cast<uint8_t>(1 + language.size() + text.size()));
    record.push_back('T');
    record.push_back(static_cast<uint8_t>(language.size())); // UTF-8, language length in low bits
    record.insert(record.end(), language.begin(), language.end());
    record.insert(record.end(), text.begin(), text.end());
    return record;
}

/** Wraps an NDEF message in the message TLV, terminated, as it sits in a tag's data area. */
std::vector<uint8_t> messageTlv(const std::vector<uint8_t>& message) {
    std::vector<uint8_t> tlv;
    tlv.push_back(0x03);
    if (message.size() < 0xFF) {
        tlv.push_back(static_cast<uint8_t>(message.size()));
    } else {
        tlv.push_back(0xFF);
        tlv.push_back(static_cast<uint8_t>(message.size() >> 8));
        tlv.push_back(static_cast<uint8_t>(message.size() & 0xFF));
    }
    tlv.insert(tlv.end(), message.begin(), message.end());
    tlv.push_back(0xFE);
    return tlv;
}

std::string parse(const std::vector<uint8_t>& data, bool& ok, size_t outSize = 64) {
    std::vector<char> out(outSize, '\0');
    ok = ndef_parse_text_record(data.data(), data.size(), out.data(), out.size());
    return ok ? std::string(out.data()) : std::string();
}

} // namespace

TEST_CASE("a well-formed Text record yields the UUID") {
    bool ok = false;
    CHECK(parse(messageTlv(textRecord(UUID)), ok) == UUID);
    CHECK(ok);
}

TEST_CASE("the 3-byte TLV length form is understood") {
    // A payload long enough to force the 0xFF escape.
    const std::string text(300, 'a');
    bool ok = false;
    const auto message = textRecord(text);
    REQUIRE(message.size() >= 0xFF);

    // The record itself needs the long payload-length form once it exceeds a short record.
    std::vector<uint8_t> longRecord;
    longRecord.push_back(0xC1); // MB | ME | TNF=well-known, SR cleared
    longRecord.push_back(0x01);
    const uint32_t payloadLength = static_cast<uint32_t>(1 + 2 + text.size());
    longRecord.push_back(static_cast<uint8_t>(payloadLength >> 24));
    longRecord.push_back(static_cast<uint8_t>(payloadLength >> 16));
    longRecord.push_back(static_cast<uint8_t>(payloadLength >> 8));
    longRecord.push_back(static_cast<uint8_t>(payloadLength));
    longRecord.push_back('T');
    longRecord.push_back(0x02);
    longRecord.push_back('e');
    longRecord.push_back('n');
    longRecord.insert(longRecord.end(), text.begin(), text.end());

    CHECK(parse(messageTlv(longRecord), ok, 512) == text);
    CHECK(ok);
}

TEST_CASE("lock and proprietary TLVs before the message are skipped") {
    std::vector<uint8_t> data { 0x01, 0x03, 0xAA, 0xBB, 0xCC, 0xFD, 0x02, 0xDE, 0xAD };
    const auto tlv = messageTlv(textRecord(UUID));
    data.insert(data.end(), tlv.begin(), tlv.end());

    bool ok = false;
    CHECK(parse(data, ok) == UUID);
    CHECK(ok);
}

TEST_CASE("a multi-byte language code is not included in the text") {
    bool ok = false;
    CHECK(parse(messageTlv(textRecord(UUID, "en-US")), ok) == UUID);
    CHECK(ok);
}

TEST_CASE("a non-Text record is skipped in favour of the Text record after it") {
    std::vector<uint8_t> uriRecord { 0x91, 0x01, 0x05, 'U', 0x01, 'a', '.', 'c', 'o' };
    auto text = textRecord(UUID);
    text[0] = 0x51; // ME | SR | TNF=well-known, MB cleared: this is the last record, not the first
    uriRecord.insert(uriRecord.end(), text.begin(), text.end());

    bool ok = false;
    CHECK(parse(messageTlv(uriRecord), ok) == UUID);
    CHECK(ok);
}

TEST_CASE("an ID field is skipped rather than read as payload") {
    auto record = textRecord(UUID);
    record[0] |= 0x08; // IL
    record.insert(record.begin() + 3, 0x02); // ID length, before the type
    record.insert(record.begin() + 5, { 0x7A, 0x7B }); // the ID itself, after the type

    bool ok = false;
    CHECK(parse(messageTlv(record), ok) == UUID);
    CHECK(ok);
}

TEST_CASE("malformed input is rejected rather than read out of bounds") {
    bool ok = false;

    SUBCASE("empty buffer") {
        parse({}, ok);
        CHECK_FALSE(ok);
    }
    SUBCASE("terminator only") {
        parse({ 0xFE }, ok);
        CHECK_FALSE(ok);
    }
    SUBCASE("no message TLV at all") {
        parse({ 0x01, 0x02, 0xAA, 0xBB, 0xFE }, ok);
        CHECK_FALSE(ok);
    }
    SUBCASE("TLV length runs past the buffer") {
        parse({ 0x03, 0x40, 0xD1, 0x01 }, ok);
        CHECK_FALSE(ok);
    }
    SUBCASE("record truncated mid-payload") {
        auto tlv = messageTlv(textRecord(UUID));
        tlv.resize(tlv.size() - 10);
        parse(tlv, ok);
        CHECK_FALSE(ok);
    }
    SUBCASE("language length larger than the payload") {
        auto tlv = messageTlv(textRecord(UUID));
        tlv[6] = 0x3F; // status byte: claims a 63-byte language code
        parse(tlv, ok);
        CHECK_FALSE(ok);
    }
}

TEST_CASE("text that does not fit the output buffer is rejected, not truncated") {
    bool ok = false;
    // The UUID is 36 characters, so 36 bytes leaves no room for the terminator.
    parse(messageTlv(textRecord(UUID)), ok, 36);
    CHECK_FALSE(ok);

    parse(messageTlv(textRecord(UUID)), ok, 37);
    CHECK(ok);
}
