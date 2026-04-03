#include "ClientApp.h"

static const char* DEFAULT_SERVER = "127.0.0.1";

int main(int argc, char* argv[]) {
    const char* server_ip = (argc > 1) ? argv[1] : DEFAULT_SERVER;
    uint16_t    port = (argc > 2) ? (uint16_t)atoi(argv[2]) : proto::DEFAULT_PORT;

    ClientApp app;
    if (!app.connect(server_ip, port))
        return 1;

    app.run();
    app.disconnect();
    return 0;
}
