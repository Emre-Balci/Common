#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "common/containers/span.hpp"
#include "common/core/status.hpp"
#include "common/file/serial_port.hpp"
#include "common/time/clock.hpp"
#include "common/time/deadline.hpp"
#include "test_framework.hpp"

using namespace std::chrono_literals;

namespace {

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

common::Span<const std::uint8_t> bytes_of(const std::string& text) {
    return common::Span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

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

}  // namespace

TEST_CASE(serial_port_open_errors) {
    common::SerialPort port{};
    CHECK(!port.is_open());
    CHECK(port.descriptor() == -1);
    CHECK(port.open(nullptr, common::SerialOptions{}) == common::Status::kInvalidArgument);
    CHECK(port.open("/nonexistent/ttyUSB9", common::SerialOptions{}) == common::Status::kNotFound);
    CHECK(port.open("/dev/null", common::SerialOptions{}) == common::Status::kInvalidArgument);  // Not a terminal.
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

    PseudoTerminal terminal{};
    REQUIRE(terminal.valid());
    CHECK(common::is_ok(port.open(terminal.slave_path(), common::SerialOptions{})));
    CHECK(port.open(terminal.slave_path(), common::SerialOptions{}) == common::Status::kInvalidArgument);
}

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

    CHECK(common::baud_rate_from_bits_per_second(38'400U).value() == common::BaudRate::k38400);
    CHECK(common::baud_rate_from_bits_per_second(4'000'000U).value() == common::BaudRate::k4000000);
    CHECK(common::baud_rate_from_bits_per_second(12'345U).status() == common::Status::kNotSupported);
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
