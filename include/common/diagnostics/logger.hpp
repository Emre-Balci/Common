/// @file logger.hpp
/// @brief Logging from real-time threads: the RT thread queues a compact binary record, and a
///        non-RT thread formats it and writes it out.
///
/// The rule "no I/O on the RT path" leaves a real-time thread no direct way to report what it is
/// doing. Logger closes that gap. log() copies the time, the level, a pointer to the format string
/// and up to kMaxLogArguments arguments into a fixed-size LogRecord and pushes it into a wait-free
/// queue: no formatting, no I/O, no allocation, no lock, and a bounded number of steps. A non-RT
/// thread calls drain(), which formats each record into one line and hands the line to a LogSink
/// such as write_log_line_to_fd.
///
/// Usage pattern: one Logger per producing thread, and one non-RT thread that drains them all.
///
/// @code
///   // Static storage: each logger embeds its queue (Capacity * sizeof(LogRecord) bytes).
///   common::Logger<1024U> g_control_log{};
///   common::Logger<1024U> g_gateway_log{common::LogLevel::kInfo};
///
///   // Control thread (real-time). The format must be a string literal; arguments are copied.
///   g_control_log.warning("cycle {} overran by {} ns", cycle, overrun_ns);
///
///   // Logging thread (not real-time): drains every logger in turn.
///   common::NativeHandle fd{STDERR_FILENO};  // On Windows: ::GetStdHandle(STD_ERROR_HANDLE).
///   while (running) {
///       std::size_t lines{g_control_log.drain(&common::write_log_line_to_fd, &fd, 64U)};
///       lines += g_gateway_log.drain(&common::write_log_line_to_fd, &fd, 64U);
///       if (lines == 0U) {
///           static_cast<void>(common::sleep_for(std::chrono::milliseconds{1}));
///       }
///   }
/// @endcode
///
/// Each record becomes one line, "<seconds>.<nanoseconds> <LEVEL> <message>\n", for example
/// "12.000345120 WARNING cycle 7 overran by 1250 ns". The time is MonotonicClock time (seconds
/// since an unspecified start, usually boot), not wall-clock time.

#ifndef COMMON_DIAGNOSTICS_LOGGER_HPP
#define COMMON_DIAGNOSTICS_LOGGER_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>

#include "common/concurrency/spsc_queue.hpp"
#include "common/containers/span.hpp"
#include "common/core/assert.hpp"
#include "common/core/config.hpp"
#include "common/time/clock.hpp"

namespace common {

/// Severity of a log record, in increasing order.
enum class LogLevel : std::uint8_t {
    kDebug,
    kInfo,
    kWarning,
    kError,
};

/// @return "DEBUG", "INFO", "WARNING" or "ERROR" (the name printed in a log line), or "UNKNOWN"
///         for a value outside the enumeration.
[[nodiscard]] constexpr const char* to_string(const LogLevel level) noexcept {
    const char* text{"UNKNOWN"};
    switch (level) {
        case LogLevel::kDebug:
            text = "DEBUG";
            break;
        case LogLevel::kInfo:
            text = "INFO";
            break;
        case LogLevel::kWarning:
            text = "WARNING";
            break;
        case LogLevel::kError:
            text = "ERROR";
            break;
    }
    return text;
}

/// Most arguments one log record can carry; log() rejects more at compile time.
inline constexpr std::size_t kMaxLogArguments{6U};

namespace detail {
namespace logger {

// Character types print as characters (char) or are rejected (wide types); they are not integers here.
template <typename T>
inline constexpr bool kIsCharacter{std::is_same<T, char>::value || std::is_same<T, wchar_t>::value ||
                                   std::is_same<T, char16_t>::value || std::is_same<T, char32_t>::value};

/// signed char, short, int, long, long long (and the std::intN_t aliases).
template <typename T>
inline constexpr bool kIsSignedInteger{std::is_integral<T>::value && std::is_signed<T>::value && (!kIsCharacter<T>)};

/// unsigned char, unsigned short, unsigned int, unsigned long, unsigned long long (and std::uintN_t).
template <typename T>
inline constexpr bool kIsUnsignedInteger{std::is_integral<T>::value && std::is_unsigned<T>::value &&
                                         (!std::is_same<T, bool>::value) && (!kIsCharacter<T>)};

/// The default time of a LogRecord. The default constructor of std::chrono::time_point is not
/// noexcept; calling it through this noexcept function keeps LogRecord nothrow default-constructible,
/// as SpscQueue requires.
[[nodiscard]] constexpr TimePoint zero_time() noexcept {
    return TimePoint{};
}

[[nodiscard]] constexpr std::uint8_t level_rank(const LogLevel level) noexcept {
    return static_cast<std::uint8_t>(level);
}

}  // namespace logger
}  // namespace detail

/// One argument of a log record: a tagged copy of the value, 25 bytes, trivially copyable.
///
/// Everything is copied, text included, so the caller may reuse or free its buffers as soon as
/// log() returns. Each type maps explicitly, and never through a narrowing conversion:
/// - signed char, short, int, long, long long (and std::intN_t) -> kSigned, stored as std::int64_t.
/// - unsigned char, unsigned short, unsigned int, unsigned long, unsigned long long (and
///   std::uintN_t) -> kUnsigned, stored as std::uint64_t.
/// - float, double -> kDouble, stored as double (float converts exactly).
/// - bool -> kBool. char -> kChar, printed as the character.
/// - const char*, char*, a string literal, std::string_view -> kText, a copy of the text.
/// - An enumeration -> the kind of its underlying type.
///
/// std::int8_t and std::uint8_t are integers, so they print as numbers; only plain char prints as
/// a character. Text keeps at most kMaxTextLength characters: longer text keeps its first
/// kMaxTextLength - 3 characters followed by "...". A null const char* stores "(null)".
///
/// Rejected at compile time: long double (double cannot hold it without loss, so convert it
/// explicitly), wchar_t, char16_t and char32_t, and pointers other than to char (they would
/// otherwise print as a bool).
class LogArgument final {
public:
    enum class Kind : std::uint8_t {
        kNone,      ///< Default-constructed: holds no value and prints as "{}".
        kSigned,    ///< as_signed()
        kUnsigned,  ///< as_unsigned()
        kDouble,    ///< as_double()
        kBool,      ///< as_bool()
        kChar,      ///< as_char()
        kText,      ///< as_text()
    };

    /// Longest text an argument stores, including the "..." of a truncated text.
    static constexpr std::size_t kMaxTextLength{23U};

    constexpr LogArgument() noexcept = default;

    template <typename T, std::enable_if_t<detail::logger::kIsSignedInteger<T>, int> = 0>
    explicit LogArgument(const T value) noexcept {
        store(Kind::kSigned, std::int64_t{value});
    }

    template <typename T, std::enable_if_t<detail::logger::kIsUnsignedInteger<T>, int> = 0>
    explicit LogArgument(const T value) noexcept {
        store(Kind::kUnsigned, std::uint64_t{value});
    }

    explicit LogArgument(const float value) noexcept {
        store(Kind::kDouble, static_cast<double>(value));  // Exact: every float is a double.
    }

    explicit LogArgument(const double value) noexcept {
        store(Kind::kDouble, value);
    }

    explicit LogArgument(const bool value) noexcept {
        store(Kind::kBool, value);
    }

    explicit LogArgument(const char value) noexcept {
        store(Kind::kChar, value);
    }

    /// Copies the NUL-terminated @p text. At most kMaxTextLength + 1 characters are read, which is
    /// enough to tell whether the text has to be truncated.
    explicit LogArgument(const char* const text) noexcept {
        if (text == nullptr) {
            store_text(kNullText.data(), kNullText.size());
        } else {
            // Copying while scanning reads each character of the caller's text once, and none after
            // the terminator. (A separate copy after the scan makes GCC's -Warray-bounds report a
            // false positive for short char arrays.)
            std::array<char, kMaxTextLength + 1U> scanned{};
            std::size_t length{0U};
            while ((length <= kMaxTextLength) && (text[length] != '\0')) {
                scanned[length] = text[length];
                ++length;
            }
            store_text(scanned.data(), length);
        }
    }

    /// Copies @p text, which need not be NUL-terminated (e.g. a field of a received message).
    explicit LogArgument(const std::string_view text) noexcept {
        store_text(text.data(), text.size());
    }

    /// An enumeration is stored as its underlying type: an enumeration with a char underlying type
    /// (e.g. enum class Side : char { kBuy = 'B' }) prints as a character, any other as a number.
    template <typename E, std::enable_if_t<std::is_enum<E>::value, int> = 0>
    explicit LogArgument(const E value) noexcept : LogArgument{static_cast<std::underlying_type_t<E>>(value)} {}

    explicit LogArgument(long double value) = delete;
    explicit LogArgument(wchar_t value) = delete;
    explicit LogArgument(char16_t value) = delete;
    explicit LogArgument(char32_t value) = delete;
    template <typename T>
    explicit LogArgument(const T* pointer) = delete;

    [[nodiscard]] constexpr Kind kind() const noexcept {
        return kind_;
    }

    /// @pre kind() == Kind::kSigned
    [[nodiscard]] std::int64_t as_signed() const noexcept {
        COMMON_ASSERT(kind_ == Kind::kSigned);
        return load<std::int64_t>();
    }

    /// @pre kind() == Kind::kUnsigned
    [[nodiscard]] std::uint64_t as_unsigned() const noexcept {
        COMMON_ASSERT(kind_ == Kind::kUnsigned);
        return load<std::uint64_t>();
    }

    /// @pre kind() == Kind::kDouble
    [[nodiscard]] double as_double() const noexcept {
        COMMON_ASSERT(kind_ == Kind::kDouble);
        return load<double>();
    }

    /// @pre kind() == Kind::kBool
    [[nodiscard]] bool as_bool() const noexcept {
        COMMON_ASSERT(kind_ == Kind::kBool);
        return load<bool>();
    }

    /// @pre kind() == Kind::kChar
    [[nodiscard]] char as_char() const noexcept {
        COMMON_ASSERT(kind_ == Kind::kChar);
        return payload_[0U];
    }

    /// The stored text as given, including a "..." that marks truncation. The view points into
    /// this argument and is valid while the argument exists.
    /// @pre kind() == Kind::kText
    [[nodiscard]] std::string_view as_text() const noexcept {
        COMMON_ASSERT(kind_ == Kind::kText);
        const std::size_t length{(length_ <= kMaxTextLength) ? length_ : kMaxTextLength};
        return std::string_view{payload_.data(), length};
    }

private:
    static constexpr std::array<char, 6U> kNullText{'(', 'n', 'u', 'l', 'l', ')'};
    static constexpr std::array<char, 3U> kEllipsis{'.', '.', '.'};

    // A value is stored as its bytes: no union, and std::memcpy is the defined way to reinterpret them.
    template <typename T>
    void store(const Kind kind, const T value) noexcept {
        static_assert(std::is_trivially_copyable<T>::value && (sizeof(T) <= kMaxTextLength), "payload too large");
        static_cast<void>(std::memcpy(payload_.data(), &value, sizeof(T)));
        kind_ = kind;
    }

    template <typename T>
    [[nodiscard]] T load() const noexcept {
        T value{};
        static_cast<void>(std::memcpy(&value, payload_.data(), sizeof(T)));
        return value;
    }

    /// @pre text points to min(length, kMaxTextLength) readable characters (nullptr if length == 0).
    void store_text(const char* const text, const std::size_t length) noexcept {
        const bool truncated{length > kMaxTextLength};
        const std::size_t kept{truncated ? (kMaxTextLength - kEllipsis.size()) : length};
        for (std::size_t i{0U}; i < kept; ++i) {
            payload_[i] = text[i];
        }
        std::size_t stored{kept};
        if (truncated) {
            for (const char dot : kEllipsis) {
                payload_[stored] = dot;
                ++stored;
            }
        }
        length_ = static_cast<std::uint8_t>(stored);
        kind_ = Kind::kText;
    }

    // kText: the characters. kChar: payload_[0]. Numbers and bool: the bytes of the value.
    std::array<char, kMaxTextLength> payload_{};
    std::uint8_t length_{0U};  // kText: number of characters in payload_.
    Kind kind_{Kind::kNone};
};

static_assert(std::is_trivially_copyable<LogArgument>::value, "LogArgument must be trivially copyable");

/// One log call as it travels from the producing thread to the draining thread: fixed size
/// (168 bytes on x86-64 and AArch64) and trivially copyable.
///
/// @warning @ref format is NOT copied. The record holds only the pointer, and the string is read
///          later on the draining thread. It must have static storage duration, which in practice
///          means a string literal. Never pass a stack buffer, a std::string's c_str() or any other
///          storage that may change or disappear: pass such text as an argument ("{}"), which is
///          copied.
struct LogRecord final {
    TimePoint time{detail::logger::zero_time()};
    const char* format{nullptr};  ///< Static storage duration; nullptr prints an empty message.
    LogLevel level{LogLevel::kDebug};
    std::uint8_t argument_count{0U};  ///< Number of valid entries in arguments, <= kMaxLogArguments.
    std::array<LogArgument, kMaxLogArguments> arguments{};
};

static_assert(std::is_trivially_copyable<LogRecord>::value, "LogRecord must be trivially copyable");
static_assert(std::is_nothrow_default_constructible<LogRecord>::value, "SpscQueue needs a noexcept LogRecord{}");
static_assert(sizeof(LogRecord) <= 256U, "LogRecord must stay compact");

/// Receives one formatted line: @p length characters at @p text, ending with '\n'. The text is
/// only valid during the call. Sinks run on the draining thread, never on a real-time thread.
using LogSink = void (*)(const char* text, std::size_t length, void* context) noexcept;

/// Formats @p record as one line, "<seconds>.<9-digit nanoseconds> <LEVEL> <message>\n", into
/// @p out, without allocation, stdio or locale.
///
/// In the message, each "{}" is replaced by the next argument; "{{" and "}}" print a single brace;
/// a "{}" without an argument left prints "{}"; surplus arguments are ignored; any other brace
/// prints as it is. At most 4096 characters of the format are read.
///
/// Arguments print as follows:
/// - Integers: exactly, in decimal.
/// - double: when |x| < 1e15, in fixed notation with up to 6 fractional digits (trailing zeros and
///   a trailing point removed): 0.1 -> "0.1", 2.0 -> "2", 0.0000004 -> "0". These digits are exact,
///   the same as printf("%.6f") with its trailing zeros removed, including the sign of zero
///   (-0.0 -> "-0"). Larger values print in scientific notation with up to 6 fractional digits:
///   1e20 -> "1e+20". There the last digit may differ from printf, because the scaling uses
///   double arithmetic instead of stdio. NaN prints as "nan" and infinities as "inf" and "-inf".
/// - bool: "true" or "false".
/// - char and text: as they are, except that control characters (0x00-0x1F and 0x7F) print as '?',
///   so an argument can never break a record into several lines or send terminal commands.
///
/// When the line does not fit, it is truncated to out.size() - 1 characters followed by '\n'.
/// No NUL terminator is written. Callable on any thread; meant for the draining thread.
/// @return the number of characters written: 0 if @p out is empty, otherwise the line length
///         including the '\n'.
[[nodiscard]] std::size_t format_log_record(const LogRecord& record, Span<char> out) noexcept;

/// A ready-made LogSink: writes the line to a file descriptor with ::write() (WriteFile() on
/// Windows), continuing after partial writes and restarting after EINTR (a bounded number of times).
///
/// @p context must point to a NativeHandle holding the descriptor: on Linux an int such as
/// STDERR_FILENO or the descriptor of an open log file, on Windows a HANDLE such as
/// GetStdHandle(STD_ERROR_HANDLE) or File::descriptor(). The rest of a line is dropped when the
/// write fails, including EAGAIN on a non-blocking descriptor, because a sink cannot report errors.
/// On Linux, writing to a pipe or socket whose reader has gone raises SIGPIPE unless the process
/// ignores that signal. Blocking I/O: never call it on a real-time thread.
/// @pre context != nullptr, and text != nullptr unless length == 0.
void write_log_line_to_fd(const char* text, std::size_t length, void* context) noexcept;

/// Log channel from exactly one producer thread (typically real-time) to one draining thread
/// (never real-time).
///
/// Threads:
/// - log(), log_at(), debug(), info(), warning(), error(): the single producer thread. RT-safe:
///   wait-free, no allocation, no I/O, no formatting, bounded copy of at most sizeof(LogRecord)
///   bytes. A record below min_level() is skipped before the clock is read or anything is copied.
///   When the queue is full the record is dropped and counted, and log() returns at once.
/// - drain(): the single consumer thread. Formats and calls the sink: not RT-safe.
/// - set_min_level(), min_level(), enabled(), dropped(), size_approx(): any thread.
///
/// The format string is not copied and must have static storage duration (a string literal):
/// see LogRecord. Arguments are copied (see LogArgument).
///
/// The object embeds Capacity records of sizeof(LogRecord) bytes (168 KiB for 1024): give it
/// static storage duration, never a thread's stack. Capacity must be a power of two.
template <std::size_t Capacity>
class Logger final {
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "64-bit atomics must be lock-free");
    static_assert(std::atomic<LogLevel>::is_always_lock_free, "std::atomic<LogLevel> must be lock-free");

public:
    /// Longest line drain() passes to a sink, including the '\n'; longer lines are truncated.
    static constexpr std::size_t kMaxLineLength{511U};

    /// Records at @p min_level and above are queued; the default queues everything.
    constexpr explicit Logger(const LogLevel min_level = LogLevel::kDebug) noexcept : min_level_{min_level} {}
    ~Logger() = default;
    Logger(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger& operator=(Logger&&) = delete;

    /// Producer thread only. Queues a record of @p level with the current MonotonicClock time.
    /// @p format must have static storage duration (a string literal). At most kMaxLogArguments
    /// arguments; their types are listed at LogArgument.
    /// @pre format != nullptr
    template <typename... Args>
    void log(const LogLevel level, const char* const format, const Args&... args) noexcept {
        static_assert(sizeof...(Args) <= kMaxLogArguments, "a log record holds at most kMaxLogArguments arguments");
        if (enabled(level)) {
            push(MonotonicClock::now(), level, format, args...);
        }
    }

    /// Producer thread only. Like log(), with a time the caller already has (e.g. the time the
    /// cycle read), which saves a clock read and makes the record deterministic in tests.
    template <typename... Args>
    void log_at(const TimePoint time, const LogLevel level, const char* const format, const Args&... args) noexcept {
        static_assert(sizeof...(Args) <= kMaxLogArguments, "a log record holds at most kMaxLogArguments arguments");
        if (enabled(level)) {
            push(time, level, format, args...);
        }
    }

    template <typename... Args>
    void debug(const char* const format, const Args&... args) noexcept {
        log(LogLevel::kDebug, format, args...);
    }

    template <typename... Args>
    void info(const char* const format, const Args&... args) noexcept {
        log(LogLevel::kInfo, format, args...);
    }

    template <typename... Args>
    void warning(const char* const format, const Args&... args) noexcept {
        log(LogLevel::kWarning, format, args...);
    }

    template <typename... Args>
    void error(const char* const format, const Args&... args) noexcept {
        log(LogLevel::kError, format, args...);
    }

    /// Any thread. Records below @p level are skipped from now on (relaxed: the producer sees the
    /// change within a few calls).
    void set_min_level(const LogLevel level) noexcept {
        min_level_.store(level, std::memory_order_relaxed);
    }

    /// Any thread.
    [[nodiscard]] LogLevel min_level() const noexcept {
        return min_level_.load(std::memory_order_relaxed);
    }

    /// Any thread. @return true if a record of @p level would be queued. Lets the producer skip
    /// computing expensive arguments.
    [[nodiscard]] bool enabled(const LogLevel level) const noexcept {
        return detail::logger::level_rank(level) >= detail::logger::level_rank(min_level());
    }

    /// Any thread. Total number of records dropped because the queue was full. Records skipped by
    /// the level filter are not counted.
    [[nodiscard]] std::uint64_t dropped() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

    /// Any thread. Number of queued records; may be stale by the time it is used.
    [[nodiscard]] std::size_t size_approx() const noexcept {
        return queue_.size_approx();
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Capacity;
    }

    /// Consumer thread only (not real-time). Pops at most @p max_records records, formats each with
    /// format_log_record() into a buffer of kMaxLineLength + 1 characters on the stack and passes the
    /// line to @p sink with @p context. The text passed to the sink is also NUL-terminated.
    ///
    /// If records were dropped since the previous drain(), it first passes one WARNING line
    /// "dropped <n> log record(s): queue full" (this line is not counted and is sent even when
    /// @p max_records is 0).
    /// @pre sink != nullptr
    /// @return the number of records formatted (0 when the queue is empty).
    std::size_t drain(const LogSink sink, void* const context, const std::size_t max_records) noexcept {
        COMMON_ASSERT(sink != nullptr);
        std::size_t formatted{0U};
        // The guard keeps the queue intact when assertions are disabled.
        if (sink != nullptr) {
            std::array<char, kMaxLineLength + 1U> line{};
            const std::uint64_t total_dropped{dropped_.load(std::memory_order_relaxed)};
            if (total_dropped != reported_dropped_) {
                const LogRecord notice{MonotonicClock::now(),
                                       "dropped {} log record(s): queue full",
                                       LogLevel::kWarning,
                                       1U,
                                       {{LogArgument{total_dropped - reported_dropped_}}}};
                emit(notice, sink, context, line);
                reported_dropped_ = total_dropped;
            }
            bool more{true};
            while (more && (formatted < max_records)) {
                const LogRecord* const record{queue_.front()};
                if (record != nullptr) {
                    emit(*record, sink, context, line);
                    queue_.pop();
                    ++formatted;
                } else {
                    more = false;
                }
            }
        }
        return formatted;
    }

private:
    template <typename... Args>
    void push(const TimePoint time, const LogLevel level, const char* const format, const Args&... args) noexcept {
        COMMON_ASSERT(format != nullptr);
        constexpr std::uint8_t kArgumentCount{static_cast<std::uint8_t>(sizeof...(Args))};
        const LogRecord record{time, format, level, kArgumentCount, {{LogArgument{args}...}}};
        if (!queue_.try_push(record)) {
            // Only the producer writes the counter, so a load and a store suffice: no locked
            // read-modify-write instruction on the RT path.
            dropped_.store(dropped_.load(std::memory_order_relaxed) + 1U, std::memory_order_relaxed);
        }
    }

    static void emit(const LogRecord& record, const LogSink sink, void* const context,
                     std::array<char, kMaxLineLength + 1U>& line) noexcept {
        // The last character is kept for the NUL terminator.
        const std::size_t length{format_log_record(record, Span<char>{line.data(), kMaxLineLength})};
        line[length] = '\0';
        sink(line.data(), length, context);
    }

    SpscQueue<LogRecord, Capacity> queue_{};
    // Written by the producer (on drops) and the configuring thread, read by everyone.
    alignas(kCacheLineSize) std::atomic<std::uint64_t> dropped_{0U};
    std::atomic<LogLevel> min_level_;
    // Consumer-owned: the value of dropped_ at the last report.
    alignas(kCacheLineSize) std::uint64_t reported_dropped_{0U};
};

}  // namespace common

#endif  // COMMON_DIAGNOSTICS_LOGGER_HPP
