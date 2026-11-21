#pragma once

#include <chrono>
#include <cstdint>
#include <stop_token>

#include "protocol/Registration.hpp"
#include "util/Result.hpp"

namespace veloplay::protocol {

struct RegistrationParams {
    std::uint32_t address = 0; // console IPv4, host byte order
    std::uint16_t port = kRegistPort;
    PsnAccountId account;
    std::uint32_t pin = 0;
    std::chrono::milliseconds searchTimeout{3000};
    std::chrono::milliseconds responseTimeout{5000};
};

/// Runs the whole pairing exchange and blocks until it finishes, fails or is
/// cancelled. The console only answers while its "Link Device" screen is open.
///
/// Error codes: Timeout (console silent), Rejected (console said no, message
/// carries its reason), Network, InvalidData, Canceled.
Result<RegisteredHost> registerWithConsole(const RegistrationParams& params, std::stop_token stop = {});

} // namespace veloplay::protocol
