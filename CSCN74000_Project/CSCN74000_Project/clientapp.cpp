#include "ClientApp.h"
#include <iostream>
#include <fstream>
#include <ctime>
#include <cstring>
#include <direct.h>

using namespace proto;

static const char* LOG_DIR = "logs";
static const char* LOG_FILE = "logs/client.log";

// ============================================================
//  Constructor / Destructor
// ============================================================

ClientApp::ClientApp()
    : m_sock(INVALID_SOCKET)
    , m_seq(0)
    , m_log(nullptr)
    , m_timeout_sec(DEFAULT_TIMEOUT_SEC)
    , m_port(DEFAULT_PORT)
    , m_max_log_bytes(MAX_LOG_BYTES)
    , m_log_path(LOG_FILE)
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    _mkdir(LOG_DIR);
    m_log = fopen(m_log_path.c_str(), "a");
    if (!m_log) m_log = stderr;
}

ClientApp::~ClientApp() {
    disconnect();
    if (m_log && m_log != stderr) fclose(m_log);
    WSACleanup();
}

// ============================================================
//  Public - connect / reconnect / disconnect
// ============================================================

bool ClientApp::connect(const char* server_ip, uint16_t port, int timeout_sec) {
    m_server_ip = server_ip;
    m_port = port;
    m_timeout_sec = timeout_sec;

    m_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_sock == INVALID_SOCKET) {
        printf("socket() failed\n");
        return false;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(m_port);
    addr.sin_addr.s_addr = inet_addr(m_server_ip.c_str());

    if (::connect(m_sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        printf("connect() failed to %s:%u\n", m_server_ip.c_str(), (unsigned)m_port);
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
        return false;
    }

    DWORD to = (DWORD)(m_timeout_sec * 1000);
    setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof(to));

    if (!doHelloAuth(m_sock)) {
        printf("HELLO/AUTH handshake failed\n");
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
        return false;
    }

    printf("Connected and verified.\n");
    return true;
}

// US-COM-05: close the existing socket, open a fresh TCP connection to the
// same server, and re-run the full HELLO/AUTH sequence so the session is
// verified before any further commands are issued.
bool ClientApp::reconnect() {
    printf("Reconnecting to %s:%u ...\n", m_server_ip.c_str(), (unsigned)m_port);

    // Tear down the old socket cleanly
    if (m_sock != INVALID_SOCKET) {
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
    }
    m_seq = 0;  // reset sequence counter for the new session

    // Re-establish TCP connection
    m_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_sock == INVALID_SOCKET) {
        printf("reconnect: socket() failed\n");
        return false;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(m_port);
    addr.sin_addr.s_addr = inet_addr(m_server_ip.c_str());

    if (::connect(m_sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        printf("reconnect: connect() failed\n");
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
        return false;
    }

    DWORD to = (DWORD)(m_timeout_sec * 1000);
    setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof(to));

    // US-COM-05: always repeat the HELLO/AUTH sequence after reconnecting
    if (!doHelloAuth(m_sock)) {
        printf("reconnect: HELLO/AUTH failed\n");
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
        return false;
    }

    printf("Reconnected and verified.\n");
    return true;
}

void ClientApp::disconnect() {
    if (m_sock != INVALID_SOCKET) {
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
    }
}

// ============================================================
//  Public - run (main menu loop)
// ============================================================

void ClientApp::run() {
    if (m_sock == INVALID_SOCKET) return;

    for (;;) {
        printf("\n1=Submit flight plan  2=Request takeoff slot  "
            "3=Request landing slot  4=Request dispatch package  "
            "5=Reconnect  6=Export log  0=Exit\n> ");
        int choice = 0;
        if (scanf("%d", &choice) != 1) break;
        if (choice == 0) break;

        switch (choice) {
        case 1: cmdSubmitFlightPlan();       break;
        case 2: cmdRequestTakeoffSlot();     break;
        case 3: cmdRequestLandingSlot();     break;
        case 4: cmdRequestDispatchPackage(); break;

            // US-COM-05: allow the user to trigger a manual reconnect
        case 5:
            if (reconnect())
                printf("Session re-established.\n");
            else
                printf("Reconnect failed.\n");
            break;

            // US-LOG-06: export log to a user-specified path
        case 6: {
            char dest[256] = {};
            printf("Export log to path: ");
            scanf("%255s", dest);
            if (exportLog(dest))
                printf("Log exported to %s\n", dest);
            else
                printf("Export failed.\n");
            break;
        }

        default: printf("Unknown option\n"); break;
        }
    }
}

// ============================================================
//  US-LOG-06 - export log
// ============================================================

// Copies the current log file byte-for-byte to dest_path.
// The live log file stays open and continues to be appended to.
bool ClientApp::exportLog(const char* dest_path) {
    if (!dest_path || dest_path[0] == '\0') return false;

    // Flush any buffered log data before copying
    if (m_log && m_log != stderr) fflush(m_log);

    std::ifstream src(m_log_path, std::ios::binary);
    if (!src.is_open()) return false;

    std::ofstream dst(dest_path, std::ios::binary | std::ios::trunc);
    if (!dst.is_open()) return false;

    dst << src.rdbuf();
    return dst.good();
}

// ============================================================
//  Private - handshake
// ============================================================

bool ClientApp::doHelloAuth(SOCKET s) {
    if (!sendPacket(s, HELLO, nullptr, 0)) return false;
    Header h;
    if (!recvHeader(s, h) || h.command_id != HELLO_ACK) return false;
    std::vector<uint8_t> dummy;
    if (h.payload_length && !recvPayload(s, h.payload_length, dummy)) return false;
    logPacket("RX", HELLO_ACK, h.sequence, h.payload_length);

    if (!sendPacket(s, AUTH, nullptr, 0)) return false;
    if (!recvHeader(s, h) || h.command_id != AUTH_ACK) return false;
    if (h.payload_length && !recvPayload(s, h.payload_length, dummy)) return false;
    logPacket("RX", AUTH_ACK, h.sequence, h.payload_length);

    return true;
}

// ============================================================
//  Private - commands
// ============================================================

void ClientApp::cmdSubmitFlightPlan() {
    FlightPlanPayload fp = {};
    printf("Flight ID:        "); scanf("%31s", fp.flight_id);
    printf("Aircraft ID:      "); scanf("%31s", fp.aircraft_id);
    printf("Origin:           "); scanf("%31s", fp.origin);
    printf("Destination:      "); scanf("%31s", fp.destination);
    printf("ETD (epoch min):  "); scanf("%I64u", (unsigned __int64*)&fp.etd);
    printf("ETA (epoch min):  "); scanf("%I64u", (unsigned __int64*)&fp.eta);
    printf("Runway op:        "); scanf("%15s", fp.runway_op);

    fp.etd = _byteswap_uint64(fp.etd);
    fp.eta = _byteswap_uint64(fp.eta);

    if (!sendPacket(m_sock, SUBMIT_FLIGHT_PLAN, (uint8_t*)&fp, sizeof(fp))) return;

    Header h;
    if (!recvHeader(m_sock, h) || h.command_id != FLIGHT_PLAN_RESP) return;
    std::vector<uint8_t> pl;
    if (!recvPayload(m_sock, h.payload_length, pl)) return;

    // US-LOG-02: log response with result code
    uint16_t rc = 0xFFFF;
    if (pl.size() >= 2) {
        rc = ntohs(*(uint16_t*)pl.data());
        printf("Result: %u\n", (unsigned)rc);
    }
    logPacket("RX", FLIGHT_PLAN_RESP, h.sequence, h.payload_length, rc);
}

void ClientApp::cmdRequestTakeoffSlot() {
    uint32_t etd = 0;
    printf("ETD (epoch min): "); scanf("%u", &etd);
    etd = htonl(etd);

    if (!sendPacket(m_sock, REQ_TAKEOFF_SLOT, (uint8_t*)&etd, 4)) return;

    Header h;
    if (!recvHeader(m_sock, h) || h.command_id != TAKEOFF_SLOT_RESP) return;
    std::vector<uint8_t> pl;
    if (!recvPayload(m_sock, h.payload_length, pl)) return;

    // US-LOG-02: log response with result code
    uint16_t rc = 0xFFFF;
    if (pl.size() >= 2) {
        rc = ntohs(*(uint16_t*)pl.data());
        printf("Result: %u", (unsigned)rc);
        if (pl.size() >= 18) printf("  Runway: %.16s", pl.data() + 2);
        printf("\n");
    }
    logPacket("RX", TAKEOFF_SLOT_RESP, h.sequence, h.payload_length, rc);
}

void ClientApp::cmdRequestLandingSlot() {
    uint32_t eta = 0;
    printf("ETA (epoch min): "); scanf("%u", &eta);
    eta = htonl(eta);

    if (!sendPacket(m_sock, REQ_LANDING_SLOT, (uint8_t*)&eta, 4)) return;

    Header h;
    if (!recvHeader(m_sock, h) || h.command_id != LANDING_SLOT_RESP) return;
    std::vector<uint8_t> pl;
    if (!recvPayload(m_sock, h.payload_length, pl)) return;

    // US-LOG-02: log response with result code
    uint16_t rc = 0xFFFF;
    if (pl.size() >= 2) {
        rc = ntohs(*(uint16_t*)pl.data());
        printf("Result: %u", (unsigned)rc);
        if (pl.size() >= 18) printf("  Runway: %.16s", pl.data() + 2);
        printf("\n");
    }
    logPacket("RX", LANDING_SLOT_RESP, h.sequence, h.payload_length, rc);
}

void ClientApp::cmdRequestDispatchPackage() {
    if (!sendPacket(m_sock, REQ_DISPATCH_PKG, nullptr, 0)) return;

    std::ofstream out("dispatch_package.bin", std::ios::binary);
    if (!out) { printf("Cannot create dispatch_package.bin\n"); return; }

    size_t total_written = 0;
    while (total_written < DISPATCH_PKG_SIZE) {
        Header h;
        if (!recvHeader(m_sock, h) || h.command_id != DISPATCH_CHUNK) break;
        std::vector<uint8_t> pl;
        if (!recvPayload(m_sock, h.payload_length, pl)) break;
        logPacket("RX", DISPATCH_CHUNK, h.sequence, h.payload_length);

        if (pl.size() < sizeof(ChunkMeta)) break;
        ChunkMeta meta;
        memcpy(&meta, pl.data(), sizeof(meta));
        meta.chunk_index = ntohl(meta.chunk_index);
        meta.total_chunks = ntohl(meta.total_chunks);
        meta.total_size = ntohl(meta.total_size);

        uint32_t data_len = (uint32_t)pl.size() - (uint32_t)sizeof(ChunkMeta);
        out.write((const char*)pl.data() + sizeof(ChunkMeta), data_len);
        total_written += data_len;
    }
    out.close();
    printf("Saved %zu bytes to dispatch_package.bin\n", total_written);
}

// ============================================================
//  US-LOG-05 - log rotation
// ============================================================

// Called before each write. If the current log file has grown past
// m_max_log_bytes, close it, rename it to client_YYYYMMDD_HHMMSS.log,
// then open a fresh client.log.
void ClientApp::rotateLogIfNeeded() {
    if (!m_log || m_log == stderr) return;

    // ftell gives the current write position = approximate file size
    long pos = ftell(m_log);
    if (pos < m_max_log_bytes) return;

    fclose(m_log);
    m_log = nullptr;

    // Build a timestamped archive name: logs/client_20260308_221554.log
    time_t t = time(nullptr);
    struct tm tm_buf;
    localtime_s(&tm_buf, &t);
    char archive[256];
    strftime(archive, sizeof(archive),
        "logs/client_%Y%m%d_%H%M%S.log", &tm_buf);

    rename(m_log_path.c_str(), archive);
    printf("[log] Rotated log to %s\n", archive);

    m_log = fopen(m_log_path.c_str(), "a");
    if (!m_log) m_log = stderr;
}

// ============================================================
//  Private - logging
// ============================================================

void ClientApp::logPacket(const char* dir, uint16_t cmd, uint32_t seq,
    uint32_t payload_sz, uint16_t result) {
    // US-LOG-05: rotate before writing if size threshold exceeded
    rotateLogIfNeeded();

    if (!m_log) return;
    time_t t = time(nullptr);
    struct tm tm_buf;
    localtime_s(&tm_buf, &t);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);

    if (result != 0xFFFF)
        fprintf(m_log, "%s %s CMD=%u SEQ=%u PAYLOAD=%u RESULT=%u\n",
            ts, dir, (unsigned)cmd, seq, payload_sz, (unsigned)result);
    else
        fprintf(m_log, "%s %s CMD=%u SEQ=%u PAYLOAD=%u\n",
            ts, dir, (unsigned)cmd, seq, payload_sz);
    fflush(m_log);  // US-LOG-04: flush every packet
}

// ============================================================
//  Private - network helpers
// ============================================================

bool ClientApp::sendPacket(SOCKET s, CmdId cmd, const uint8_t* payload, uint32_t payload_len) {
    Header h = {};
    h.version = VERSION;
    h.command_id = cmd;
    h.sequence = ++m_seq;
    h.payload_length = payload_len;
    h.checksum = (payload && payload_len) ? checksum(payload, payload_len) : 0;
    header_to_net(h);

    if (send(s, (const char*)&h, sizeof(h), 0) != (int)sizeof(h)) return false;
    if (payload && payload_len &&
        send(s, (const char*)payload, payload_len, 0) != (int)payload_len) return false;

    logPacket("TX", cmd, m_seq, payload_len);
    return true;
}

bool ClientApp::recvHeader(SOCKET s, Header& h) {
    if (recv(s, (char*)&h, sizeof(h), 0) != (int)sizeof(h)) return false;
    header_to_host(h);
    return true;
}

bool ClientApp::recvPayload(SOCKET s, uint32_t len, std::vector<uint8_t>& out) {
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