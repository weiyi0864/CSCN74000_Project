#pragma once
#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include "Protocol.h"
#include <vector>
#include <string>
#include <cstdio>

class ServerApp {
public:
    // Server state machine states
    enum State {
        STATE_INIT,
        STATE_LISTENING,
        STATE_CONNECTED,
        STATE_VERIFIED,
        STATE_TRANSFERRING,
        STATE_ERROR
    };

    ServerApp();
    ~ServerApp();

    bool start(uint16_t port = proto::DEFAULT_PORT, int timeout_sec = proto::DEFAULT_TIMEOUT_SEC);
    void run();
    void stop();

    // Expose state for unit testing
    State getState() const { return m_state; }

private:
    // --- Network helpers ---
    bool sendPacket(SOCKET s, proto::CmdId cmd, const uint8_t* payload, uint32_t payload_len, int result_code = -1);
    bool recvHeader(SOCKET s, proto::Header& h);
    bool recvPayload(SOCKET s, uint32_t len, std::vector<uint8_t>& out);

    // --- Connection lifecycle ---
    bool waitForClient();
    void handleClient();
    void resetConnection();

    // --- Command handlers ---
    void handleHello(SOCKET s, const proto::Header& h);
    void handleAuth(SOCKET s, const proto::Header& h);
    void handleSubmitFlightPlan(SOCKET s, const proto::Header& h, const std::vector<uint8_t>& payload);
    void handleReqTakeoffSlot(SOCKET s, const proto::Header& h, const std::vector<uint8_t>& payload);
    void handleReqLandingSlot(SOCKET s, const proto::Header& h, const std::vector<uint8_t>& payload);
    void handleReqDispatchPkg(SOCKET s);

    // --- Business logic ---
    bool     validateFlightPlan(const proto::FlightPlanPayload& fp);
    uint16_t allocTakeoffSlot(uint32_t etd, char* runway_out);
    uint16_t allocLandingSlot(uint32_t eta, char* runway_out);

    // --- State machine ---
    bool transitionTo(State next);
    static const char* stateName(State s);

    // --- Logging ---
    void logPacket(const char* dir, uint16_t cmd, uint32_t seq, uint32_t payload_sz, uint16_t result = 0xFFFF);

    // --- Members ---
    SOCKET      m_listen_sock;
    SOCKET      m_client_sock;
    State       m_state;
    uint32_t    m_seq;
    FILE* m_log;
    int         m_timeout_sec;
    bool        m_transfer_busy;
    std::vector<proto::FlightPlanPayload> m_flight_plans;
};
