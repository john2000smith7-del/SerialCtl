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
        if (serialctl::PowerService::ConnectionAvailable()) throw std::runtime_error("Production power feature must remain disabled");
        if (serialctl::PowerService::VisaAvailable() || !serialctl::PowerService::UsbResources().empty())
            throw std::runtime_error("Disabled power feature must not expose VISA resources");
        for (const auto &backend : {"usb", "serial", "simulation", "", "tcp", "unknown"}) {
            for (int click = 0; click < 10; ++click) {
                const auto result = power.Submit({{"type", "connect"}, {"backend", backend},
                    {"resource", "USB::must-not-open"}, {"port", "COM1"}});
                if (!result.contains("error") || result.value("code", "") != "FEATURE_NOT_AVAILABLE" ||
                    result.contains("id") || power.State() != initial)
                    throw std::runtime_error("Disabled production backend was queued or changed state");
            }
        }
        for (const auto &type : {"disconnect", "recover", "output", "parameters", "protection", "task", "stop",
                                 "mode", "batch", "settings", "deviceTimer", "trigger", "diagnostic", "sequence",
                                 "schedule", "interval", "step"}) {
            for (const bool enabled : {false, true}) {
                const auto result = power.Submit({{"type", type}, {"enabled", enabled}, {"channels", {1, 2, 3}}});
                if (!result.contains("error") || result.value("code", "") != "FEATURE_NOT_AVAILABLE" ||
                    result.contains("id") || power.State() != initial)
                    throw std::runtime_error("Disabled power action was queued or changed state");
            }
        }
        if (power.AuthorizeTest(L"JJ123123") || power.AuthorizeTest(L"wrong") || serialctl::PowerService::ConnectionAvailable())
            throw std::runtime_error("Wrong Test password unlocked production power");
        if (!power.AuthorizeTest(L"jj123123") || !serialctl::PowerService::ConnectionAvailable())
            throw std::runtime_error("Correct Test password did not unlock the shared gate");
        // Authorization alone must not open, enumerate or alter any device.
        if (power.State() != initial) throw std::runtime_error("Unlock changed power state");
        power.DisableTest();
        if (serialctl::PowerService::ConnectionAvailable() || !power.Submit({{"type","output"},{"channels",{1}},{"enabled",true}}).contains("error"))
            throw std::runtime_error("Lock failed to reject subsequent commands");
        power.Shutdown();
        serialctl::PowerService restarted;
        if (serialctl::PowerService::ConnectionAvailable()) throw std::runtime_error("Test authorization survived locking/restart");
        std::cout << "Production power disabled: USB/serial/simulation and all actions rejected before queueing; no hardware opened\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
