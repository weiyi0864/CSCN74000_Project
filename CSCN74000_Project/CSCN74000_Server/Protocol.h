#ifndef PROTOCOL_H
#define PROTOCOL_H

#pragma comment(lib, "ws2_32.lib")
#include <winsock2.h>
#include <ws2tcpip.h>
#include <intrin.h>

#include <cstdint>
#include <cstring>

namespace proto {

    constexpr uint8_t  VERSION = 1;
    constexpr uint32_t HEADER_SIZE = 15;
    constexpr uint32_t DISPATCH_PKG_SIZE = 1024 * 1024;  // 1 MB
    constexpr uint32_t CHUNK_SIZE = 8192;
    constexpr uint32_t DEFAULT_PORT = 5050;
    constexpr int     DEFAULT_TIMEOUT_SEC = 120;

    enum CmdId : uint16_t {
        HELLO = 1,
        HELLO_ACK,
        AUTH,
        AUTH_ACK,
        SUBMIT_FLIGHT_PLAN,
        FLIGHT_PLAN_RESP,
        REQ_TAKEOFF_SLOT,
        TAKEOFF_SLOT_RESP,
        REQ_LANDING_SLOT,
        LANDING_SLOT_RESP,
        REQ_DISPATCH_PKG,
        DISPATCH_CHUNK,
        ERROR_RESP
    };

    enum ResultCode : uint16_t {
        OK = 0,
        ERR_BUSY,
        ERR_INVALID_STATE,
        ERR_AUTH_FAILED,
        ERR_INVALID_PACKET,
        ERR_VALIDATION
    };

#pragma pack(push, 1)
    struct Header {
        uint8_t  version;
        uint16_t command_id;
        uint32_t sequence;
        uint32_t payload_length;
        uint32_t checksum;
    };
#pragma pack(pop)

    inline uint32_t checksum(const uint8_t* data, uint32_t len) {
        uint32_t sum = 0;
        for (uint32_t i = 0; i < len; ++i) sum += data[i];
        return sum;
    }

    inline void header_to_net(Header& h) {
        h.command_id = htons(h.command_id);
        h.sequence = htonl(h.sequence);
        h.payload_length = htonl(h.payload_length);
        h.checksum = htonl(h.checksum);
    }

    inline void header_to_host(Header& h) {
        h.command_id = ntohs(h.command_id);
        h.sequence = ntohl(h.sequence);
        h.payload_length = ntohl(h.payload_length);
        h.checksum = ntohl(h.checksum);
    }

    constexpr size_t FLIGHT_ID_LEN = 32;
    constexpr size_t AIRCRAFT_ID_LEN = 32;
    constexpr size_t ORIGIN_LEN = 32;
    constexpr size_t DEST_LEN = 32;
    constexpr size_t RUNWAY_OP_LEN = 16;
    constexpr size_t FLIGHT_PLAN_PAYLOAD_SIZE =
        FLIGHT_ID_LEN + AIRCRAFT_ID_LEN + ORIGIN_LEN + DEST_LEN + 8 + 8 + RUNWAY_OP_LEN;  // +etd+eta

    struct FlightPlanPayload {
        char flight_id[FLIGHT_ID_LEN];
        char aircraft_id[AIRCRAFT_ID_LEN];
        char origin[ORIGIN_LEN];
        char destination[DEST_LEN];
        uint64_t etd;   // epoch min
        uint64_t eta;   // epoch min
        char runway_op[RUNWAY_OP_LEN];
    };

    struct ChunkMeta {
        uint32_t chunk_index;
        uint32_t total_chunks;
        uint32_t total_size;
    };

}  // namespace proto

#endif
#pragma once
