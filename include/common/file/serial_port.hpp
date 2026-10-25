/// @file serial_port.hpp
/// @brief Serial port (UART, USB-serial adapter, pseudo-terminal) in raw mode, with timeouts.
///
/// A serial device sends bytes whenever it likes, and may stop answering or be unplugged at any
/// time. SerialPort therefore never waits without a deadline: every read and write takes a
/// Deadline, and a vanished device is reported as kDisconnected so the caller can reconnect.
///
/// Serial I/O is done by system calls that wait in the kernel, so nothing in this header is safe on
/// the real-time path. Run the port on its own non-real-time I/O thread, which exchanges commands
/// and decoded results with the real-time threads through SpscQueue, SeqLock or TripleBuffer.

#ifndef COMMON_FILE_SERIAL_PORT_HPP
#define COMMON_FILE_SERIAL_PORT_HPP

#include <cstddef>
#include <cstdint>

#include "common/containers/span.hpp"
#include "common/core/config.hpp"
#include "common/core/status.hpp"
#include "common/time/deadline.hpp"

namespace common {

/// Standard line speeds supported by the Linux termios interface. The enumerator value is the
/// speed in bit/s; on Windows it is passed to the driver as it is.
enum class BaudRate : std::uint32_t {
    k1200 = 1'200U,
    k2400 = 2'400U,
    k4800 = 4'800U,
    k9600 = 9'600U,
    k19200 = 19'200U,
    k38400 = 38'400U,
    k57600 = 57'600U,
    k115200 = 115'200U,
    k230400 = 230'400U,
    k460800 = 460'800U,
    k500000 = 500'000U,
    k576000 = 576'000U,
    k921600 = 921'600U,
    k1000000 = 1'000'000U,
    k1152000 = 1'152'000U,
    k1500000 = 1'500'000U,
    k2000000 = 2'000'000U,
    k2500000 = 2'500'000U,
    k3000000 = 3'000'000U,
    k3500000 = 3'500'000U,
    k4000000 = 4'000'000U,
};

/// @return the BaudRate for a speed in bit/s; kNotSupported if it is not a standard speed.
[[nodiscard]] Result<BaudRate> baud_rate_from_bits_per_second(std::uint32_t bits_per_second) noexcept;

enum class Parity : std::uint8_t {
    kNone,
    kEven,
    kOdd,
};

enum class StopBits : std::uint8_t {
    kOne,
    kTwo,
};

enum class FlowControl : std::uint8_t {
    kNone,
    kRtsCts,  ///< Hardware flow control on the RTS/CTS lines.
};

/// Line settings for SerialPort::open(). Characters always have 8 data bits.
struct SerialOptions final {
    BaudRate baud_rate{BaudRate::k38400};
    Parity parity{Parity::kNone};
    StopBits stop_bits{StopBits::kOne};
    FlowControl flow_control{FlowControl::kNone};
    /// Refuse further opens of the device while this handle is open (TIOCEXCL), so a second
    /// program cannot interleave its bytes with ours. Processes with CAP_SYS_ADMIN bypass it.
    /// Windows always opens a serial port for one handle only, whatever this says.
    bool exclusive{true};
};

/// Owns an open serial device in raw mode and closes it on destruction. Move-only.
///
/// Raw mode: bytes pass through unchanged in both directions (no echo, no line editing, no
/// newline translation, no special control characters), with 8 data bits and the modem control
/// lines ignored (CLOCAL), so the port works on a three-wire connection.
///
/// Timeouts: read() returns as soon as some bytes have arrived, and waits at most until its
/// Deadline for the first one. write_all() waits at most until its Deadline for space in the
/// kernel's output buffer. A Deadline::never() waits indefinitely; avoid it for devices that can
/// fall silent.
///
/// Error mapping: kNotFound (no such device), kPermissionDenied (e.g. not in the dialout group),
/// kResourceLimit (the port is in exclusive use by another handle, or out of descriptors),
/// kInvalidArgument (not a terminal device, invalid options, or an operation on a closed port),
/// kNotSupported (the device rejects a setting), kTimeout, kDisconnected (the device was unplugged
/// or hung up: close the port and open it again), kInterrupted, kSystemError.
///
/// On Windows, the port is a COM port ("COM3"), opened for overlapped I/O so that reads and writes
/// end at their deadline; the same raw settings apply through the DCB. DTR and RTS are raised, as
/// Linux does on open. Unlike Linux, bytes with a parity error are passed on rather than dropped.
/// Opening a port that another handle has open reports kResourceLimit.
///
/// Not thread-safe: one thread uses a port at a time.
class SerialPort final {
public:
    SerialPort() noexcept = default;

    /// Closes the device if one is open.
    ~SerialPort();

    SerialPort(SerialPort&& other) noexcept;
    SerialPort& operator=(SerialPort&& other) noexcept;
    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    /// Opens the terminal device @p path (e.g. "/dev/ttyUSB0"; on Windows a COM port name such as
    /// "COM3", or a device path such as "\\\\.\\COM12") and configures it in raw mode with
    /// @p options. Bytes received before the call are discarded.
    /// @return kInvalidArgument for nullptr, a port that is already open, or a path that is not a
    ///         terminal; kNotSupported if the device does not accept the settings; otherwise the
    ///         open(2) result. On error the port stays closed.
    Status open(const char* path, const SerialOptions& options) noexcept;

    /// Closes the device, first releasing exclusive access: the kernel keeps the exclusive flag
    /// as long as the device stays in use (e.g. the other side of a pseudo-terminal), so it would
    /// otherwise lock out the next open. Afterwards the port is closed even if an error is reported.
    /// @return kOk (also when nothing was open) or the close(2) error.
    Status close() noexcept;

    [[nodiscard]] bool is_open() const noexcept {
        return descriptor_ != kInvalidNativeHandle;
    }

    /// @return the descriptor, e.g. to wait for several ports with poll(2) (on Windows, the HANDLE,
    ///         opened with FILE_FLAG_OVERLAPPED); kInvalidNativeHandle (-1 on Linux) when closed.
    [[nodiscard]] NativeHandle descriptor() const noexcept {
        return descriptor_;
    }

    /// Changes the line speed after all pending output has been sent (TCSADRAIN; on Windows after
    /// drain()), e.g. after telling the device to switch. Waiting for the output is the kernel's and is not bounded by a
    /// deadline. @return kNotSupported if the device rejects the speed.
    Status set_baud_rate(BaudRate baud_rate) noexcept;

    /// @return the current line speed.
    [[nodiscard]] Result<BaudRate> baud_rate() const noexcept;

    /// Reads the bytes that have arrived, up to buffer.size(), waiting until @p deadline for at
    /// least one. Bytes that arrive after the first are not waited for.
    /// @return the number of bytes read (at least 1; 0 only for an empty buffer); kTimeout if
    ///         nothing arrived before the deadline (an expired deadline still takes what is
    ///         already there); kDisconnected if the device went away.
    [[nodiscard]] Result<std::size_t> read(Span<std::uint8_t> buffer, const Deadline& deadline) noexcept;

    /// Writes all of @p data into the kernel's output buffer, waiting until @p deadline when the
    /// buffer is full (e.g. while hardware flow control holds the line). Returning does not mean
    /// the bytes were sent on the line yet; see drain().
    /// @return kTimeout if not everything was written before the deadline (part of @p data may have
    ///         been written); kDisconnected if the device went away.
    Status write_all(Span<const std::uint8_t> data, const Deadline& deadline) noexcept;

    /// Waits until all written bytes have been sent on the line (tcdrain; FlushFileBuffers on
    /// Windows). The kernel's wait is not bounded by a deadline: with hardware flow control a
    /// stalled peer can hold it.
    Status drain() noexcept;

    /// Discards received bytes that have not been read yet, e.g. stale output of the device before
    /// a new command.
    Status discard_input() noexcept;

    /// Discards written bytes that have not been sent yet.
    Status discard_output() noexcept;

private:
    NativeHandle descriptor_{kInvalidNativeHandle};
#if defined(_WIN32)
    void* event_{nullptr};  ///< Manual-reset event for the overlapped reads and writes.
#else
    bool exclusive_{false};  ///< This handle set TIOCEXCL and clears it again on close().
#endif
};

}  // namespace common

#endif  // COMMON_FILE_SERIAL_PORT_HPP
