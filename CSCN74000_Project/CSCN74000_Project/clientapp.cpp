#include "ClientApp.h"
#include <iostream>
#include <fstream>
#include <ctime>
#include <cstring>
#include <cctype>
#include <direct.h>

using namespace proto;

// --- Input helpers: retry on invalid input ---
static uint64_t readUint64(const char* prompt) {
    uint64_t val = 0;
    for (;;) {
        printf("%s", prompt);
        if (scanf("%I64u", (unsigned __int64*)&val) == 1) return val;
        int c; while ((c = getchar()) != '\n' && c != EOF) {}
        printf("  Invalid input, please enter a number.\n");
    }
}

static uint32_t readUint32(const char* prompt) {
    uint32_t val = 0;
    for (;;) {
        printf("%s", prompt);
        if (scanf("%u", &val) == 1) return val;
        int c; while ((c = getchar()) != '\n' && c != EOF) {}
        printf("  Invalid input, please enter a number.\n");
    }
}

// Read a non-empty string of 1..maxLen alphanumeric/dash/hyphen chars
static void readString(const char* prompt, char* buf, size_t bufSize) {
    for (;;) {
        printf("%s", prompt);
        char tmp[256] = {};
        if (scanf("%255s", tmp) != 1 || tmp[0] == '\0') {
            int c; while ((c = getchar()) != '\n' && c != EOF) {}
            printf("  Input cannot be empty.\n");
            continue;
        }
        if (strlen(tmp) >= bufSize) {
            printf("  Input too long (max %zu chars).\n", bufSize - 1);
            continue;
        }
        strncpy_s(buf, bufSize, tmp, _TRUNCATE);
        return;
    }
}

// Read a 2-4 letter uppercase ICAO code (e.g. CYYZ)
static void readICAO(const char* prompt, char* buf, size_t bufSize) {
    for (;;) {
        printf("%s", prompt);
        char tmp[256] = {};
        if (scanf("%255s", tmp) != 1 || tmp[0] == '\0') {
            int c; while ((c = getchar()) != '\n' && c != EOF) {}
            printf("  Input cannot be empty.\n");
            continue;
        }
        size_t len = strlen(tmp);
        if (len < 2 || len > 4) {
            printf("  ICAO code must be 2-4 characters (e.g. CYYZ).\n");
            continue;
        }
        bool valid = true;
        for (size_t i = 0; i < len; ++i) {
            if (!isalpha((unsigned char)tmp[i])) { valid = false; break; }
            tmp[i] = (char)toupper((unsigned char)tmp[i]);
        }
        if (!valid) {
            printf("  ICAO code must contain only letters.\n");
            continue;
        }
        strncpy_s(buf, bufSize, tmp, _TRUNCATE);
        return;
    }
}

// Read TAKEOFF or LANDING
static void readRunwayOp(const char* prompt, char* buf, size_t bufSize) {
    for (;;) {
        printf("%s", prompt);
        char tmp[256] = {};
        if (scanf("%255s", tmp) != 1 || tmp[0] == '\0') {
            int c; while ((c = getchar()) != '\n' && c != EOF) {}
            printf("  Input cannot be empty.\n");
            continue;
        }
        // uppercase for comparison
        for (size_t i = 0; tmp[i]; ++i)
            tmp[i] = (char)toupper((unsigned char)tmp[i]);
        if (strcmp(tmp, "TAKEOFF") == 0 || strcmp(tmp, "LANDING") == 0) {
            strncpy_s(buf, bufSize, tmp, _TRUNCATE);
            return;
        }
        printf("  Please enter TAKEOFF or LANDING.\n");
    }
}

static const char* const LOG_DIR = "logs";
static const char* const LOG_FILE = "logs/client.log";

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
    if (!m_log) {
        m_log = stderr;
    }
}

ClientApp::~ClientApp() {
    disconnect();
    if (m_log && m_log != stderr) {
        fclose(m_log);
    }
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

bool ClientApp::reconnect() {
    printf("Reconnecting to %s:%u ...\n", m_server_ip.c_str(), (unsigned)m_port);

    if (m_sock != INVALID_SOCKET) {
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
    }
    m_seq = 0;

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
    if (m_sock == INVALID_SOCKET) {
        return;
    }

    for (;;) {
        printf("\n--- Menu ---\n"
            "  1 = Submit flight plan\n"
            "  2 = Request takeoff slot\n"
            "  3 = Request landing slot\n"
            "  4 = Request dispatch package\n"
            "  5 = Reconnect\n"
            "  6 = Export log\n"
            "  0 = Exit\n"
            "> ");
        int choice = 0;
        if (scanf("%d", &choice) != 1) {
            int c; while ((c = getchar()) != '\n' && c != EOF) {}
            printf("  Invalid input, please enter a number.\n");
            continue;
        }
        if (choice == 0) {
            break;
        }

        switch (choice) {
        case 1: cmdSubmitFlightPlan();       break;
        case 2: cmdRequestTakeoffSlot();     break;
        case 3: cmdRequestLandingSlot();     break;
        case 4: cmdRequestDispatchPackage(); break;

        case 5:
            if (reconnect()) {
                printf("Session re-established.\n");
            }
            else {
                printf("Reconnect failed.\n");
            }
            break;

        case 6: {
            char dest[256] = {};
            printf("Export log to path: ");
            scanf("%255s", dest);
            if (exportLog(dest)) {
                printf("Log exported to %s\n", dest);
            }
            else {
                printf("Export failed.\n");
            }
            break;
        }

        default:
            printf("Unknown option\n");
            break;
        }
    }
}

// ============================================================
//  US-LOG-06 - export log
// ============================================================

bool ClientApp::exportLog(const char* dest_path) {
    if (!dest_path || dest_path[0] == '\0') {
        return false;
    }

    if (m_log && m_log != stderr) {
        fflush(m_log);
    }

    std::ifstream src(m_log_path, std::ios::binary);
    if (!src.is_open()) {
        return false;
    }

    std::ofstream dst(dest_path, std::ios::binary | std::ios::trunc);
    if (!dst.is_open()) {
        return false;
    }

    dst << src.rdbuf();
    return dst.good();
}

// ============================================================
//  Private - handshake
// ============================================================

bool ClientApp::doHelloAuth(SOCKET s) {
    if (!sendPacket(s, HELLO, nullptr, 0)) {
        return false;
    }
    Header h;
    if (!recvHeader(s, h) || h.command_id != HELLO_ACK) {
        return false;
    }
    std::vector<uint8_t> dummy;
    if (h.payload_length && !recvPayload(s, h.payload_length, dummy)) {
        return false;
    }
    logPacket("RX", HELLO_ACK, h.sequence, h.payload_length);

    if (!sendPacket(s, AUTH, nullptr, 0)) {
        return false;
    }
    if (!recvHeader(s, h) || h.command_id != AUTH_ACK) {
        return false;
    }
    if (h.payload_length && !recvPayload(s, h.payload_length, dummy)) {
        return false;
    }
    logPacket("RX", AUTH_ACK, h.sequence, h.payload_length);

    return true;
}

// ============================================================
//  Private - commands
// ============================================================

void ClientApp::cmdSubmitFlightPlan() {
    FlightPlanPayload fp = {};
    readString("Flight ID (e.g. AC101):              ", fp.flight_id, sizeof(fp.flight_id));
    readString("Aircraft ID (e.g. C-FABC):           ", fp.aircraft_id, sizeof(fp.aircraft_id));
    readICAO("Origin ICAO code (e.g. CYYZ):        ", fp.origin, sizeof(fp.origin));
    readICAO("Destination ICAO code (e.g. CYUL):   ", fp.destination, sizeof(fp.destination));
    fp.etd = readUint64("ETD in minutes since epoch (e.g. 1000): ");
    fp.eta = readUint64("ETA in minutes since epoch (e.g. 1060): ");
    readRunwayOp("Runway operation (TAKEOFF/LANDING):  ", fp.runway_op, sizeof(fp.runway_op));

    fp.etd = _byteswap_uint64(fp.etd);
    fp.eta = _byteswap_uint64(fp.eta);

    if (!sendPacket(m_sock, SUBMIT_FLIGHT_PLAN, (uint8_t*)&fp, sizeof(fp))) {
        return;
    }

    Header h;
    if (!recvHeader(m_sock, h) || h.command_id != FLIGHT_PLAN_RESP) {
        return;
    }
    std::vector<uint8_t> pl;
    if (!recvPayload(m_sock, h.payload_length, pl)) {
        return;
    }

    uint16_t rc = 0xFFFF;
    if (pl.size() >= 2) {
        rc = ntohs(*(uint16_t*)pl.data());
        printf("Result: %u\n", (unsigned)rc);
    }
    logPacket("RX", FLIGHT_PLAN_RESP, h.sequence, h.payload_length, rc);
}

void ClientApp::cmdRequestTakeoffSlot() {
    uint32_t etd = readUint32("ETD in minutes since epoch (e.g. 1000): ");
    etd = htonl(etd);

    if (!sendPacket(m_sock, REQ_TAKEOFF_SLOT, (uint8_t*)&etd, 4)) {
        return;
    }

    Header h;
    if (!recvHeader(m_sock, h) || h.command_id != TAKEOFF_SLOT_RESP) {
        return;
    }
    std::vector<uint8_t> pl;
    if (!recvPayload(m_sock, h.payload_length, pl)) {
        return;
    }

    uint16_t rc = 0xFFFF;
    if (pl.size() >= 2) {
        rc = ntohs(*(uint16_t*)pl.data());
        printf("Result: %u", (unsigned)rc);
        if (pl.size() >= 18) {
            printf("  Runway: %.16s", pl.data() + 2);
        }
        printf("\n");
    }
    logPacket("RX", TAKEOFF_SLOT_RESP, h.sequence, h.payload_length, rc);
}

void ClientApp::cmdRequestLandingSlot() {
    uint32_t eta = readUint32("ETA in minutes since epoch (e.g. 1060): ");
    eta = htonl(eta);

    if (!sendPacket(m_sock, REQ_LANDING_SLOT, (uint8_t*)&eta, 4)) {
        return;
    }

    Header h;
    if (!recvHeader(m_sock, h) || h.command_id != LANDING_SLOT_RESP) {
        return;
    }
    std::vector<uint8_t> pl;
    if (!recvPayload(m_sock, h.payload_length, pl)) {
        return;
    }

    uint16_t rc = 0xFFFF;
    if (pl.size() >= 2) {
        rc = ntohs(*(uint16_t*)pl.data());
        printf("Result: %u", (unsigned)rc);
        if (pl.size() >= 18) {
            printf("  Runway: %.16s", pl.data() + 2);
        }
        printf("\n");
    }
    logPacket("RX", LANDING_SLOT_RESP, h.sequence, h.payload_length, rc);
}

void ClientApp::cmdRequestDispatchPackage() {
    if (!sendPacket(m_sock, REQ_DISPATCH_PKG, nullptr, 0)) {
        return;
    }

    std::ofstream out("dispatch_package.bin", std::ios::binary);
    if (!out) {
        printf("Cannot create dispatch_package.bin\n");
        return;
    }

    size_t total_written = 0;
    while (total_written < DISPATCH_PKG_SIZE) {
        Header h;
        if (!recvHeader(m_sock, h) || h.command_id != DISPATCH_CHUNK) {
            break;
        }
        std::vector<uint8_t> pl;
        if (!recvPayload(m_sock, h.payload_length, pl)) {
            break;
        }
        logPacket("RX", DISPATCH_CHUNK, h.sequence, h.payload_length);

        if (pl.size() < sizeof(ChunkMeta)) {
            break;
        }
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

void ClientApp::rotateLogIfNeeded() {
    if (!m_log || m_log == stderr) {
        return;
    }

    long pos = ftell(m_log);
    if (pos < m_max_log_bytes) {
        return;
    }

    fclose(m_log);
    m_log = nullptr;

    time_t t = time(nullptr);
    struct tm tm_buf;
    localtime_s(&tm_buf, &t);
    char archive[256];
    strftime(archive, sizeof(archive),
        "logs/client_%Y%m%d_%H%M%S.log", &tm_buf);

    rename(m_log_path.c_str(), archive);
    printf("[log] Rotated log to %s\n", archive);

    m_log = fopen(m_log_path.c_str(), "a");
    if (!m_log) {
        m_log = stderr;
    }
}

// ============================================================
//  Private - logging
// ============================================================

void ClientApp::logPacket(const char* dir, uint16_t cmd, uint32_t seq,
    uint32_t payload_sz, uint16_t result) {
    rotateLogIfNeeded();

    if (!m_log) {
        return;
    }
    time_t t = time(nullptr);
    struct tm tm_buf;
    localtime_s(&tm_buf, &t);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);

    if (result != 0xFFFF) {
        fprintf(m_log, "%s %s CMD=%u SEQ=%u PAYLOAD=%u RESULT=%u\n",
            ts, dir, (unsigned)cmd, seq, payload_sz, (unsigned)result);
    }
    else {
        fprintf(m_log, "%s %s CMD=%u SEQ=%u PAYLOAD=%u\n",
            ts, dir, (unsigned)cmd, seq, payload_sz);
    }
    fflush(m_log);
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

    if (send(s, (const char*)&h, sizeof(h), 0) != (int)sizeof(h)) {
        return false;
    }
    if (payload && payload_len &&
        send(s, (const char*)payload, payload_len, 0) != (int)payload_len) {
        return false;
    }

    logPacket("TX", cmd, m_seq, payload_len);
    return true;
}

bool ClientApp::recvHeader(SOCKET s, Header& h) {
    if (recv(s, (char*)&h, sizeof(h), 0) != (int)sizeof(h)) {
        return false;
    }
    header_to_host(h);
    return true;
}

bool ClientApp::recvPayload(SOCKET s, uint32_t len, std::vector<uint8_t>& out) {
    out.resize(len);
    if (len == 0) {
        return true;
    }
    size_t total = 0;
    while (total < len) {
        int n = recv(s, (char*)out.data() + total, (int)(len - total), 0);
        if (n <= 0) {
            return false;
        }
        total += n;
    }
    return true;
}