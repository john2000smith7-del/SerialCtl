#include "PowerService.h"
#include <iostream>
#include <stdexcept>

#ifdef SERIALCTL_POWER_TESTING
#error This test must verify production transports without simulation overrides.
#endif

int main() {
    try {
        serialctl::PowerService power;
        const auto initial = power.State();
        if (!serialctl::PowerService::ConnectionAvailable()) throw std::runtime_error("Real power connections remain disabled");
        for (const auto &backend : {"simulation", "", "tcp", "unknown"}) {
            for (int click = 0; click < 10; ++click) {
                const auto result = power.Submit({{"type", "connect"}, {"backend", backend},
                    {"resource", "USB::must-not-open"}, {"port", "COM1"}});
                if (!result.contains("error") ||
                    result.contains("id") || power.State() != initial)
                    throw std::runtime_error("Unsupported/simulated production backend was queued or changed state");
            }
        }
        std::cout << "Production USB/serial enabled; unsupported and simulated transports rejected; no hardware opened\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
