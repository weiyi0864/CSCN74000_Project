#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include "Protocol.h"
#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <ctime>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <direct.h>

using namespace proto;

enum State {
    STATE_INIT,
    STATE_LISTENING,
    STATE_CONNECTED,
    STATE_VERIFIED,
    STATE_DISPATCHING,
    STATE_TRANSFERRING,
    STATE_ERROR
};

static const char* LOG_DIR = "logs";
static uint16_t g_port = DEFAULT_PORT;
static int g_timeout_sec = DEFAULT_TIMEOUT_SEC;
static uint32_t g_seq = 0;
static FILE* g_log = nullptr;
static std::vector<FlightPlanPayload> g_flight_plans;
static bool g_transfer_busy = false;

void log_packet(const char* dir, uint16_t cmd, uint32_t seq, uint32_t payload_sz, uint16_t result = 0xFFFF) {
    if (!g_log) return;
    time_t t = time(nullptr);
    struct tm tm_buf;
    localtime_s(&tm_buf, &t);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);
    if (result != 0xFFFF)
        fprintf(g_log, "%s %s CMD=%u SEQ=%u PAYLOAD=%u RESULT=%u\n", ts, dir, (unsigned)cmd, seq, payload_sz, (unsigned)result);
    else
        fprintf(g_log, "%s %s CMD=%u SEQ=%u PAYLOAD=%u\n", ts, dir, (unsigned)cmd, seq, payload_sz);
    fflush(g_log);
}

bool send_packet(SOCKET s, CmdId cmd, const uint8_t* payload, uint32_t payload_len, int result_code = -1) {
    Header h = {};
    h.version = VERSION;
    h.command_id = cmd;
    h.sequence = ++g_seq;
    h.payload_length = payload_len;
    h.checksum = payload && payload_len ? checksum(payload, payload_len) : 0;
    header_to_net(h);
    if (send(s, (const char*)&h, sizeof(h), 0) != (int)sizeof(h)) return false;
    if (payload && payload_len && send(s, (const char*)payload, payload_len, 0) != (int)payload_len) return false;
    if (result_code >= 0)
        log_packet("TX", cmd, g_seq, payload_len, (uint16_t)result_code);
    else
        log_packet("TX", cmd, g_seq, payload_len);
    return true;
}

bool recv_header(SOCKET s, Header& h) {
    if (recv(s, (char*)&h, sizeof(h), 0) != (int)sizeof(h)) return false;
    header_to_host(h);
    return true;
}

bool recv_payload(SOCKET s, uint32_t len, std::vector<uint8_t>& out) {
    out.resize(len);
    if (len == 0) return true;
    size_t total = 0;
    while (total < len) {
        int n = recv(s, (char*)out.data() + total, (int)(len - total), 0);
        if (n <= 0) return false;
        total += n;
    }
    return true;
}

bool validate_flight_plan(const FlightPlanPayload& fp) {
    return fp.flight_id[0] && fp.aircraft_id[0] && fp.origin[0] && fp.destination[0];
}

uint16_t alloc_takeoff_slot(uint32_t etd, char* runway_out) {
    (void)etd;
    snprintf(runway_out, 16, "R01");
    return (uint16_t)OK;
}

uint16_t alloc_landing_slot(uint32_t eta, char* runway_out) {
    (void)eta;
    snprintf(runway_out, 16, "R02");
    return (uint16_t)OK;
}

int main() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup failed\n");
        return 1;
    }

    std::string log_path = std::string(LOG_DIR) + "/server.log";
    _mkdir(LOG_DIR);
    g_log = fopen(log_path.c_str(), "a");
    if (!g_log) g_log = stderr;

    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) {
        printf("socket failed\n");
        return 1;
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(g_port);
    if (bind(listen_sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        printf("bind failed\n");
        return 1;
    }
    listen(listen_sock, 5);

    State state = STATE_LISTENING;
    SOCKET client_sock = INVALID_SOCKET;
    bool run = true;

    printf("Server listening on port %u\n", (unsigned)g_port);

    while (run) {
        if (state == STATE_LISTENING) {
            if (client_sock != INVALID_SOCKET) {
                closesocket(client_sock);
                client_sock = INVALID_SOCKET;
            }
            g_transfer_busy = false;
            g_seq = 0;
            printf("Waiting for connection...\n");
            client_sock = accept(listen_sock, nullptr, nullptr);
            if (client_sock == INVALID_SOCKET) continue;
            state = STATE_CONNECTED;
            printf("Client connected\n");
        }

        if (client_sock == INVALID_SOCKET) { state = STATE_LISTENING; continue; }

        DWORD to = g_timeout_sec * 1000;
        setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof(to));

        Header h;
        if (!recv_header(client_sock, h)) {
            state = STATE_LISTENING;
            continue;
        }
        if (h.version != VERSION || h.payload_length > 1024 * 1024) {
            send_packet(client_sock, ERROR_RESP, (const uint8_t*)&h.command_id, 2, ERR_INVALID_PACKET);
            continue;
        }
        std::vector<uint8_t> payload;
        if (h.payload_length && !recv_payload(client_sock, h.payload_length, payload)) {
            state = STATE_LISTENING;
            continue;
        }
        uint32_t recv_checksum = payload.empty() ? 0 : checksum(payload.data(), (uint32_t)payload.size());
        if (recv_checksum != h.checksum) {
            send_packet(client_sock, ERROR_RESP, nullptr, 0, ERR_INVALID_PACKET);
            continue;
        }
        log_packet("RX", (CmdId)h.command_id, h.sequence, h.payload_length);

        switch ((CmdId)h.command_id) {
        case HELLO:
            if (state != STATE_CONNECTED) break;
            send_packet(client_sock, HELLO_ACK, nullptr, 0);
            break;
        case AUTH:
            if (state != STATE_CONNECTED) break;
            send_packet(client_sock, AUTH_ACK, nullptr, 0);
            state = STATE_VERIFIED;
            break;
        case SUBMIT_FLIGHT_PLAN:
            if (state != STATE_VERIFIED && state != STATE_DISPATCHING) break;
            if (payload.size() >= sizeof(FlightPlanPayload)) {
                FlightPlanPayload fp;
                memcpy(&fp, payload.data(), sizeof(fp));
                fp.etd = _byteswap_uint64(fp.etd);
                fp.eta = _byteswap_uint64(fp.eta);
                if (validate_flight_plan(fp)) {
                    g_flight_plans.push_back(fp);
                    uint16_t rc = OK;
                    uint16_t rc_net = htons(rc);
                    send_packet(client_sock, FLIGHT_PLAN_RESP, (uint8_t*)&rc_net, 2, OK);
                }
                else {
                    uint16_t rc = ERR_VALIDATION;
                    uint16_t rc_net = htons(rc);
                    send_packet(client_sock, FLIGHT_PLAN_RESP, (uint8_t*)&rc_net, 2, ERR_VALIDATION);
                }
            }
            break;
        case REQ_TAKEOFF_SLOT:
            if (state != STATE_VERIFIED && state != STATE_DISPATCHING) break;
            if (g_transfer_busy) {
                uint16_t rc = ERR_BUSY;
                uint16_t rc_net = htons(rc);
                send_packet(client_sock, TAKEOFF_SLOT_RESP, (uint8_t*)&rc_net, 2, ERR_BUSY);
            }
            else if (payload.size() >= 4) {
                uint32_t etd;
                memcpy(&etd, payload.data(), 4);
                etd = ntohl(etd);
                char rwy[16];
                uint16_t rc = alloc_takeoff_slot(etd, rwy);
                std::vector<uint8_t> resp;
                resp.resize(2 + 16);
                uint16_t rc_net = htons(rc);
                memcpy(resp.data(), &rc_net, 2);
                memcpy(resp.data() + 2, rwy, 16);
                send_packet(client_sock, TAKEOFF_SLOT_RESP, resp.data(), (uint32_t)resp.size(), rc);
            }
            break;
        case REQ_LANDING_SLOT:
            if (state != STATE_VERIFIED && state != STATE_DISPATCHING) break;
            if (g_transfer_busy) {
                uint16_t rc = ERR_BUSY;
                uint16_t rc_net = htons(rc);
                send_packet(client_sock, LANDING_SLOT_RESP, (uint8_t*)&rc_net, 2, ERR_BUSY);
            }
            else if (payload.size() >= 4) {
                uint32_t eta;
                memcpy(&eta, payload.data(), 4);
                eta = ntohl(eta);
                char rwy[16];
                uint16_t rc = alloc_landing_slot(eta, rwy);
                std::vector<uint8_t> resp;
                resp.resize(2 + 16);
                uint16_t rc_net = htons(rc);
                memcpy(resp.data(), &rc_net, 2);
                memcpy(resp.data() + 2, rwy, 16);
                send_packet(client_sock, LANDING_SLOT_RESP, resp.data(), (uint32_t)resp.size(), rc);
            }
            break;
        case REQ_DISPATCH_PKG:
            if (state != STATE_VERIFIED && state != STATE_DISPATCHING) break;
            g_transfer_busy = true;
            state = STATE_TRANSFERRING;
            {
                std::vector<uint8_t> pkg(DISPATCH_PKG_SIZE);
                for (size_t i = 0; i < pkg.size(); ++i) pkg[i] = (uint8_t)(i % 256);
                uint32_t total_chunks = (DISPATCH_PKG_SIZE + CHUNK_SIZE - 1) / CHUNK_SIZE;
                for (uint32_t i = 0; i < total_chunks; ++i) {
                    uint32_t off = i * CHUNK_SIZE;
                    uint32_t len;
                    if (off + CHUNK_SIZE <= pkg.size())
                        len = (uint32_t)CHUNK_SIZE;
                    else
                        len = (uint32_t)(pkg.size() - off);
                    std::vector<uint8_t> chunk_payload(sizeof(ChunkMeta) + len);
                    ChunkMeta meta;
                    meta.chunk_index = htonl(i);
                    meta.total_chunks = htonl(total_chunks);
                    meta.total_size = htonl(DISPATCH_PKG_SIZE);
                    memcpy(chunk_payload.data(), &meta, sizeof(meta));
                    memcpy(chunk_payload.data() + sizeof(meta), pkg.data() + off, len);
                    if (!send_packet(client_sock, DISPATCH_CHUNK, chunk_payload.data(), (uint32_t)chunk_payload.size())) break;
                }
            }
            g_transfer_busy = false;
            state = STATE_VERIFIED;
            break;
        default:
            break;
        }
    }

    if (g_log && g_log != stderr) fclose(g_log);
    if (client_sock != INVALID_SOCKET) closesocket(client_sock);
    closesocket(listen_sock);
    WSACleanup();
    return 0;
}
