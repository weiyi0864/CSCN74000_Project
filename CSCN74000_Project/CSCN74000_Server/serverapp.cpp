#include "ServerApp.h"
#include <ctime>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <direct.h>
#include <iostream>

using namespace proto;

static const char* LOG_DIR = "logs";
static const char* LOG_FILE = "logs/server.log";

// ============================================================
//  Constructor / Destructor
// ============================================================

ServerApp::ServerApp()
    : m_listen_sock(INVALID_SOCKET)
    , m_client_sock(INVALID_SOCKET)
    , m_state(STATE_INIT)
    , m_seq(0)
    , m_log(nullptr)
    , m_timeout_sec(DEFAULT_TIMEOUT_SEC)
    , m_transfer_busy(false)
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    _mkdir(LOG_DIR);
    m_log = fopen(LOG_FILE, "a");
    if (!m_log) m_log = stderr;
}

ServerApp::~ServerApp() {
    stop();
    if (m_log && m_log != stderr) fclose(m_log);
    WSACleanup();
}

// ============================================================
//  Public interface
// ============================================================

bool ServerApp::start(uint16_t port, int timeout_sec) {
    m_timeout_sec = timeout_sec;

    m_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listen_sock == INVALID_SOCKET) {
        printf("socket() failed\n");
        return false;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(m_listen_sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        printf("bind() failed\n");
        return false;
    }
    listen(m_listen_sock, 5);

    transitionTo(STATE_LISTENING);
    printf("Server listening on port %u\n", (unsigned)port);
    return true;
}

void ServerApp::run() {
    while (m_state != STATE_ERROR) {
        if (m_state == STATE_LISTENING) {
            if (!waitForClient()) continue;
        }
        handleClient();
        resetConnection();
    }
}

void ServerApp::stop() {
    if (m_client_sock != INVALID_SOCKET) {
        closesocket(m_client_sock);
        m_client_sock = INVALID_SOCKET;
    }
    if (m_listen_sock != INVALID_SOCKET) {
        closesocket(m_listen_sock);
        m_listen_sock = INVALID_SOCKET;
    }
}

// ============================================================
//  Private – connection lifecycle
// ============================================================

bool ServerApp::waitForClient() {
    printf("Waiting for connection...\n");
    m_client_sock = accept(m_listen_sock, nullptr, nullptr);
    if (m_client_sock == INVALID_SOCKET) return false;

    DWORD to = (DWORD)(m_timeout_sec * 1000);
    setsockopt(m_client_sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&to, sizeof(to));

    transitionTo(STATE_CONNECTED);
    printf("Client connected\n");
    return true;
}

void ServerApp::handleClient() {
    while (m_state == STATE_CONNECTED ||
        m_state == STATE_VERIFIED ||
        m_state == STATE_TRANSFERRING)
    {
        Header h;
        if (!recvHeader(m_client_sock, h)) {
            printf("Client disconnected or recv error\n");
            return;
        }

        // Basic validation
        if (h.version != VERSION || h.payload_length > 1024 * 1024) {
            sendPacket(m_client_sock, ERROR_RESP,
                (const uint8_t*)&h.command_id, 2, ERR_INVALID_PACKET);
            continue;
        }

        std::vector<uint8_t> payload;
        if (h.payload_length && !recvPayload(m_client_sock, h.payload_length, payload)) {
            return;
        }

        // Checksum verification
        uint32_t recv_cs = payload.empty() ? 0 : checksum(payload.data(), (uint32_t)payload.size());
        if (recv_cs != h.checksum) {
            sendPacket(m_client_sock, ERROR_RESP, nullptr, 0, ERR_INVALID_PACKET);
            continue;
        }

        logPacket("RX", (CmdId)h.command_id, h.sequence, h.payload_length);

        switch ((CmdId)h.command_id) {
        case HELLO:             handleHello(m_client_sock, h);                         break;
        case AUTH:              handleAuth(m_client_sock, h);                          break;
        case SUBMIT_FLIGHT_PLAN:handleSubmitFlightPlan(m_client_sock, h, payload);     break;
        case REQ_TAKEOFF_SLOT:  handleReqTakeoffSlot(m_client_sock, h, payload);       break;
        case REQ_LANDING_SLOT:  handleReqLandingSlot(m_client_sock, h, payload);       break;
        case REQ_DISPATCH_PKG:  handleReqDispatchPkg(m_client_sock);                   break;
        default:
            printf("Unknown command %u – ignored\n", (unsigned)h.command_id);
            break;
        }
    }
}

void ServerApp::resetConnection() {
    if (m_client_sock != INVALID_SOCKET) {
        closesocket(m_client_sock);
        m_client_sock = INVALID_SOCKET;
    }
    m_transfer_busy = false;
    m_seq = 0;
    transitionTo(STATE_LISTENING);
}

// ============================================================
//  Private – command handlers
// ============================================================

void ServerApp::handleHello(SOCKET s, const Header& /*h*/) {
    if (m_state != STATE_CONNECTED) return;
    sendPacket(s, HELLO_ACK, nullptr, 0);
}

void ServerApp::handleAuth(SOCKET s, const Header& /*h*/) {
    if (m_state != STATE_CONNECTED) return;
    sendPacket(s, AUTH_ACK, nullptr, 0);
    transitionTo(STATE_VERIFIED);
}

void ServerApp::handleSubmitFlightPlan(SOCKET s, const Header& /*h*/,
    const std::vector<uint8_t>& payload) {
    if (m_state != STATE_VERIFIED) return;

    if (payload.size() < sizeof(FlightPlanPayload)) {
        uint16_t rc = htons(ERR_INVALID_PACKET);
        sendPacket(s, FLIGHT_PLAN_RESP, (uint8_t*)&rc, 2, ERR_INVALID_PACKET);
        return;
    }

    FlightPlanPayload fp;
    memcpy(&fp, payload.data(), sizeof(fp));
    fp.etd = _byteswap_uint64(fp.etd);
    fp.eta = _byteswap_uint64(fp.eta);

    if (validateFlightPlan(fp)) {
        m_flight_plans.push_back(fp);
        uint16_t rc = htons(OK);
        sendPacket(s, FLIGHT_PLAN_RESP, (uint8_t*)&rc, 2, OK);
    }
    else {
        uint16_t rc = htons(ERR_VALIDATION);
        sendPacket(s, FLIGHT_PLAN_RESP, (uint8_t*)&rc, 2, ERR_VALIDATION);
    }
}

void ServerApp::handleReqTakeoffSlot(SOCKET s, const Header& /*h*/,
    const std::vector<uint8_t>& payload) {
    if (m_state != STATE_VERIFIED) return;

    if (m_transfer_busy) {
        uint16_t rc = htons(ERR_BUSY);
        sendPacket(s, TAKEOFF_SLOT_RESP, (uint8_t*)&rc, 2, ERR_BUSY);
        return;
    }
    if (payload.size() < 4) return;

    uint32_t etd;
    memcpy(&etd, payload.data(), 4);
    etd = ntohl(etd);

    char rwy[16];
    uint16_t rc = allocTakeoffSlot(etd, rwy);

    std::vector<uint8_t> resp(2 + 16);
    uint16_t rc_net = htons(rc);
    memcpy(resp.data(), &rc_net, 2);
    memcpy(resp.data() + 2, rwy, 16);
    sendPacket(s, TAKEOFF_SLOT_RESP, resp.data(), (uint32_t)resp.size(), rc);
}

void ServerApp::handleReqLandingSlot(SOCKET s, const Header& /*h*/,
    const std::vector<uint8_t>& payload) {
    if (m_state != STATE_VERIFIED) return;

    if (m_transfer_busy) {
        uint16_t rc = htons(ERR_BUSY);
        sendPacket(s, LANDING_SLOT_RESP, (uint8_t*)&rc, 2, ERR_BUSY);
        return;
    }
    if (payload.size() < 4) return;

    uint32_t eta;
    memcpy(&eta, payload.data(), 4);
    eta = ntohl(eta);

    char rwy[16];
    uint16_t rc = allocLandingSlot(eta, rwy);

    std::vector<uint8_t> resp(2 + 16);
    uint16_t rc_net = htons(rc);
    memcpy(resp.data(), &rc_net, 2);
    memcpy(resp.data() + 2, rwy, 16);
    sendPacket(s, LANDING_SLOT_RESP, resp.data(), (uint32_t)resp.size(), rc);
}

void ServerApp::handleReqDispatchPkg(SOCKET s) {
    if (m_state != STATE_VERIFIED) return;

    m_transfer_busy = true;
    transitionTo(STATE_TRANSFERRING);

    // Generate 1 MB synthetic dispatch package
    std::vector<uint8_t> pkg(DISPATCH_PKG_SIZE);
    for (size_t i = 0; i < pkg.size(); ++i) pkg[i] = (uint8_t)(i % 256);

    uint32_t total_chunks = (DISPATCH_PKG_SIZE + CHUNK_SIZE - 1) / CHUNK_SIZE;
    for (uint32_t i = 0; i < total_chunks; ++i) {
        uint32_t off = i * CHUNK_SIZE;
        uint32_t len = (off + CHUNK_SIZE <= (uint32_t)pkg.size())
            ? CHUNK_SIZE
            : (uint32_t)(pkg.size() - off);

        std::vector<uint8_t> chunk_payload(sizeof(ChunkMeta) + len);
        ChunkMeta meta;
        meta.chunk_index = htonl(i);
        meta.total_chunks = htonl(total_chunks);
        meta.total_size = htonl(DISPATCH_PKG_SIZE);
        memcpy(chunk_payload.data(), &meta, sizeof(meta));
        memcpy(chunk_payload.data() + sizeof(meta), pkg.data() + off, len);

        if (!sendPacket(s, DISPATCH_CHUNK, chunk_payload.data(), (uint32_t)chunk_payload.size()))
            break;
    }

    m_transfer_busy = false;
    transitionTo(STATE_VERIFIED);
}

// ============================================================
//  Private – business logic
// ============================================================

bool ServerApp::validateFlightPlan(const FlightPlanPayload& fp) {
    return fp.flight_id[0] && fp.aircraft_id[0] &&
        fp.origin[0] && fp.destination[0];
}

uint16_t ServerApp::allocTakeoffSlot(uint32_t /*etd*/, char* runway_out) {
    snprintf(runway_out, 16, "R01");
    return (uint16_t)OK;
}

uint16_t ServerApp::allocLandingSlot(uint32_t /*eta*/, char* runway_out) {
    snprintf(runway_out, 16, "R02");
    return (uint16_t)OK;
}

// ============================================================
//  Private – state machine
// ============================================================

bool ServerApp::transitionTo(State next) {
    printf("State: %s -> %s\n", stateName(m_state), stateName(next));
    m_state = next;
    return true;
}

const char* ServerApp::stateName(State s) {
    switch (s) {
    case STATE_INIT:         return "INIT";
    case STATE_LISTENING:    return "LISTENING";
    case STATE_CONNECTED:    return "CONNECTED";
    case STATE_VERIFIED:     return "VERIFIED";
    case STATE_TRANSFERRING: return "TRANSFERRING";
    case STATE_ERROR:        return "ERROR";
    default:                 return "UNKNOWN";
    }
}

// ============================================================
//  Private – network helpers
// ============================================================

bool ServerApp::sendPacket(SOCKET s, CmdId cmd, const uint8_t* payload,
    uint32_t payload_len, int result_code) {
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

    if (result_code >= 0)
        logPacket("TX", cmd, m_seq, payload_len, (uint16_t)result_code);
    else
        logPacket("TX", cmd, m_seq, payload_len);
    return true;
}

bool ServerApp::recvHeader(SOCKET s, Header& h) {
    if (recv(s, (char*)&h, sizeof(h), 0) != (int)sizeof(h)) return false;
    header_to_host(h);
    return true;
}

bool ServerApp::recvPayload(SOCKET s, uint32_t len, std::vector<uint8_t>& out) {
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

// ============================================================
//  Private – logging
// ============================================================

void ServerApp::logPacket(const char* dir, uint16_t cmd, uint32_t seq,
    uint32_t payload_sz, uint16_t result) {
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
    fflush(m_log);
}
