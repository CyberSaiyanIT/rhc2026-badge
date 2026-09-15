#ifdef ESP_PLATFORM
#include <sdkconfig.h>
#endif

#if defined(CONFIG_SOC_WIFI_SUPPORTED) || defined(CONFIG_SLAVE_SOC_WIFI_SUPPORTED)

#include <Tactility/app/pingpong/PingPongProtocol.h>

#include <cstdio>
#include <cstring>

namespace tt::app::pingpong {

namespace {

void writeU16(uint8_t* buffer, size_t& offset, uint16_t value) {
    buffer[offset++] = static_cast<uint8_t>(value & 0xFF);
    buffer[offset++] = static_cast<uint8_t>(value >> 8);
}

void writeU32(uint8_t* buffer, size_t& offset, uint32_t value) {
    for (int i = 0; i < 4; i++) {
        buffer[offset++] = static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
    }
}

uint16_t readU16(const uint8_t* buffer, size_t& offset) {
    uint16_t value = static_cast<uint16_t>(buffer[offset]) | (static_cast<uint16_t>(buffer[offset + 1]) << 8);
    offset += 2;
    return value;
}

uint32_t readU32(const uint8_t* buffer, size_t& offset) {
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) {
        value |= static_cast<uint32_t>(buffer[offset + i]) << (i * 8);
    }
    offset += 4;
    return value;
}

size_t payloadSizeOf(PacketType type) {
    switch (type) {
        case PacketType::Hello:
        case PacketType::Decline:
        case PacketType::Rematch:
        case PacketType::Bye: return 0;
        case PacketType::Invite:
        case PacketType::Accept: return PUBLIC_KEY_SIZE;
        case PacketType::State: return 15;
        case PacketType::Input: return 2;
    }
    return 0;
}

} // namespace

std::string nameOf(const MacAddress& address) {
    char name[16];
    snprintf(name, sizeof(name), "Badge-%02X%02X%02X", address[3], address[4], address[5]);
    return name;
}

size_t serialize(const Packet& packet, uint8_t* out, size_t outSize) {
    if (outSize < MAX_PACKET_SIZE) {
        return 0;
    }

    size_t offset = 0;
    writeU32(out, offset, PROTOCOL_MAGIC);
    out[offset++] = PROTOCOL_VERSION;
    out[offset++] = static_cast<uint8_t>(packet.type);
    out[offset++] = packet.session;
    memcpy(out + offset, packet.from.data(), packet.from.size());
    offset += packet.from.size();
    memcpy(out + offset, packet.to.data(), packet.to.size());
    offset += packet.to.size();

    switch (packet.type) {
        case PacketType::Invite:
        case PacketType::Accept:
            memcpy(out + offset, packet.publicKey.data(), packet.publicKey.size());
            offset += packet.publicKey.size();
            break;
        case PacketType::State:
            writeU16(out, offset, static_cast<uint16_t>(packet.ballX));
            writeU16(out, offset, static_cast<uint16_t>(packet.ballY));
            writeU16(out, offset, static_cast<uint16_t>(packet.ballVx));
            writeU16(out, offset, static_cast<uint16_t>(packet.ballVy));
            writeU16(out, offset, static_cast<uint16_t>(packet.hostPaddleY));
            writeU16(out, offset, static_cast<uint16_t>(packet.guestPaddleY));
            out[offset++] = packet.hostScore;
            out[offset++] = packet.guestScore;
            out[offset++] = packet.over;
            break;
        case PacketType::Input:
            writeU16(out, offset, static_cast<uint16_t>(packet.paddleY));
            break;
        default:
            break;
    }

    return offset;
}

bool deserialize(const uint8_t* data, size_t length, Packet& out) {
    if (length < HEADER_SIZE || length > MAX_PACKET_SIZE) {
        return false;
    }

    size_t offset = 0;
    if (readU32(data, offset) != PROTOCOL_MAGIC) {
        return false;
    }
    if (data[offset++] != PROTOCOL_VERSION) {
        return false;
    }

    uint8_t rawType = data[offset++];
    if (rawType < static_cast<uint8_t>(PacketType::Hello) || rawType > static_cast<uint8_t>(PacketType::Rematch)) {
        return false;
    }
    out.type = static_cast<PacketType>(rawType);
    out.session = data[offset++];
    memcpy(out.from.data(), data + offset, out.from.size());
    offset += out.from.size();
    memcpy(out.to.data(), data + offset, out.to.size());
    offset += out.to.size();

    if (length - HEADER_SIZE != payloadSizeOf(out.type)) {
        return false;
    }

    switch (out.type) {
        case PacketType::Invite:
        case PacketType::Accept:
            memcpy(out.publicKey.data(), data + offset, out.publicKey.size());
            break;
        case PacketType::State:
            out.ballX = static_cast<int16_t>(readU16(data, offset));
            out.ballY = static_cast<int16_t>(readU16(data, offset));
            out.ballVx = static_cast<int16_t>(readU16(data, offset));
            out.ballVy = static_cast<int16_t>(readU16(data, offset));
            out.hostPaddleY = static_cast<int16_t>(readU16(data, offset));
            out.guestPaddleY = static_cast<int16_t>(readU16(data, offset));
            out.hostScore = data[offset++];
            out.guestScore = data[offset++];
            out.over = data[offset++];
            break;
        case PacketType::Input:
            out.paddleY = static_cast<int16_t>(readU16(data, offset));
            break;
        default:
            break;
    }

    return true;
}

} // namespace tt::app::pingpong

#endif // CONFIG_SOC_WIFI_SUPPORTED || CONFIG_SLAVE_SOC_WIFI_SUPPORTED
