#include "common/file/serial_port.hpp"

#include <cstddef>
#include <cstdint>

#include "detail/overlapped_io.hpp"
#include "detail/win32.hpp"
#include "detail/win32_status.hpp"

namespace common {
namespace {

// A read that the driver completes without data (its own timeout, after about 49 days) is repeated,
// and so is a write that completes without progress; the bound only guards against a misbehaving
// driver.
constexpr std::uint32_t kMaxStalls{1000U};

/// Maps the error of opening a serial device: Windows lets one handle at a time open a COM port and
/// refuses the next one with ERROR_ACCESS_DENIED, like TIOCEXCL on Linux.
Status open_status(const DWORD error) noexcept {
    return (error == ERROR_ACCESS_DENIED) ? Status::kResourceLimit : detail::status_from_win32(error);
}

/// The Status of a failed call on an open port; an unplugged device is kDisconnected.
Status last_error() noexcept {
    return detail::transfer_status(::GetLastError());
}

/// Converts @p path into the device path to open: a name such as "COM3" gets the prefix "\\.\", which
/// COM10 and above need; a path that starts with two separators already is a device path.
Status device_path(const char* const path, detail::WidePath& device) noexcept {
    const bool qualified{((path[0U] == '\\') || (path[0U] == '/')) && ((path[1U] == '\\') || (path[1U] == '/'))};
    return device.assign(path, qualified ? L"" : L"\\\\.\\");
}

/// Raw mode with the line settings of @p options: 8 data bits, no XON/XOFF, no replacement or
/// removal of bytes, DSR and DCD ignored, DTR raised and RTS raised (or driven by the flow control).
Status raw_settings(const HANDLE handle, const SerialOptions& options, DCB& settings) noexcept {
    settings = DCB{};
    settings.DCBlength = sizeof(DCB);
    // GetCommState() fails for anything that is not a serial device.
    Status status{(::GetCommState(handle, &settings) != FALSE) ? Status::kOk : Status::kInvalidArgument};
    if (is_ok(status)) {
        settings.BaudRate = static_cast<DWORD>(options.baud_rate);  // The enumerator value is the speed.
        settings.ByteSize = 8U;
        settings.fBinary = TRUE;
        settings.fOutxDsrFlow = FALSE;
        settings.fDtrControl = DTR_CONTROL_ENABLE;
        settings.fDsrSensitivity = FALSE;
        settings.fTXContinueOnXoff = TRUE;
        settings.fOutX = FALSE;
        settings.fInX = FALSE;
        settings.fErrorChar = FALSE;
        settings.fNull = FALSE;
        settings.fAbortOnError = FALSE;
        switch (options.parity) {
            case Parity::kNone:
                settings.fParity = FALSE;
                settings.Parity = NOPARITY;
                break;
            case Parity::kEven:
                settings.fParity = TRUE;
                settings.Parity = EVENPARITY;
                break;
            case Parity::kOdd:
                settings.fParity = TRUE;
                settings.Parity = ODDPARITY;
                break;
        }
        if (options.stop_bits == StopBits::kTwo) {
            settings.StopBits = TWOSTOPBITS;
        } else {
            settings.StopBits = ONESTOPBIT;
        }
        if (options.flow_control == FlowControl::kRtsCts) {
            settings.fOutxCtsFlow = TRUE;
            settings.fRtsControl = RTS_CONTROL_HANDSHAKE;
        } else {
            settings.fOutxCtsFlow = FALSE;
            settings.fRtsControl = RTS_CONTROL_ENABLE;
        }
    }
    return status;
}

/// Applies @p wanted with SetCommState() and reads the settings back, so that a setting the driver
/// adjusted instead of rejecting shows up.
Status apply(const HANDLE handle, const DCB& wanted) noexcept {
    DCB requested{wanted};
    Status status{Status::kOk};
    if (::SetCommState(handle, &requested) == FALSE) {
        // ERROR_INVALID_PARAMETER: the driver rejects a setting, e.g. the speed.
        const DWORD error{::GetLastError()};
        status = (error == ERROR_INVALID_PARAMETER) ? Status::kNotSupported : detail::transfer_status(error);
    }
    if (is_ok(status)) {
        DCB actual{};
        actual.DCBlength = sizeof(DCB);
        status = (::GetCommState(handle, &actual) != FALSE) ? Status::kOk : last_error();
        if (is_ok(status)) {
            const bool same{(actual.BaudRate == wanted.BaudRate) && (actual.ByteSize == wanted.ByteSize) &&
                            (actual.Parity == wanted.Parity) && (actual.StopBits == wanted.StopBits) &&
                            (actual.fOutxCtsFlow == wanted.fOutxCtsFlow) && (actual.fRtsControl == wanted.fRtsControl)};
            status = same ? Status::kOk : Status::kNotSupported;
        }
    }
    return status;
}

/// Makes a read complete as soon as at least one byte is there; without data the driver would wait
/// MAXDWORD - 1 ms, but SerialPort cancels the read at its deadline. Writes have no timeout of their
/// own either: write_all() cancels them at its deadline.
Status set_timeouts(const HANDLE handle) noexcept {
    COMMTIMEOUTS timeouts{};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = MAXDWORD;
    timeouts.ReadTotalTimeoutConstant = MAXDWORD - 1U;
    timeouts.WriteTotalTimeoutMultiplier = 0U;
    timeouts.WriteTotalTimeoutConstant = 0U;
    return (::SetCommTimeouts(handle, &timeouts) != FALSE) ? Status::kOk : last_error();
}

/// Discards the queued bytes that @p flags select (PURGE_RXCLEAR, PURGE_TXCLEAR).
Status purge(const HANDLE handle, const DWORD flags) noexcept {
    return (::PurgeComm(handle, flags) != FALSE) ? Status::kOk : last_error();
}

}  // namespace

SerialPort::~SerialPort() {
    static_cast<void>(close());
}

SerialPort::SerialPort(SerialPort&& other) noexcept : descriptor_{other.descriptor_}, event_{other.event_} {
    other.descriptor_ = kInvalidNativeHandle;
    other.event_ = nullptr;
}

SerialPort& SerialPort::operator=(SerialPort&& other) noexcept {
    if (this != &other) {
        static_cast<void>(close());
        descriptor_ = other.descriptor_;
        event_ = other.event_;
        other.descriptor_ = kInvalidNativeHandle;
        other.event_ = nullptr;
    }
    return *this;
}

Status SerialPort::open(const char* const path, const SerialOptions& options) noexcept {
    Status status{((path == nullptr) || is_open()) ? Status::kInvalidArgument : Status::kOk};
    detail::WidePath device{};
    if (is_ok(status)) {
        status = device_path(path, device);
    }
    HANDLE handle{INVALID_HANDLE_VALUE};
    if (is_ok(status)) {
        // Overlapped I/O lets reads and writes end at their deadline. No sharing: a serial port is
        // always exclusive on Windows.
        handle = ::CreateFileW(device.c_str(), GENERIC_READ | GENERIC_WRITE, 0U, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            status = open_status(::GetLastError());
        }
    }
    // Manual reset; ReadFile() and WriteFile() reset it when an operation starts.
    const HANDLE event{is_ok(status) ? ::CreateEventW(nullptr, TRUE, FALSE, nullptr) : nullptr};
    if (is_ok(status) && (event == nullptr)) {
        status = detail::last_win32_error();
    }
    DCB settings{};
    if (is_ok(status)) {
        status = raw_settings(handle, options, settings);
    }
    if (is_ok(status)) {
        status = apply(handle, settings);
    }
    if (is_ok(status)) {
        status = set_timeouts(handle);
    }
    if (is_ok(status)) {
        status = purge(handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
    }
    if (is_ok(status)) {
        descriptor_ = handle;
        event_ = event;
    } else {
        if (event != nullptr) {
            static_cast<void>(::CloseHandle(event));
        }
        if (handle != INVALID_HANDLE_VALUE) {
            static_cast<void>(::CloseHandle(handle));
        }
    }
    return status;
}

Status SerialPort::close() noexcept {
    Status status{Status::kOk};
    if (is_open()) {
        // The handle is released even when CloseHandle() reports an error; never retry.
        status = (::CloseHandle(descriptor_) != FALSE) ? Status::kOk : detail::last_win32_error();
        static_cast<void>(::CloseHandle(event_));
        descriptor_ = kInvalidNativeHandle;
        event_ = nullptr;
    }
    return status;
}

Status SerialPort::set_baud_rate(const BaudRate baud_rate) noexcept {
    // The pending output is sent at the old speed first, as with TCSADRAIN.
    Status status{drain()};
    DCB settings{};
    settings.DCBlength = sizeof(DCB);
    if (is_ok(status)) {
        status = (::GetCommState(descriptor_, &settings) != FALSE) ? Status::kOk : last_error();
    }
    if (is_ok(status)) {
        settings.BaudRate = static_cast<DWORD>(baud_rate);
        status = apply(descriptor_, settings);
    }
    return status;
}

Result<BaudRate> SerialPort::baud_rate() const noexcept {
    Result<BaudRate> result{Result<BaudRate>::failure(Status::kInvalidArgument)};
    if (is_open()) {
        DCB settings{};
        settings.DCBlength = sizeof(DCB);
        result = (::GetCommState(descriptor_, &settings) != FALSE) ? baud_rate_from_bits_per_second(settings.BaudRate)
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
        // Each round ends the call or is a stall: a read that the driver completed without data.
        for (std::uint32_t stall{0U}; stall < kMaxStalls; ++stall) {
            const Result<std::size_t> count{detail::read_overlapped(descriptor_, event_, buffer, deadline)};
            if ((!count.has_value()) || (count.value() > 0U)) {
                result = count;
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
        const Result<std::size_t> count{
            detail::write_overlapped(descriptor_, event_, data.subspan(written), deadline)};
        if (!count.has_value()) {
            status = count.status();  // kTimeout once the deadline passes without progress.
        } else if (count.value() > 0U) {
            written += count.value();
        } else {
            ++stalls;
            status = (stalls < kMaxStalls) ? Status::kOk : Status::kInterrupted;
        }
    }
    return status;
}

Status SerialPort::drain() noexcept {
    Status status{Status::kInvalidArgument};
    if (is_open()) {
        // For a serial port, FlushFileBuffers() waits until the driver has sent every written byte.
        status = (::FlushFileBuffers(descriptor_) != FALSE) ? Status::kOk : last_error();
    }
    return status;
}

Status SerialPort::discard_input() noexcept {
    return is_open() ? purge(descriptor_, PURGE_RXCLEAR) : Status::kInvalidArgument;
}

Status SerialPort::discard_output() noexcept {
    return is_open() ? purge(descriptor_, PURGE_TXCLEAR) : Status::kInvalidArgument;
}

}  // namespace common
