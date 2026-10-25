#include "common/file/serial_port.hpp"

#include <cstdint>

#include "detail/baud_rates.hpp"

// The parts of serial_port.hpp that are the same on every platform; serial_port_posix.cpp and
// serial_port_win32.cpp implement the rest.

namespace common {

Result<BaudRate> baud_rate_from_bits_per_second(const std::uint32_t bits_per_second) noexcept {
    Result<BaudRate> result{Result<BaudRate>::failure(Status::kNotSupported)};
    // The enumerator value is the speed in bit/s.
    for (const BaudRate candidate : detail::kAllBaudRates) {
        if (static_cast<std::uint32_t>(candidate) == bits_per_second) {
            result = Result<BaudRate>::success(candidate);
            break;
        }
    }
    return result;
}

}  // namespace common
