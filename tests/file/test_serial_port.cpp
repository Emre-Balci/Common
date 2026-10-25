#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <unistd.h>
#endif

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/containers/span.hpp"
#include "common/core/config.hpp"
#include "common/core/status.hpp"
#include "common/file/serial_port.hpp"
#include "common/time/clock.hpp"
#include "common/time/deadline.hpp"
#include "test_framework.hpp"
#include "test_platform.hpp"
#if defined(_WIN32)
#include "detail/overlapped_io.hpp"
#endif

using namespace std::chrono_literals;

namespace {

common::Span<const std::uint8_t> bytes_of(const std::string& text) {
    return common::Span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

#if !defined(_WIN32)
/// A pseudo-terminal pair: SerialPort opens the slave side, and the test plays the device on the
/// master side.
class PseudoTerminal final {
public:
    PseudoTerminal() {
        master_ = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
        if ((master_ >= 0) && (::grantpt(master_) == 0) && (::unlockpt(master_) == 0)) {
            std::array<char, 128U> name{};
            if (::ptsname_r(master_, name.data(), name.size()) == 0) {
                slave_path_ = name.data();
            }
        }
    }
    ~PseudoTerminal() {
        close_master();
    }
    PseudoTerminal(const PseudoTerminal&) = delete;
    PseudoTerminal& operator=(const PseudoTerminal&) = delete;
    PseudoTerminal(PseudoTerminal&&) = delete;
    PseudoTerminal& operator=(PseudoTerminal&&) = delete;

    [[nodiscard]] bool valid() const {
        return !slave_path_.empty();
    }
    [[nodiscard]] const char* slave_path() const {
        return slave_path_.c_str();
    }

    /// The device sends @p text.
    bool send(const std::string& text) const {
        return ::write(master_, text.data(), text.size()) == static_cast<ssize_t>(text.size());
    }

    /// Collects what the port sent, until @p count bytes arrived or 1 s passed.
    [[nodiscard]] std::string receive(const std::size_t count) const {
        std::string text{};
        const common::Deadline deadline{common::Deadline::after(1s)};
        while ((text.size() < count) && (!deadline.expired())) {
            pollfd entry{master_, POLLIN, 0};
            if (::poll(&entry, 1U, 10) > 0) {
                std::array<char, 256U> buffer{};
                const ssize_t got{::read(master_, buffer.data(), buffer.size())};
                if (got <= 0) {
                    break;
                }
                text.append(buffer.data(), static_cast<std::size_t>(got));
            }
        }
        return text;
    }

    /// The device goes away.
    void close_master() {
        if (master_ >= 0) {
            static_cast<void>(::close(master_));
            master_ = -1;
        }
    }

private:
    int master_{-1};
    std::string slave_path_;
};

/// Reads from @p port until @p count bytes arrived or the deadline passed.
std::string read_text(common::SerialPort& port, const std::size_t count, const common::Deadline& deadline) {
    std::string text{};
    std::array<std::uint8_t, 64U> buffer{};
    for (std::uint32_t round{0U}; (round < 1000U) && (text.size() < count); ++round) {
        const common::Result<std::size_t> got{port.read(buffer, deadline)};
        if (!got.has_value()) {
            break;
        }
        text.append(reinterpret_cast<const char*>(buffer.data()), got.value());
    }
    return text;
}

/// Waits up to 1 s until @p port has received data, without reading it.
bool wait_for_input(const common::SerialPort& port) {
    pollfd entry{port.descriptor(), POLLIN, 0};
    return ::poll(&entry, 1U, 1000) > 0;
}
#else
/// A connected pair of named-pipe handles, both opened for overlapped I/O. Windows has no
/// pseudo-terminal for COM ports, so the tests drive the overlapped reads and writes that SerialPort
/// makes through a pipe: one end stands for the port, and the test plays the device on the other.
class OverlappedPipe final {
public:
    static constexpr DWORD kBufferSize{4096U};

    OverlappedPipe() {
        static std::uint32_t next{0U};
        const std::wstring name{L"\\\\.\\pipe\\common_test_" + std::to_wstring(test::process_id()) + L"_" +
                                std::to_wstring(next)};
        ++next;
        device_ = ::CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                     PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1U,
                                     kBufferSize, kBufferSize, 0U, nullptr);
        port_ = ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0U, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
                              nullptr);
        port_event_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        device_event_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if ((device_ != INVALID_HANDLE_VALUE) && (port_ != INVALID_HANDLE_VALUE) && (device_event_ != nullptr)) {
            // The port end is connected already: ConnectNamedPipe() then reports ERROR_PIPE_CONNECTED.
            OVERLAPPED overlapped{};
            overlapped.hEvent = device_event_;
            connected_ = (::ConnectNamedPipe(device_, &overlapped) != FALSE) || (::GetLastError() == ERROR_PIPE_CONNECTED);
        }
    }
    ~OverlappedPipe() {
        close_device();
        for (const HANDLE handle : {port_, port_event_, device_event_}) {
            if ((handle != nullptr) && (handle != INVALID_HANDLE_VALUE)) {
                static_cast<void>(::CloseHandle(handle));
            }
        }
    }
    OverlappedPipe(const OverlappedPipe&) = delete;
    OverlappedPipe& operator=(const OverlappedPipe&) = delete;
    OverlappedPipe(OverlappedPipe&&) = delete;
    OverlappedPipe& operator=(OverlappedPipe&&) = delete;

    [[nodiscard]] bool valid() const {
        return connected_ && (port_event_ != nullptr);
    }

    /// One overlapped read on the port end, as SerialPort::read() makes it.
    [[nodiscard]] common::Result<std::size_t> read(const common::Span<std::uint8_t> buffer,
                                                   const common::Deadline& deadline) const {
        return common::detail::read_overlapped(port_, port_event_, buffer, deadline);
    }

    /// One overlapped write on the port end, as SerialPort::write_all() makes it.
    [[nodiscard]] common::Result<std::size_t> write(const common::Span<const std::uint8_t> data,
                                                    const common::Deadline& deadline) const {
        return common::detail::write_overlapped(port_, port_event_, data, deadline);
    }

    /// The device sends @p text.
    bool send(const std::string& text) const {
        const common::Result<std::size_t> sent{
            common::detail::write_overlapped(device_, device_event_, bytes_of(text), common::Deadline::after(1s))};
        return sent.value_or(0U) == text.size();
    }

    /// Collects what the port sent, until @p count bytes arrived or 1 s passed.
    [[nodiscard]] std::string receive(const std::size_t count) const {
        std::string text{};
        std::array<std::uint8_t, 256U> buffer{};
        const common::Deadline deadline{common::Deadline::after(1s)};
        for (std::uint32_t round{0U}; (round < 100U) && (text.size() < count); ++round) {
            const common::Result<std::size_t> got{
                common::detail::read_overlapped(device_, device_event_, buffer, deadline)};
            if (!got.has_value()) {
                break;
            }
            text.append(reinterpret_cast<const char*>(buffer.data()), got.value());
        }
        return text;
    }

    /// The device goes away.
    void close_device() {
        if (device_ != INVALID_HANDLE_VALUE) {
            static_cast<void>(::CloseHandle(device_));
            device_ = INVALID_HANDLE_VALUE;
        }
    }

private:
    HANDLE device_{INVALID_HANDLE_VALUE};
    HANDLE port_{INVALID_HANDLE_VALUE};
    HANDLE port_event_{nullptr};
    HANDLE device_event_{nullptr};
    bool connected_{false};
};
#endif

}  // namespace

TEST_CASE(serial_port_open_errors) {
    common::SerialPort port{};
    CHECK(!port.is_open());
    CHECK(port.descriptor() == common::kInvalidNativeHandle);
    CHECK(port.open(nullptr, common::SerialOptions{}) == common::Status::kInvalidArgument);
#if defined(_WIN32)
    CHECK(port.open("COM255", common::SerialOptions{}) == common::Status::kNotFound);
    CHECK(port.open("NUL", common::SerialOptions{}) == common::Status::kInvalidArgument);  // Not a serial port.
    CHECK(port.open("\\\\.\\NUL", common::SerialOptions{}) == common::Status::kInvalidArgument);
#else
    CHECK(port.open("/nonexistent/ttyUSB9", common::SerialOptions{}) == common::Status::kNotFound);
    CHECK(port.open("/dev/null", common::SerialOptions{}) == common::Status::kInvalidArgument);  // Not a terminal.
#endif
    CHECK(!port.is_open());

    // Operations on a closed port.
    std::array<std::uint8_t, 4U> buffer{};
    CHECK(port.read(buffer, common::Deadline{}).status() == common::Status::kInvalidArgument);
    CHECK(port.write_all(buffer, common::Deadline{}) == common::Status::kInvalidArgument);
    CHECK(port.set_baud_rate(common::BaudRate::k9600) == common::Status::kInvalidArgument);
    CHECK(port.baud_rate().status() == common::Status::kInvalidArgument);
    CHECK(port.drain() == common::Status::kInvalidArgument);
    CHECK(port.discard_input() == common::Status::kInvalidArgument);
    CHECK(port.discard_output() == common::Status::kInvalidArgument);
    CHECK(common::is_ok(port.close()));

#if !defined(_WIN32)
    PseudoTerminal terminal{};
    REQUIRE(terminal.valid());
    CHECK(common::is_ok(port.open(terminal.slave_path(), common::SerialOptions{})));
    CHECK(port.open(terminal.slave_path(), common::SerialOptions{}) == common::Status::kInvalidArgument);
#endif
}

TEST_CASE(serial_port_baud_rates_from_bits_per_second) {
    CHECK(common::baud_rate_from_bits_per_second(38'400U).value() == common::BaudRate::k38400);
    CHECK(common::baud_rate_from_bits_per_second(4'000'000U).value() == common::BaudRate::k4000000);
    CHECK(common::baud_rate_from_bits_per_second(12'345U).status() == common::Status::kNotSupported);
}

// Opt-in: COMMON_TEST_SERIAL_PORT names a real serial port (e.g. "COM4" or "/dev/ttyUSB0"). The test
// opens it, changes and reads back the speed and waits for input, but sends nothing.
TEST_CASE(serial_port_device_from_environment) {
    const std::string name{test::environment("COMMON_TEST_SERIAL_PORT")};
    if (name.empty()) {
        return;
    }
    common::SerialPort port{};
    REQUIRE(common::is_ok(port.open(name.c_str(), common::SerialOptions{})));
    CHECK(port.baud_rate().value_or(common::BaudRate::k1200) == common::BaudRate::k38400);
    CHECK(common::is_ok(port.set_baud_rate(common::BaudRate::k115200)));
    CHECK(port.baud_rate().value_or(common::BaudRate::k1200) == common::BaudRate::k115200);
    common::SerialPort second{};
    CHECK(second.open(name.c_str(), common::SerialOptions{}) == common::Status::kResourceLimit);  // In use.

    std::array<std::uint8_t, 64U> buffer{};
    const common::TimePoint start{common::MonotonicClock::now()};
    const common::Result<std::size_t> got{port.read(buffer, common::Deadline::after(50ms))};
    CHECK(got.has_value() || (got.status() == common::Status::kTimeout));
    if (!got.has_value()) {
        CHECK((common::MonotonicClock::now() - start) >= 50ms);
    }
    CHECK(common::is_ok(port.discard_input()));
    CHECK(common::is_ok(port.discard_output()));
    CHECK(common::is_ok(port.drain()));
    CHECK(common::is_ok(port.close()));
}

#if !defined(_WIN32)

TEST_CASE(serial_port_raw_round_trip) {
    PseudoTerminal terminal{};
    REQUIRE(terminal.valid());
    common::SerialPort port{};
    REQUIRE(common::is_ok(port.open(terminal.slave_path(), common::SerialOptions{})));

    // Raw mode: "\r" is neither translated to "\n" nor expanded to "\r\n", and no echo comes back.
    const std::string command{"AT Z\r"};
    CHECK(common::is_ok(port.write_all(bytes_of(command), common::Deadline::after(1s))));
    CHECK(common::is_ok(port.drain()));
    CHECK(terminal.receive(command.size()) == command);

    const std::string response{"41 0C 1A F8\r\r>"};
    CHECK(terminal.send(response));
    CHECK(read_text(port, response.size(), common::Deadline::after(1s)) == response);

    // Every byte value passes through unchanged, including control characters (^C, ^S, ^Q, DEL).
    std::string all_bytes{};
    for (std::uint32_t value{0U}; value < 256U; ++value) {
        all_bytes.push_back(static_cast<char>(value));
    }
    CHECK(terminal.send(all_bytes));
    CHECK(read_text(port, all_bytes.size(), common::Deadline::after(1s)) == all_bytes);
    CHECK(common::is_ok(port.write_all(bytes_of(all_bytes), common::Deadline::after(1s))));
    CHECK(terminal.receive(all_bytes.size()) == all_bytes);

    // An empty buffer reads nothing, and empty data writes nothing.
    CHECK(port.read(common::Span<std::uint8_t>{}, common::Deadline{}).value() == 0U);
    CHECK(common::is_ok(port.write_all(common::Span<const std::uint8_t>{}, common::Deadline{})));
}

TEST_CASE(serial_port_read_timeout) {
    PseudoTerminal terminal{};
    REQUIRE(terminal.valid());
    common::SerialPort port{};
    REQUIRE(common::is_ok(port.open(terminal.slave_path(), common::SerialOptions{})));

    std::array<std::uint8_t, 16U> buffer{};
    const common::TimePoint start{common::MonotonicClock::now()};
    const common::Result<std::size_t> silent{port.read(buffer, common::Deadline::after(30ms))};
    const common::Duration waited{common::MonotonicClock::now() - start};
    CHECK(silent.status() == common::Status::kTimeout);
    CHECK(waited >= 30ms);
    CHECK(waited < 1s);

    // An expired deadline still takes the bytes that have arrived.
    CHECK(terminal.send("OK"));
    REQUIRE(wait_for_input(port));
    const common::Result<std::size_t> ready{port.read(buffer, common::Deadline{})};
    CHECK(ready.has_value());
    CHECK(ready.value_or(0U) >= 1U);

    // Bytes received before discard_input() are dropped.
    CHECK(read_text(port, 1U, common::Deadline::after(100ms)).size() <= 1U);  // Drain any rest of "OK".
    CHECK(terminal.send("stale"));
    REQUIRE(wait_for_input(port));
    CHECK(common::is_ok(port.discard_input()));
    CHECK(port.read(buffer, common::Deadline::after(20ms)).status() == common::Status::kTimeout);
    CHECK(common::is_ok(port.discard_output()));
}

TEST_CASE(serial_port_write_timeout) {
    PseudoTerminal terminal{};
    REQUIRE(terminal.valid());
    common::SerialPort port{};
    REQUIRE(common::is_ok(port.open(terminal.slave_path(), common::SerialOptions{})));

    // The device never reads, so the kernel buffers fill up and the write runs into its deadline.
    const std::vector<std::uint8_t> data(4U * 1024U * 1024U, std::uint8_t{0x55U});
    const common::TimePoint start{common::MonotonicClock::now()};
    CHECK(port.write_all(common::Span<const std::uint8_t>{data.data(), data.size()},
                         common::Deadline::after(50ms)) == common::Status::kTimeout);
    CHECK((common::MonotonicClock::now() - start) >= 50ms);
}

TEST_CASE(serial_port_settings) {
    PseudoTerminal terminal{};
    REQUIRE(terminal.valid());
    common::SerialPort port{};
    common::SerialOptions options{};
    options.baud_rate = common::BaudRate::k115200;
    options.parity = common::Parity::kEven;
    options.stop_bits = common::StopBits::kTwo;
    options.flow_control = common::FlowControl::kRtsCts;
    // A pseudo-terminal accepts tcsetattr() but drops parity; the read-back check catches that.
    CHECK(port.open(terminal.slave_path(), options) == common::Status::kNotSupported);
    CHECK(!port.is_open());

    options.parity = common::Parity::kNone;
    REQUIRE(common::is_ok(port.open(terminal.slave_path(), options)));
    CHECK(port.baud_rate().value() == common::BaudRate::k115200);

    CHECK(common::is_ok(port.set_baud_rate(common::BaudRate::k500000)));
    CHECK(port.baud_rate().value() == common::BaudRate::k500000);
    CHECK(common::is_ok(port.set_baud_rate(common::BaudRate::k9600)));
    CHECK(port.baud_rate().value() == common::BaudRate::k9600);
}

TEST_CASE(serial_port_exclusive_and_move) {
    PseudoTerminal terminal{};
    REQUIRE(terminal.valid());
    common::SerialPort first{};
    REQUIRE(common::is_ok(first.open(terminal.slave_path(), common::SerialOptions{})));

    // TIOCEXCL keeps a second handle out, except for processes with CAP_SYS_ADMIN.
    common::SerialPort second{};
    if (::geteuid() != 0U) {
        CHECK(second.open(terminal.slave_path(), common::SerialOptions{}) == common::Status::kResourceLimit);
        CHECK(!second.is_open());
    }

    common::SerialPort moved{std::move(first)};
    CHECK(!first.is_open());  // NOLINT(bugprone-use-after-move,hicpp-invalid-access-moved): moved-from state.
    CHECK(moved.is_open());
    common::SerialPort assigned{};
    assigned = std::move(moved);
    CHECK(assigned.is_open());
    CHECK(common::is_ok(assigned.close()));
    CHECK(!assigned.is_open());

    // Closed: the device can be opened again, also without exclusive access.
    common::SerialOptions shared{};
    shared.exclusive = false;
    CHECK(common::is_ok(second.open(terminal.slave_path(), shared)));
    common::SerialPort third{};
    CHECK(common::is_ok(third.open(terminal.slave_path(), shared)));
}

TEST_CASE(serial_port_reports_disconnect) {
    PseudoTerminal terminal{};
    REQUIRE(terminal.valid());
    common::SerialPort port{};
    REQUIRE(common::is_ok(port.open(terminal.slave_path(), common::SerialOptions{})));

    terminal.close_master();  // Like unplugging a USB adapter.
    std::array<std::uint8_t, 16U> buffer{};
    CHECK(port.read(buffer, common::Deadline::after(1s)).status() == common::Status::kDisconnected);
    CHECK(port.write_all(buffer, common::Deadline::after(1s)) == common::Status::kDisconnected);
    CHECK(common::is_ok(port.close()));
}

#else

TEST_CASE(serial_port_overlapped_read) {
    OverlappedPipe pipe{};
    REQUIRE(pipe.valid());
    std::array<std::uint8_t, 64U> buffer{};

    // Bytes that are there already come back at once.
    REQUIRE(pipe.send("ATZ\r"));
    CHECK(pipe.read(buffer, common::Deadline::after(1s)).value_or(0U) == 4U);
    CHECK(std::memcmp(buffer.data(), "ATZ\r", 4U) == 0);

    // Nothing arrives: the read is cancelled at its deadline, not before.
    const common::TimePoint start{common::MonotonicClock::now()};
    CHECK(pipe.read(buffer, common::Deadline::after(30ms)).status() == common::Status::kTimeout);
    CHECK((common::MonotonicClock::now() - start) >= 30ms);

    // An expired deadline still takes what is already there.
    CHECK(pipe.read(buffer, common::Deadline{}).status() == common::Status::kTimeout);
    REQUIRE(pipe.send("OK"));
    CHECK(pipe.read(buffer, common::Deadline{}).value_or(0U) == 2U);

    // A read that waits ends as soon as data arrives.
    std::thread device{[&pipe]() {
        std::this_thread::sleep_for(20ms);
        static_cast<void>(pipe.send(">"));
    }};
    const common::TimePoint waiting{common::MonotonicClock::now()};
    CHECK(pipe.read(buffer, common::Deadline::after(5s)).value_or(0U) == 1U);
    CHECK((common::MonotonicClock::now() - waiting) < 4s);
    device.join();
}

TEST_CASE(serial_port_overlapped_write) {
    OverlappedPipe pipe{};
    REQUIRE(pipe.valid());
    CHECK(pipe.write(bytes_of("AT\r"), common::Deadline::after(1s)).value_or(0U) == 3U);
    CHECK(pipe.receive(3U) == "AT\r");

    // Nobody reads: the write waits for room, and is cancelled at its deadline with part of the data
    // written, or none.
    const std::string block(1024U * 1024U, 'x');
    const common::TimePoint start{common::MonotonicClock::now()};
    const common::Result<std::size_t> sent{pipe.write(bytes_of(block), common::Deadline::after(50ms))};
    CHECK((sent.status() == common::Status::kTimeout) || (sent.value_or(block.size()) < block.size()));
    CHECK((common::MonotonicClock::now() - start) >= 50ms);
}

TEST_CASE(serial_port_overlapped_disconnect) {
    std::array<std::uint8_t, 8U> buffer{};
    {
        OverlappedPipe pipe{};
        REQUIRE(pipe.valid());
        pipe.close_device();
        CHECK(pipe.read(buffer, common::Deadline::after(1s)).status() == common::Status::kDisconnected);
        CHECK(pipe.write(bytes_of("x"), common::Deadline::after(1s)).status() == common::Status::kDisconnected);
    }
    // The device goes away while a read waits.
    OverlappedPipe pipe{};
    REQUIRE(pipe.valid());
    std::thread unplug{[&pipe]() {
        std::this_thread::sleep_for(20ms);
        pipe.close_device();
    }};
    CHECK(pipe.read(buffer, common::Deadline::after(5s)).status() == common::Status::kDisconnected);
    unplug.join();
}

#endif
