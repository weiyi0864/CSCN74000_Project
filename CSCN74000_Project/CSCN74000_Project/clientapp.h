#pragma once
#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include "../CSCN74000_Server/Protocol.h"
#include <vector>
#include <string>
#include <cstdio>

class ClientApp {
public:
    ClientApp();
    ~ClientApp();

    bool connect(const char* server_ip, uint16_t port = proto::DEFAULT_PORT,
        int timeout_sec = proto::DEFAULT_TIMEOUT_SEC);

    // US-COM-05: reconnect and re-run HELLO/AUTH without restarting the app
    bool reconnect();

    void run();
    void disconnect();

    // US-LOG-06: copy current log file to dest_path as a portable export
    bool exportLog(const char* dest_path);

private:
    // --- Network helpers ---
    bool sendPacket(SOCKET s, proto::CmdId cmd, const uint8_t* payload, uint32_t payload_len);
    bool recvHeader(SOCKET s, proto::Header& h);
    bool recvPayload(SOCKET s, uint32_t len, std::vector<uint8_t>& out);

    // --- Auth ---
    bool doHelloAuth(SOCKET s);

    // --- Commands ---
    void cmdSubmitFlightPlan();
    void cmdRequestTakeoffSlot();
    void cmdRequestLandingSlot();
    void cmdRequestDispatchPackage();

    // --- Logging ---
    // US-LOG-05: rotate log if it exceeds m_max_log_bytes before writing
    void logPacket(const char* dir, uint16_t cmd, uint32_t seq,
        uint32_t payload_sz, uint16_t result = 0xFFFF);
    void rotateLogIfNeeded();

    // --- State ---
    SOCKET      m_sock;
    uint32_t    m_seq;
    FILE* m_log;
    int         m_timeout_sec;
    std::string m_server_ip;
    uint16_t    m_port;

    // US-LOG-05: configurable max log size in bytes (default 1 MB)
    static constexpr long MAX_LOG_BYTES = 1024 * 1024;
    long        m_max_log_bytes;
    std::string m_log_path;
};