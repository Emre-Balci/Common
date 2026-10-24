#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include "common/containers/span.hpp"
#include "common/diagnostics/logger.hpp"
#include "common/time/clock.hpp"
#include "test_framework.hpp"

namespace {

// Rejected argument types (deleted constructors).
static_assert(!std::is_constructible<common::LogArgument, long double>::value, "long double is rejected");
static_assert(!std::is_constructible<common::LogArgument, wchar_t>::value, "wchar_t is rejected");
static_assert(!std::is_constructible<common::LogArgument, char32_t>::value, "char32_t is rejected");
static_assert(!std::is_constructible<common::LogArgument, const int*>::value, "pointers are rejected");
static_assert(std::is_trivially_copyable<common::LogRecord>::value, "records are copied bytewise");

constexpr common::TimePoint kTime{common::Duration{12'000'345'120}};

/// A record with @p format and up to six arguments.
template <typename... Args>
common::LogRecord make_record(const common::LogLevel level, const char* const format, const Args&... args) {
    common::LogRecord record{};
    record.time = kTime;
    record.format = format;
    record.level = level;
    record.argument_count = static_cast<std::uint8_t>(sizeof...(Args));
    record.arguments = {{common::LogArgument{args}...}};
    return record;
}

/// The whole line, '\n' included.
std::string format_line(const common::LogRecord& record) {
    std::array<char, 512U> buffer{};
    const std::size_t length{common::format_log_record(record, common::Span<char>{buffer})};
    return std::string{buffer.data(), length};
}

/// Just the message: the line without the time, the level and the '\n'.
template <typename... Args>
std::string message(const char* const format, const Args&... args) {
    const std::string line{format_line(make_record(common::LogLevel::kInfo, format, args...))};
    const std::string prefix{"12.000345120 INFO "};
    return ((line.size() > prefix.size()) && (line.compare(0U, prefix.size(), prefix) == 0))
               ? line.substr(prefix.size(), line.size() - prefix.size() - 1U)
               : std::string{"<bad line: "} + line + ">";
}

/// What printf("%.6f") prints, without trailing zeros and a trailing point.
std::string printf_fixed(const double value) {
    std::array<char, 64U> buffer{};
    static_cast<void>(std::snprintf(buffer.data(), buffer.size(), "%.6f", value));
    std::string text{buffer.data()};
    if (text.find('.') != std::string::npos) {
        while (text.back() == '0') {
            text.pop_back();
        }
        if (text.back() == '.') {
            text.pop_back();
        }
    }
    return text;
}

/// Deterministic pseudo-random numbers (splitmix64).
class Random final {
public:
    std::uint64_t next() {
        state_ += 0x9E3779B97F4A7C15U;
        std::uint64_t z{state_};
        z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9U;
        z = (z ^ (z >> 27U)) * 0x94D049BB133111EBU;
        return z ^ (z >> 31U);
    }

private:
    std::uint64_t state_{42U};
};

/// Collects the lines a Logger drains.
void collect(const char* const text, const std::size_t length, void* const context) noexcept {
    static_cast<std::vector<std::string>*>(context)->emplace_back(text, length);
}

enum class Side : char { kBuy = 'B', kSell = 'S' };
enum class Phase : std::uint16_t { kIdle = 3U };

}  // namespace

TEST_CASE(logger_line_layout) {
    CHECK(format_line(make_record(common::LogLevel::kWarning, "cycle {} overran by {} ns", 7, 1250)) ==
          "12.000345120 WARNING cycle 7 overran by 1250 ns\n");
    CHECK(format_line(make_record(common::LogLevel::kDebug, "x")) == "12.000345120 DEBUG x\n");
    CHECK(format_line(make_record(common::LogLevel::kError, "")) == "12.000345120 ERROR \n");
    CHECK(format_line(make_record(static_cast<common::LogLevel>(9U), "x")) == "12.000345120 UNKNOWN x\n");

    common::LogRecord record{make_record(common::LogLevel::kInfo, "x")};
    record.time = common::TimePoint{};
    CHECK(format_line(record) == "0.000000000 INFO x\n");
    record.time = common::TimePoint{common::Duration{-1'500'000'000}};
    CHECK(format_line(record) == "-1.500000000 INFO x\n");
    record.time = common::TimePoint{common::Duration{std::numeric_limits<std::int64_t>::min()}};
    CHECK(format_line(record) == "-9223372036.854775808 INFO x\n");
    record.format = nullptr;  // An empty message.
    record.time = kTime;
    CHECK(format_line(record) == "12.000345120 INFO \n");
}

TEST_CASE(logger_placeholders_and_braces) {
    CHECK(message("a {{ b }} c") == "a { b } c");
    CHECK(message("{{}}") == "{}");
    CHECK(message("{} {} {}", 1) == "1 {} {}");  // No argument left.
    CHECK(message("{}", 1, 2) == "1");          // Surplus arguments are ignored.
    CHECK(message("{x} }{") == "{x} }{");       // Other braces print as they are.
    CHECK(message("{{}", 1) == "{}");
    CHECK(message("{{{}}}", 5) == "{5}");
    CHECK(message("end {") == "end {");
    CHECK(message("{}{}{}{}{}{}", 1, 2, 3, 4, 5, 6) == "123456");

    // A default-constructed argument prints as "{}".
    common::LogRecord record{make_record(common::LogLevel::kInfo, "[{}]")};
    record.argument_count = 1U;
    CHECK(format_line(record) == "12.000345120 INFO [{}]\n");
    // A corrupt argument count is clamped to the arguments the record has.
    record = make_record(common::LogLevel::kInfo, "{}{}{}{}{}{}{}", 1, 2, 3, 4, 5, 6);
    record.argument_count = 200U;
    CHECK(format_line(record) == "12.000345120 INFO 123456{}\n");

    // At most 4096 characters of the format are read.
    const std::string long_format(5000U, 'a');
    std::array<char, 8192U> buffer{};
    record = make_record(common::LogLevel::kInfo, long_format.c_str());
    const std::size_t length{common::format_log_record(record, common::Span<char>{buffer})};
    CHECK(length == (std::strlen("12.000345120 INFO ") + 4096U + 1U));
}

TEST_CASE(logger_integers_and_other_values) {
    CHECK(message("{} {} {}", 0, -1, 42U) == "0 -1 42");
    CHECK(message("{}", std::numeric_limits<std::int64_t>::min()) == "-9223372036854775808");
    CHECK(message("{}", std::numeric_limits<std::int64_t>::max()) == "9223372036854775807");
    CHECK(message("{}", std::numeric_limits<std::uint64_t>::max()) == "18446744073709551615");
    CHECK(message("{} {}", std::int8_t{-5}, std::uint8_t{200U}) == "-5 200");  // Numbers, not characters.
    CHECK(message("{} {}", true, false) == "true false");
    CHECK(message("{}{}", 'o', 'k') == "ok");
    CHECK(message("{} {}", Side::kSell, Phase::kIdle) == "S 3");  // char underlying type: a character.
    CHECK(message("{} {}", 0.5F, -2.25) == "0.5 -2.25");
}

TEST_CASE(logger_text_arguments) {
    CHECK(message("{}", "hello") == "hello");
    const char* const null_text{nullptr};
    CHECK(message("{}", null_text) == "(null)");
    std::array<char, 4U> buffer{{'a', 'b', 'c', 'd'}};
    CHECK(message("{}", std::string_view{buffer.data(), 3U}) == "abc");  // Need not be NUL-terminated.
    CHECK(message("<{}>", "") == "<>");

    // Text is copied: changing the source after the record was made does not change the record.
    char mutable_text[] = "before";  // NOLINT(cppcoreguidelines-avoid-c-arrays,hicpp-avoid-c-arrays,modernize-avoid-c-arrays)
    const common::LogRecord record{make_record(common::LogLevel::kInfo, "{}", static_cast<const char*>(mutable_text))};
    mutable_text[0] = 'X';
    CHECK(format_line(record) == "12.000345120 INFO before\n");

    // At most 23 characters: longer text keeps 20 and "...".
    CHECK(message("{}", "12345678901234567890123") == "12345678901234567890123");
    CHECK(message("{}", "123456789012345678901234") == "12345678901234567890...");
    CHECK(message("{}", std::string_view{"a very long text that does not fit"}) == "a very long text tha...");

    // Control characters in arguments print as '?'; other bytes pass through.
    CHECK(message("{}", "a\nb\tc\x7f" "d") == "a?b?c?d");
    CHECK(message("{}{}", '\r', '\0') == "??");
    CHECK(message("{}", "caf\xC3\xA9") == "caf\xC3\xA9");
    // The format itself is printed unchanged.
    CHECK(message("tab\there") == "tab\there");
}

TEST_CASE(logger_doubles_match_printf) {
    const std::array<double, 22U> samples{{0.0,
                                           0.1,
                                           2.0,
                                           -2.0,
                                           0.0000004,
                                           0.0000005,
                                           0.0000015,
                                           0.0078125,  // An exact tie at the sixth decimal: to even.
                                           0.0234375,
                                           1.0000005,
                                           0.5000005,
                                           999999.9999995,
                                           999999999999999.9,
                                           123456.789,
                                           -123456.789,
                                           1e-300,
                                           5e-324,
                                           3.141592653589793,
                                           0.9999995,
                                           0.99999949999999994,
                                           1e14,
                                           4503599627370495.5 / 8.0}};
    for (const double value : samples) {
        CHECK(message("{}", value) == printf_fixed(value));
    }
    CHECK(message("{}", 0.1) == "0.1");
    CHECK(message("{}", 2.0) == "2");
    CHECK(message("{}", 0.0000004) == "0");
    CHECK(message("{}", 0.0078125) == "0.007812");
    CHECK(message("{}", -0.0) == "-0");
    CHECK(message("{}", -0.0000001) == "-0");  // Like printf, the sign stays when the value rounds to 0.

    // Random bit patterns over the whole fixed range, and decimal values near rounding boundaries.
    Random random{};
    std::uint32_t compared{0U};
    std::uint32_t mismatches{0U};
    for (std::uint32_t i{0U}; i < 50'000U; ++i) {
        const std::uint64_t bits{random.next()};
        double value{0.0};
        static_cast<void>(std::memcpy(&value, &bits, sizeof(value)));
        if (std::isfinite(value) && (std::fabs(value) < 1e15)) {
            ++compared;
            mismatches += (message("{}", value) == printf_fixed(value)) ? 0U : 1U;
        }
        // k / 10^7 lies on or next to a rounding boundary of the sixth decimal.
        const double near_tie{static_cast<double>(random.next() % 100'000'000'000U) / 1e7};
        mismatches += (message("{}", near_tie) == printf_fixed(near_tie)) ? 0U : 1U;
        // k / 2^7 includes exact ties.
        const double exact_tie{static_cast<double>(random.next() % 100'000'000U) / 128.0};
        mismatches += (message("{}", -exact_tie) == printf_fixed(-exact_tie)) ? 0U : 1U;
    }
    CHECK(compared > 1000U);
    CHECK(mismatches == 0U);
}

TEST_CASE(logger_doubles_scientific_and_special) {
    CHECK(message("{}", 1e15) == "1e+15");
    CHECK(message("{}", 1e20) == "1e+20");
    CHECK(message("{}", -1e20) == "-1e+20");
    CHECK(message("{}", 1.5e300) == "1.5e+300");
    CHECK(message("{}", 1e23) == "1e+23");
    CHECK(message("{}", 123456789012345678.0) == "1.234568e+17");
    CHECK(message("{}", 9999999.5e9) == "1e+16");  // 9.9999995 rounds up into the next power of ten.
    CHECK(message("{}", DBL_MAX) == "1.797693e+308");
    CHECK(message("{}", -DBL_MAX) == "-1.797693e+308");
    CHECK(message("{}", 2.5e15) == "2.5e+15");

    CHECK(message("{}", std::numeric_limits<double>::quiet_NaN()) == "nan");
    CHECK(message("{}", -std::numeric_limits<double>::quiet_NaN()) == "nan");
    CHECK(message("{}", std::numeric_limits<double>::infinity()) == "inf");
    CHECK(message("{}", -std::numeric_limits<double>::infinity()) == "-inf");
    CHECK(message("{}", std::numeric_limits<float>::infinity()) == "inf");
}

TEST_CASE(logger_truncates_long_lines) {
    const common::LogRecord record{make_record(common::LogLevel::kInfo, "{} and more text", 12345)};
    std::array<char, 16U> buffer{};
    buffer.fill('#');
    CHECK(common::format_log_record(record, common::Span<char>{buffer}) == 16U);
    CHECK(std::string(buffer.data(), buffer.size()) == "12.000345120 IN\n");

    CHECK(common::format_log_record(record, common::Span<char>{buffer}.first(1U)) == 1U);
    CHECK(buffer[0U] == '\n');
    CHECK(common::format_log_record(record, common::Span<char>{}) == 0U);
}

TEST_CASE(logger_queue_drain_and_levels) {
    common::Logger<8U> logger{};
    CHECK(common::Logger<8U>::capacity() == 8U);
    CHECK(logger.min_level() == common::LogLevel::kDebug);
    logger.log_at(kTime, common::LogLevel::kInfo, "speed {} rpm", 1200U);
    logger.debug("debug {}", 1);
    logger.info("info");
    logger.warning("warning {} {}", "a", 2.5);
    logger.error("error {}", -3);
    CHECK(logger.size_approx() == 5U);

    std::vector<std::string> lines{};
    CHECK(logger.drain(&collect, &lines, 2U) == 2U);
    REQUIRE(lines.size() == 2U);
    CHECK(lines[0U] == "12.000345120 INFO speed 1200 rpm\n");
    CHECK(lines[1U].find(" DEBUG debug 1\n") != std::string::npos);
    CHECK(logger.drain(&collect, &lines, 100U) == 3U);
    REQUIRE(lines.size() == 5U);
    CHECK(lines[2U].find(" INFO info\n") != std::string::npos);
    CHECK(lines[3U].find(" WARNING warning a 2.5\n") != std::string::npos);
    CHECK(lines[4U].find(" ERROR error -3\n") != std::string::npos);
    CHECK(logger.drain(&collect, &lines, 100U) == 0U);

    // Records below the minimum level are skipped and not counted as dropped.
    logger.set_min_level(common::LogLevel::kWarning);
    CHECK(!logger.enabled(common::LogLevel::kInfo));
    CHECK(logger.enabled(common::LogLevel::kError));
    logger.info("skipped");
    logger.debug("skipped");
    logger.error("kept");
    CHECK(logger.size_approx() == 1U);
    CHECK(logger.dropped() == 0U);

    common::Logger<8U> quiet{common::LogLevel::kError};
    quiet.warning("skipped");
    CHECK(quiet.size_approx() == 0U);
}

TEST_CASE(logger_counts_and_reports_drops) {
    common::Logger<8U> logger{};
    for (std::uint32_t i{0U}; i < 11U; ++i) {
        logger.log_at(kTime, common::LogLevel::kInfo, "record {}", i);
    }
    CHECK(logger.dropped() == 3U);  // Records 8, 9 and 10 did not fit.

    // The notice comes first, even when no record may be drained.
    std::vector<std::string> lines{};
    CHECK(logger.drain(&collect, &lines, 0U) == 0U);
    REQUIRE(lines.size() == 1U);
    CHECK(lines[0U].find(" WARNING dropped 3 log record(s): queue full\n") != std::string::npos);

    // Reported once; the queued records follow in order.
    CHECK(logger.drain(&collect, &lines, 100U) == 8U);
    REQUIRE(lines.size() == 9U);
    CHECK(lines[1U] == "12.000345120 INFO record 0\n");
    CHECK(lines[8U] == "12.000345120 INFO record 7\n");

    // New drops are reported as the difference since the last report.
    for (std::uint32_t i{0U}; i < 10U; ++i) {
        logger.log_at(kTime, common::LogLevel::kInfo, "again {}", i);
    }
    CHECK(logger.dropped() == 5U);
    lines.clear();
    CHECK(logger.drain(&collect, &lines, 100U) == 8U);
    REQUIRE(!lines.empty());
    CHECK(lines[0U].find("dropped 2 log record(s)") != std::string::npos);
}

TEST_CASE(logger_write_to_file_descriptor) {
    std::array<int, 2U> pipe_ends{{-1, -1}};
    REQUIRE(::pipe(pipe_ends.data()) == 0);
    int write_end{pipe_ends[1U]};
    const std::string_view line{"12.000000000 INFO hello\n"};
    common::write_log_line_to_fd(line.data(), line.size(), &write_end);
    common::write_log_line_to_fd(nullptr, 0U, &write_end);  // Nothing to write.

    common::Logger<4U> logger{};
    logger.log_at(kTime, common::LogLevel::kError, "code {}", 7);
    CHECK(logger.drain(&common::write_log_line_to_fd, &write_end, 4U) == 1U);
    static_cast<void>(::close(write_end));

    std::array<char, 256U> buffer{};
    std::string received{};
    for (std::uint32_t round{0U}; round < 100U; ++round) {
        const ssize_t count{::read(pipe_ends[0U], buffer.data(), buffer.size())};
        if (count <= 0) {
            break;
        }
        received.append(buffer.data(), static_cast<std::size_t>(count));
    }
    static_cast<void>(::close(pipe_ends[0U]));
    CHECK(received == "12.000000000 INFO hello\n12.000345120 ERROR code 7\n");

    // A failing write drops the line instead of reporting or blocking.
    int closed_descriptor{write_end};
    common::write_log_line_to_fd(line.data(), line.size(), &closed_descriptor);
}

TEST_CASE(logger_threaded_producer_and_consumer) {
    static common::Logger<64U> logger{};
    constexpr std::uint32_t kRecords{20'000U};
    std::atomic<bool> done{false};

    std::thread producer{[&done]() {
        for (std::uint32_t i{0U}; i < kRecords; ++i) {
            logger.info("record {}", i);
        }
        done.store(true, std::memory_order_release);
    }};

    // Records arrive in order; the dropped ones leave gaps that the counter accounts for.
    struct Check final {
        std::uint64_t received{0U};
        std::int64_t last{-1};
        bool ordered{true};
    } check{};
    const common::LogSink sink{[](const char* const text, const std::size_t length, void* const context) noexcept {
        Check& state{*static_cast<Check*>(context)};
        const std::string_view line{text, length};
        const std::size_t position{line.find("INFO record ")};
        if (position != std::string_view::npos) {
            std::int64_t value{0};
            for (const char digit : line.substr(position + 12U)) {
                if ((digit >= '0') && (digit <= '9')) {
                    value = (value * 10) + (digit - '0');
                }
            }
            state.ordered = state.ordered && (value > state.last);
            state.last = value;
            ++state.received;
        }
    }};
    for (std::uint32_t round{0U}; round < 10'000'000U; ++round) {
        const bool finished{done.load(std::memory_order_acquire)};
        const std::size_t lines{logger.drain(sink, &check, 64U)};
        if (finished && (lines == 0U)) {
            break;
        }
    }
    producer.join();
    CHECK(check.ordered);
    CHECK((check.received + logger.dropped()) == kRecords);
}
