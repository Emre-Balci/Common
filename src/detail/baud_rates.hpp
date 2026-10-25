/// @file baud_rates.hpp
/// @brief Internal: every BaudRate, for the serial port implementations.

#ifndef COMMON_SRC_DETAIL_BAUD_RATES_HPP
#define COMMON_SRC_DETAIL_BAUD_RATES_HPP

#include <array>

#include "common/file/serial_port.hpp"

namespace common {
namespace detail {

inline constexpr std::array<BaudRate, 21U> kAllBaudRates{
    {BaudRate::k1200,    BaudRate::k2400,    BaudRate::k4800,    BaudRate::k9600,    BaudRate::k19200,
     BaudRate::k38400,   BaudRate::k57600,   BaudRate::k115200,  BaudRate::k230400,  BaudRate::k460800,
     BaudRate::k500000,  BaudRate::k576000,  BaudRate::k921600,  BaudRate::k1000000, BaudRate::k1152000,
     BaudRate::k1500000, BaudRate::k2000000, BaudRate::k2500000, BaudRate::k3000000, BaudRate::k3500000,
     BaudRate::k4000000}};

}  // namespace detail
}  // namespace common

#endif  // COMMON_SRC_DETAIL_BAUD_RATES_HPP
