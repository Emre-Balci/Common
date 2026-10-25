#include "common/file/serial_port.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>

#include "detail/baud_rates.hpp"
#include "detail/posix_status.hpp"

namespace common {
namespace {

// An EINTR restart or a spurious wake-up makes no progress; the bound only guards against a signal
// storm or a misbehaving driver.
constexpr std::uint32_t kMaxStalls{1000U};

// Linux transfers at most 0x7ffff000 bytes per read(2)/write(2) call.
constexpr std::size_t kMaxTransfer{0x7FFFF000U};

// Bits that select the character format, parity, stop bits and flow control in c_cflag.
constexpr tcflag_t kLineBits{tcflag_t{CSIZE} | tcflag_t{PARENB} | tcflag_t{PARODD} | tcflag_t{CSTOPB} |
                             tcflag_t{CRTSCTS}};

/// Maps errno to a Status, with the meanings errors have for a terminal device.
Status serial_status(const int error) noexcept {
    Status status{Status::kSystemError};
    switch (error) {
        case EIO:     // A hung-up terminal.
        case ENXIO:   // The device behind the node is gone.
        case ENODEV:  // Likewise, e.g. an unplugged USB adapter.
            status = Status::kDisconnected;
            break;
        case EBUSY:  // Another handle holds the port exclusively (TIOCEXCL).
            status = Status::kResourceLimit;
            break;
        case ENOTTY:  // Not a terminal device.
            status = Status::kInvalidArgument;
            break;
        default:
            status = detail::status_from_errno(error);
            break;
    }
    return status;
}

Status last_error() noexcept {
    return serial_status(errno);
}

std::size_t transfer_size(const std::size_t requested) noexcept {
    return (requested < kMaxTransfer) ? requested : kMaxTransfer;
}

/// Runs a call that returns 0 on success or -1 with errno, restarting it after EINTR (bounded).
template <typename Call>
Status call_with_restart(const Call& call) noexcept {
    Status status{Status::kInterrupted};
    for (std::uint32_t attempt{0U}; attempt < kMaxStalls; ++attempt) {
        if (call() == 0) {
            status = Status::kOk;
            break;
        }
        if (errno != EINTR) {
            status = last_error();
            break;
        }
    }
    return status;
}

speed_t to_speed(const BaudRate baud_rate) noexcept {
    speed_t speed{B38400};
    switch (baud_rate) {
        case BaudRate::k1200:
            speed = B1200;
            break;
        case BaudRate::k2400:
            speed = B2400;
            break;
        case BaudRate::k4800:
            speed = B4800;
            break;
        case BaudRate::k9600:
            speed = B9600;
            break;
        case BaudRate::k19200:
            speed = B19200;
            break;
        case BaudRate::k38400:
            speed = B38400;
            break;
        case BaudRate::k57600:
            speed = B57600;
            break;
        case BaudRate::k115200:
            speed = B115200;
            break;
        case BaudRate::k230400:
            speed = B230400;
            break;
        case BaudRate::k460800:
            speed = B460800;
            break;
        case BaudRate::k500000:
            speed = B500000;
            break;
        case BaudRate::k576000:
            speed = B576000;
            break;
        case BaudRate::k921600:
            speed = B921600;
            break;
        case BaudRate::k1000000:
            speed = B1000000;
            break;
        case BaudRate::k1152000:
            speed = B1152000;
            break;
        case BaudRate::k1500000:
            speed = B1500000;
            break;
        case BaudRate::k2000000:
            speed = B2000000;
            break;
        case BaudRate::k2500000:
            speed = B2500000;
            break;
        case BaudRate::k3000000:
            speed = B3000000;
            break;
        case BaudRate::k3500000:
            speed = B3500000;
            break;
        case BaudRate::k4000000:
            speed = B4000000;
            break;
    }
    return speed;
}

/// @return the BaudRate for a termios speed; kNotSupported for a speed outside the enumeration.
Result<BaudRate> from_speed(const speed_t speed) noexcept {
    Result<BaudRate> result{Result<BaudRate>::failure(Status::kNotSupported)};
    for (const BaudRate candidate : detail::kAllBaudRates) {
        if (to_speed(candidate) == speed) {
            result = Result<BaudRate>::success(candidate);
            break;
        }
    }
    return result;
}

/// Sets @p speed in both directions of @p settings.
Status set_speed(termios& settings, const speed_t speed) noexcept {
    const bool ok{(::cfsetispeed(&settings, speed) == 0) && (::cfsetospeed(&settings, speed) == 0)};
    return ok ? Status::kOk : Status::kNotSupported;
}

/// Applies @p wanted with tcsetattr() and reads the settings back: tcsetattr() succeeds when any
/// part of a request was applied, so a rejected setting only shows in the read-back.
Status apply(const std::int32_t descriptor, const int when, const termios& wanted) noexcept {
    Status status{call_with_restart([descriptor, when, &wanted]() noexcept {
        return ::tcsetattr(descriptor, when, &wanted);
    })};
    if (is_ok(status)) {
        termios actual{};
        status = (::tcgetattr(descriptor, &actual) == 0) ? Status::kOk : last_error();
        if (is_ok(status)) {
            const bool same{(::cfgetospeed(&actual) == ::cfgetospeed(&wanted)) &&
                            (::cfgetispeed(&actual) == ::cfgetispeed(&wanted)) &&
                            ((actual.c_cflag & kLineBits) == (wanted.c_cflag & kLineBits))};
            status = same ? Status::kOk : Status::kNotSupported;
        }
    }
    return status;
}

/// Raw mode with the line settings of @p options.
Status raw_settings(const std::int32_t descriptor, const SerialOptions& options, termios& settings) noexcept {
    Status status{(::tcgetattr(descriptor, &settings) == 0) ? Status::kOk : last_error()};
    if (is_ok(status)) {
        ::cfmakeraw(&settings);  // No echo, line editing, translation or signals; 8 data bits.
        settings.c_iflag &= ~(tcflag_t{IXON} | tcflag_t{IXOFF} | tcflag_t{IXANY});
        settings.c_cflag &= ~kLineBits;
        settings.c_cflag |= tcflag_t{CS8} | tcflag_t{CLOCAL} | tcflag_t{CREAD};
        switch (options.parity) {
            case Parity::kNone:
                break;
            case Parity::kEven:
                settings.c_cflag |= tcflag_t{PARENB};
                settings.c_iflag |= tcflag_t{INPCK} | tcflag_t{IGNPAR};  // Drop bytes with a parity error.
                break;
            case Parity::kOdd:
                settings.c_cflag |= tcflag_t{PARENB} | tcflag_t{PARODD};
                settings.c_iflag |= tcflag_t{INPCK} | tcflag_t{IGNPAR};
                break;
        }
        if (options.stop_bits == StopBits::kTwo) {
            settings.c_cflag |= tcflag_t{CSTOPB};
        }
        if (options.flow_control == FlowControl::kRtsCts) {
            settings.c_cflag |= tcflag_t{CRTSCTS};
        }
        // With O_NONBLOCK and VMIN = 1, a read without data fails with EAGAIN, so a read that
        // returns 0 can only mean a hang-up.
        settings.c_cc[VMIN] = 1U;
        settings.c_cc[VTIME] = 0U;
        status = set_speed(settings, to_speed(options.baud_rate));
    }
    return status;
}

/// Converts a non-negative duration to a timespec.
timespec to_relative_timespec(const Duration duration) noexcept {
    const std::int64_t ns{duration.count()};
    timespec ts{};
    ts.tv_sec = ns / MonotonicClock::kNanosPerSecond;
    ts.tv_nsec = ns % MonotonicClock::kNanosPerSecond;
    return ts;
}

/// Waits until @p descriptor is ready for @p events or @p deadline passes.
/// @return kOk when ready; kTimeout; kDisconnected on a hang-up or device error; kInterrupted when a
///         signal ended the wait (the caller retries); kInvalidArgument for a bad descriptor.
Status wait_ready(const std::int32_t descriptor, const short events, const Deadline& deadline) noexcept {
    pollfd entry{};
    entry.fd = descriptor;
    entry.events = events;
    timespec timeout{};
    const timespec* timeout_pointer{nullptr};
    if (!deadline.is_never()) {
        timeout = to_relative_timespec(deadline.remaining());
        timeout_pointer = &timeout;
    }
    const int ready{::ppoll(&entry, 1U, timeout_pointer, nullptr)};
    Status status{Status::kTimeout};
    if (ready < 0) {
        status = (errno == EINTR) ? Status::kInterrupted : last_error();
    } else if (ready == 0) {
        status = Status::kTimeout;
    } else if ((static_cast<std::uint32_t>(entry.revents) & static_cast<std::uint32_t>(events)) != 0U) {
        status = Status::kOk;
    } else if ((static_cast<std::uint32_t>(entry.revents) & std::uint32_t{POLLNVAL}) != 0U) {
        status = Status::kInvalidArgument;
    } else {
        status = Status::kDisconnected;  // POLLHUP or POLLERR without the requested readiness.
    }
    return status;
}

/// Decides how SerialPort::read() goes on after a read(2) that returned no data: @p count is its
/// result (0 or -1) and @p error the errno it set.
/// @return kOk or kInterrupted to read again; any other Status ends the read with that error.
Status after_empty_read(const ssize_t count, const int error, const std::int32_t descriptor,
                        const Deadline& deadline) noexcept {
    Status status{Status::kInterrupted};
    if (count == 0) {
        status = Status::kDisconnected;  // A hang-up: see raw_settings().
    } else if (error == EINTR) {
        status = Status::kInterrupted;
    } else if (error != EAGAIN) {
        status = serial_status(error);
    } else if (deadline.expired()) {
        status = Status::kTimeout;
    } else {
        status = wait_ready(descriptor, POLLIN, deadline);
    }
    return status;
}

/// Discards the queued bytes selected by @p queue (TCIFLUSH or TCOFLUSH).
Status flush_queue(const std::int32_t descriptor, const int queue) noexcept {
    return (::tcflush(descriptor, queue) == 0) ? Status::kOk : last_error();
}

}  // namespace

SerialPort::~SerialPort() {
    static_cast<void>(close());
}

SerialPort::SerialPort(SerialPort&& other) noexcept : descriptor_{other.descriptor_}, exclusive_{other.exclusive_} {
    other.descriptor_ = -1;
    other.exclusive_ = false;
}

SerialPort& SerialPort::operator=(SerialPort&& other) noexcept {
    if (this != &other) {
        static_cast<void>(close());
        descriptor_ = other.descriptor_;
        exclusive_ = other.exclusive_;
        other.descriptor_ = -1;
        other.exclusive_ = false;
    }
    return *this;
}

Status SerialPort::open(const char* const path, const SerialOptions& options) noexcept {
    Status status{((path == nullptr) || is_open()) ? Status::kInvalidArgument : Status::kOk};
    int descriptor{-1};
    if (is_ok(status)) {
        // O_NONBLOCK: neither open() nor later reads wait for the modem control lines or for data.
        const int flags{static_cast<int>(static_cast<std::uint32_t>(O_RDWR) | static_cast<std::uint32_t>(O_NOCTTY) |
                                         static_cast<std::uint32_t>(O_CLOEXEC) | static_cast<std::uint32_t>(O_NONBLOCK))};
        status = Status::kInterrupted;
        for (std::uint32_t attempt{0U}; attempt < kMaxStalls; ++attempt) {
            // open(2) is variadic in C; it is the only way to open a device.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
            descriptor = ::open(path, flags);
            if (descriptor >= 0) {
                status = Status::kOk;
                break;
            }
            if (errno != EINTR) {
                status = last_error();
                break;
            }
        }
    }
    if (is_ok(status) && options.exclusive) {
        // ioctl(2) is variadic in C; TIOCEXCL takes no argument.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
        status = (::ioctl(descriptor, TIOCEXCL) == 0) ? Status::kOk : last_error();
    }
    termios settings{};
    if (is_ok(status)) {
        status = raw_settings(descriptor, options, settings);
    }
    if (is_ok(status)) {
        status = apply(descriptor, TCSANOW, settings);
    }
    if (is_ok(status)) {
        status = (::tcflush(descriptor, TCIOFLUSH) == 0) ? Status::kOk : last_error();
    }
    if (is_ok(status)) {
        descriptor_ = descriptor;
        exclusive_ = options.exclusive;
    } else if (descriptor >= 0) {
        // Closing a descriptor that failed after TIOCEXCL; release the flag like close() does.
        if (options.exclusive) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
            static_cast<void>(::ioctl(descriptor, TIOCNXCL));
        }
        static_cast<void>(::close(descriptor));
    } else {
        // Nothing was opened.
    }
    return status;
}

Status SerialPort::close() noexcept {
    Status status{Status::kOk};
    if (is_open()) {
        if (exclusive_) {
            // Best effort: the descriptor is closed whatever the outcome. ioctl(2) is variadic in C.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
            static_cast<void>(::ioctl(descriptor_, TIOCNXCL));
            exclusive_ = false;
        }
        // Linux releases the descriptor even when close() fails, so it is never retried.
        status = (::close(descriptor_) == 0) ? Status::kOk : last_error();
        descriptor_ = -1;
    }
    return status;
}

Status SerialPort::set_baud_rate(const BaudRate baud_rate) noexcept {
    Status status{is_open() ? Status::kOk : Status::kInvalidArgument};
    termios settings{};
    if (is_ok(status)) {
        status = (::tcgetattr(descriptor_, &settings) == 0) ? Status::kOk : last_error();
    }
    if (is_ok(status)) {
        status = set_speed(settings, to_speed(baud_rate));
    }
    if (is_ok(status)) {
        status = apply(descriptor_, TCSADRAIN, settings);
    }
    return status;
}

Result<BaudRate> SerialPort::baud_rate() const noexcept {
    Result<BaudRate> result{Result<BaudRate>::failure(Status::kInvalidArgument)};
    if (is_open()) {
        termios settings{};
        result = (::tcgetattr(descriptor_, &settings) == 0) ? from_speed(::cfgetospeed(&settings))
                                                           : Result<BaudRate>::failure(last_error());
    }
    return result;
}

Result<std::size_t> SerialPort::read(const Span<std::uint8_t> buffer, const Deadline& deadline) noexcept {
    Result<std::size_t> result{Result<std::size_t>::failure(Status::kInterrupted)};
    if (!is_open()) {
        result = Result<std::size_t>::failure(Status::kInvalidArgument);
    } else if (buffer.empty()) {
        result = Result<std::size_t>::success(0U);
    } else {
        // Each round either ends the call or is a stall (EINTR, or a wake-up without data).
        for (std::uint32_t stall{0U}; stall < kMaxStalls; ++stall) {
            const ssize_t count{::read(descriptor_, buffer.data(), transfer_size(buffer.size()))};
            if (count > 0) {
                result = Result<std::size_t>::success(static_cast<std::size_t>(count));
                break;
            }
            const Status next{after_empty_read(count, errno, descriptor_, deadline)};
            if ((!is_ok(next)) && (next != Status::kInterrupted)) {
                result = Result<std::size_t>::failure(next);
                break;
            }
        }
    }
    return result;
}

Status SerialPort::write_all(const Span<const std::uint8_t> data, const Deadline& deadline) noexcept {
    Status status{is_open() ? Status::kOk : Status::kInvalidArgument};
    std::size_t written{0U};
    std::uint32_t stalls{0U};
    // Each round writes at least one byte or is a stall, so data.size() + kMaxStalls rounds suffice.
    const std::size_t max_rounds{data.size() + kMaxStalls};
    for (std::size_t round{0U}; is_ok(status) && (written < data.size()) && (round < max_rounds); ++round) {
        const Span<const std::uint8_t> rest{data.subspan(written)};
        const ssize_t count{::write(descriptor_, rest.data(), transfer_size(rest.size()))};
        if (count > 0) {
            written += static_cast<std::size_t>(count);
        } else if ((count < 0) && (errno != EAGAIN) && (errno != EINTR)) {
            status = last_error();
        } else {
            ++stalls;
            if (stalls >= kMaxStalls) {
                status = Status::kInterrupted;
            } else if ((count < 0) && (errno == EINTR)) {
                // Restart the write.
            } else {
                status = deadline.expired() ? Status::kTimeout : wait_ready(descriptor_, POLLOUT, deadline);
                if (status == Status::kInterrupted) {
                    status = Status::kOk;  // A signal ended the wait: write again.
                }
            }
        }
    }
    return status;
}

Status SerialPort::drain() noexcept {
    Status status{Status::kInvalidArgument};
    if (is_open()) {
        const std::int32_t descriptor{descriptor_};
        status = call_with_restart([descriptor]() noexcept { return ::tcdrain(descriptor); });
    }
    return status;
}

Status SerialPort::discard_input() noexcept {
    return is_open() ? flush_queue(descriptor_, TCIFLUSH) : Status::kInvalidArgument;
}

Status SerialPort::discard_output() noexcept {
    return is_open() ? flush_queue(descriptor_, TCOFLUSH) : Status::kInvalidArgument;
}

}  // namespace common
