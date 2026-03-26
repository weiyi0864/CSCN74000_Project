#include "ServerApp.h"

int main() {
    ServerApp app;
    if (!app.start())
        return 1;

    app.run();
    app.stop();
    return 0;
}
