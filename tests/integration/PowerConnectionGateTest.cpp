#include "PowerService.h"
#include <iostream>
#include <stdexcept>

#ifdef SERIALCTL_POWER_TESTING
#error This test must verify the production connection gate without test overrides.
#endif

int main() {
    try {
        serialctl::PowerService power;
        const auto initial = power.State();
        for (const auto &backend : {"usb", "serial", "simulation", ""}) {
            for (int click = 0; click < 10; ++click) {
                const auto result = power.Submit({{"type", "connect"}, {"backend", backend},
                    {"resource", "USB::blocked"}, {"port", "COM1"}});
                if (result.value("code", "") != "FEATURE_NOT_AVAILABLE" ||
                    result.value("error", "") != serialctl::PowerService::ConnectionUnavailableMessage() ||
                    result.contains("id") || power.State() != initial)
                    throw std::runtime_error("Unavailable power connection was queued or changed device state");
            }
        }
        std::cout << "Production power connection gate passed; no hardware opened\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
