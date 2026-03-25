#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include "../CSCN74000_Server/Protocol.h"
#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <ctime>
#include <cstring>
#include <direct.h>

using namespace proto;

static const char* LOG_DIR = "logs";
static const char* DEFAULT_SERVER = "127.0.0.1";
static uint16_t g_server_port = DEFAULT_PORT;
static int g_timeout_sec = DEFAULT_TIMEOUT_SEC;
static uint32_t g_seq = 0;
static FILE* g_log = nullptr;

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

bool send_packet(SOCKET s, CmdId cmd, const uint8_t* payload, uint32_t payload_len) {
    Header h = {};
    h.version = VERSION;
    h.command_id = cmd;
    h.sequence = ++g_seq;
    h.payload_length = payload_len;
    h.checksum = payload && payload_len ? checksum(payload, payload_len) : 0;
    header_to_net(h);
    if (send(s, (const char*)&h, sizeof(h), 0) != (int)sizeof(h)) return false;
    if (payload && payload_len && send(s, (const char*)payload, payload_len, 0) != (int)payload_len) return false;
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

bool do_hello_auth(SOCKET s) {
    if (!send_packet(s, HELLO, nullptr, 0)) return false;
    Header h;
    if (!recv_header(s, h) || h.command_id != HELLO_ACK) return false;
    std::vector<uint8_t> dummy;
    if (h.payload_length && !recv_payload(s, h.payload_length, dummy)) return false;
    log_packet("RX", HELLO_ACK, h.sequence, h.payload_length);

    if (!send_packet(s, AUTH, nullptr, 0)) return false;
    if (!recv_header(s, h) || h.command_id != AUTH_ACK) return false;
    if (h.payload_length && !recv_payload(s, h.payload_length, dummy)) return false;
    log_packet("RX", AUTH_ACK, h.sequence, h.payload_length);
    return true;
}

int main(int argc, char* argv[]) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup failed\n");
        return 1;
    }

    const char* server_ip = argc > 1 ? argv[1] : DEFAULT_SERVER;
    if (argc > 2) g_server_port = (uint16_t)atoi(argv[2]);

    std::string log_path = std::string(LOG_DIR) + "/client.log";
    _mkdir(LOG_DIR);
    g_log = fopen(log_path.c_str(), "a");
    if (!g_log) g_log = stderr;

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        printf("socket failed\n");
        return 1;
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(g_server_port);
    addr.sin_addr.s_addr = inet_addr(server_ip);
    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        printf("connect failed to %s:%u\n", server_ip, (unsigned)g_server_port);
        return 1;
    }
    DWORD to = g_timeout_sec * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof(to));

    if (!do_hello_auth(sock)) {
        printf("HELLO/AUTH failed\n");
        closesocket(sock);
        return 1;
    }
    printf("Connected and verified.\n");

    for (;;) {
        printf("\n1=Submit flight plan  2=Request takeoff slot  3=Request landing slot  4=Request dispatch package  0=Exit\n> ");
        int choice = 0;
        if (scanf("%d", &choice) != 1) break;
        if (choice == 0) break;

        if (choice == 1) {
            FlightPlanPayload fp = {};
            printf("Flight ID: "); scanf("%31s", fp.flight_id);
            printf("Aircraft ID: "); scanf("%31s", fp.aircraft_id);
            printf("Origin: "); scanf("%31s", fp.origin);
            printf("Destination: "); scanf("%31s", fp.destination);
            printf("ETD (epoch min): ");
            scanf("%I64u", (unsigned __int64*)&fp.etd);
            printf("ETA (epoch min): ");
            scanf("%I64u", (unsigned __int64*)&fp.eta);
            printf("Runway op: "); scanf("%15s", fp.runway_op);
            fp.etd = _byteswap_uint64(fp.etd);
            fp.eta = _byteswap_uint64(fp.eta);
            if (!send_packet(sock, SUBMIT_FLIGHT_PLAN, (uint8_t*)&fp, sizeof(fp))) break;
            Header h;
            if (!recv_header(sock, h) || h.command_id != FLIGHT_PLAN_RESP) break;
            std::vector<uint8_t> pl;
            if (!recv_payload(sock, h.payload_length, pl)) break;
            log_packet("RX", FLIGHT_PLAN_RESP, h.sequence, h.payload_length);
            if (pl.size() >= 2) {
                uint16_t rc = ntohs(*(uint16_t*)pl.data());
                printf("Result: %u\n", (unsigned)rc);
            }
        }
        else if (choice == 2) {
            uint32_t etd = 0;
            printf("ETD (epoch min): "); scanf("%u", &etd);
            etd = htonl(etd);
            if (!send_packet(sock, REQ_TAKEOFF_SLOT, (uint8_t*)&etd, 4)) break;
            Header h;
            if (!recv_header(sock, h) || h.command_id != TAKEOFF_SLOT_RESP) break;
            std::vector<uint8_t> pl;
            if (!recv_payload(sock, h.payload_length, pl)) break;
            log_packet("RX", TAKEOFF_SLOT_RESP, h.sequence, h.payload_length);
            if (pl.size() >= 2) {
                uint16_t rc = ntohs(*(uint16_t*)pl.data());
                printf("Result: %u", (unsigned)rc);
                if (pl.size() >= 18) printf(" Runway: %.16s", pl.data() + 2);
                printf("\n");
            }
        }
        else if (choice == 3) {
            uint32_t eta = 0;
            printf("ETA (epoch min): "); scanf("%u", &eta);
            eta = htonl(eta);
            if (!send_packet(sock, REQ_LANDING_SLOT, (uint8_t*)&eta, 4)) break;
            Header h;
            if (!recv_header(sock, h) || h.command_id != LANDING_SLOT_RESP) break;
            std::vector<uint8_t> pl;
            if (!recv_payload(sock, h.payload_length, pl)) break;
            log_packet("RX", LANDING_SLOT_RESP, h.sequence, h.payload_length);
            if (pl.size() >= 2) {
                uint16_t rc = ntohs(*(uint16_t*)pl.data());
                printf("Result: %u", (unsigned)rc);
                if (pl.size() >= 18) printf(" Runway: %.16s", pl.data() + 2);
                printf("\n");
            }
        }
        else if (choice == 4) {
            if (!send_packet(sock, REQ_DISPATCH_PKG, nullptr, 0)) break;
            std::ofstream out("dispatch_package.bin", std::ios::binary);
            if (!out) { printf("Cannot create dispatch_package.bin\n"); continue; }
            size_t total_written = 0;
            while (total_written < DISPATCH_PKG_SIZE) {
                Header h;
                if (!recv_header(sock, h) || h.command_id != DISPATCH_CHUNK) break;
                std::vector<uint8_t> pl;
                if (!recv_payload(sock, h.payload_length, pl)) break;
                log_packet("RX", DISPATCH_CHUNK, h.sequence, h.payload_length);
                if (pl.size() < sizeof(ChunkMeta)) break;
                ChunkMeta meta;
                memcpy(&meta, pl.data(), sizeof(meta));
                meta.chunk_index = ntohl(meta.chunk_index);
                meta.total_chunks = ntohl(meta.total_chunks);
                meta.total_size = ntohl(meta.total_size);
                uint32_t data_len = (uint32_t)pl.size() - sizeof(ChunkMeta);
                out.write((const char*)pl.data() + sizeof(ChunkMeta), data_len);
                total_written += data_len;
            }
            out.close();
            printf("Saved %zu bytes to dispatch_package.bin\n", total_written);
        }
    }

    if (g_log && g_log != stderr) fclose(g_log);
    closesocket(sock);
    WSACleanup();
    return 0;
}