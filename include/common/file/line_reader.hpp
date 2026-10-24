/// @file line_reader.hpp
/// @brief Line-by-line reading of text files through a fixed buffer, without heap or stdio.
///
/// Meant for the initialisation phase: configuration files, CSV reference data, calibration
/// tables. Reading blocks in the kernel like every file operation, so nothing in this header is
/// safe on the real-time path.

#ifndef COMMON_FILE_LINE_READER_HPP
#define COMMON_FILE_LINE_READER_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "common/containers/span.hpp"
#include "common/core/assert.hpp"
#include "common/core/status.hpp"
#include "common/file/file.hpp"

namespace common {

/// Splits the contents of a File into lines. The bytes are read with File::read() into an inline
/// buffer of @p BufferSize bytes, and each line is handed out as a view into that buffer.
///
/// Line format:
/// - A line ends at "\n". A "\r" right before the "\n" belongs to the terminator, so "\n" and
///   "\r\n" files read the same; any other "\r" is part of the line.
/// - A last line without a final "\n" is still returned. An empty file has no lines, and an empty
///   line yields an empty span.
/// - Lines are not NUL-terminated and may contain any byte other than "\n", including NUL.
///
/// Line length: a line fits when the bytes before its "\n" (a "\r" included), or all of its bytes
/// for a last line without "\n", number at most kMaxLineLength = BufferSize - 1. A longer line
/// stops the reader with kOutOfRange. The caller decides what that means, e.g. reject the file
/// and report line line_number() + 1, or read it again with a larger BufferSize.
///
/// Bounded work: a next() call makes at most BufferSize calls of File::read(), because each one
/// adds at least one byte to the buffer or ends the input, and it examines each buffered byte
/// once. The reader does not limit the number of lines, so the caller's loop needs its own bound,
/// e.g. a maximum line count:
/// @code
///   common::LineReader<256U> reader{file};  // file: a common::File opened for reading.
///   common::Span<const char> line{};
///   for (std::uint32_t count{0U}; (count < kMaxLines) && reader.next(line); ++count) {
///       handle(line);
///   }
///   if (!common::is_ok(reader.status())) {
///       // E.g. kOutOfRange: line reader.line_number() + 1 is longer than kMaxLineLength.
///   }
/// @endcode
///
/// The reader starts at the File's current position and keeps a pointer to the File: the File
/// must outlive the reader and must not be read, written, moved or repositioned by anyone else
/// while the reader is in use.
///
/// Not thread-safe: one thread uses a reader at a time. File::read() blocks (on storage, or on a
/// pipe until data arrives), so never use a reader on a real-time thread. The object embeds its
/// buffer; for a large BufferSize prefer static storage duration to a small thread stack.
template <std::size_t BufferSize>
class LineReader final {
    static_assert(BufferSize >= 2U, "BufferSize must be at least 2");
    // File::read() fills bytes and lines are views of chars over the same storage. That is valid
    // because std::uint8_t is unsigned char, which may access the bytes of any object.
    static_assert(std::is_same<std::uint8_t, unsigned char>::value, "std::uint8_t must be unsigned char");

public:
    /// The longest line that fits: the number of bytes before its "\n", a "\r" included.
    static constexpr std::size_t kMaxLineLength{BufferSize - 1U};

    /// Reads from @p file, which the caller opened for reading and which must outlive the reader.
    /// Nothing is read before the first next().
    explicit LineReader(File& file) noexcept : file_{&file} {}

    ~LineReader() = default;
    LineReader(const LineReader&) = delete;
    LineReader(LineReader&&) = delete;
    LineReader& operator=(const LineReader&) = delete;
    LineReader& operator=(LineReader&&) = delete;

    /// Reads the next line into @p line, without its terminator. The span views the reader's
    /// buffer and stays valid until the next call of next() or the end of the reader.
    /// @return true with a line; false at the end of the input or on an error, with @p line empty:
    ///         status() tells which. A partial line read before an error is not returned. Once
    ///         next() has returned false, it always returns false.
    [[nodiscard]] bool next(Span<const char>& line) noexcept {
        line = Span<const char>{};
        bool found{false};
        std::size_t checked{0U};  // Unread bytes already known to hold no "\n".
        for (std::size_t round{0U}; (!found) && (!finished_) && (round < kMaxRounds); ++round) {
            const std::size_t newline{find_newline(begin_ + checked)};
            if (newline < end_) {
                std::size_t length{newline - begin_};
                if ((length > 0U) && (buffer_[newline - 1U] == '\r')) {
                    --length;  // The "\r" of a "\r\n".
                }
                line = Span<const char>{buffer_}.subspan(begin_, length);
                begin_ = newline + 1U;
                found = true;
            } else if (end_of_input_ && (begin_ < end_)) {
                // The last line, without "\n".
                line = Span<const char>{buffer_}.subspan(begin_, end_ - begin_);
                begin_ = end_;
                found = true;
            } else if (end_of_input_) {
                finished_ = true;  // Clean end: status_ stays kOk.
            } else if ((end_ - begin_) == BufferSize) {
                status_ = Status::kOutOfRange;  // The line does not fit in the buffer.
                finished_ = true;
            } else {
                checked = end_ - begin_;
                refill();
            }
        }
        COMMON_ASSERT(found || finished_);  // kMaxRounds always suffices.
        if (found) {
            ++line_number_;  // Cannot wrap: every line consumes at least one byte of input.
        }
        return found;
    }

    /// @return kOk while lines are read and after a clean end of the input; the error File::read()
    ///         reported (e.g. kInvalidArgument for a File that is not open); or kOutOfRange when a
    ///         line longer than kMaxLineLength stopped the reader.
    [[nodiscard]] Status status() const noexcept {
        return status_;
    }

    /// @return the 1-based number of the last line returned, 0 before the first one. When next()
    ///         fails, the line it was reading is line_number() + 1.
    [[nodiscard]] std::uint64_t line_number() const noexcept {
        return line_number_;
    }

private:
    // A round of next() either ends the call (a line, the end of the input, an error, a full
    // buffer) or reads at least one byte into a buffer of BufferSize bytes. So a call has at most
    // BufferSize reading rounds and one final round.
    static constexpr std::size_t kMaxRounds{BufferSize + 1U};

    /// @return the index of the first "\n" in [from, end_), or end_ if there is none.
    [[nodiscard]] std::size_t find_newline(const std::size_t from) const noexcept {
        std::size_t position{end_};
        for (std::size_t index{from}; index < end_; ++index) {
            if (buffer_[index] == '\n') {
                position = index;
                break;
            }
        }
        return position;
    }

    /// Moves the unread bytes to the front of the buffer and appends what one File::read() returns.
    /// Called with fewer than BufferSize unread bytes, so the read is never empty and 0 means the
    /// end of the input.
    void refill() noexcept {
        const std::size_t unread{end_ - begin_};
        if (begin_ > 0U) {
            const Span<char> buffer{buffer_};
            const Span<char> rest{buffer.subspan(begin_, unread)};
            // The destination starts before the source, which std::copy allows for overlapping ranges.
            static_cast<void>(std::copy(rest.begin(), rest.end(), buffer.begin()));
            begin_ = 0U;
            end_ = unread;
        }
        const Span<char> space{Span<char>{buffer_}.subspan(end_)};
        // A pointer to unsigned char may access the bytes of any object (exception to MISRA C++:2023
        // Rule 8.2.5).
        const Span<std::uint8_t> bytes{reinterpret_cast<std::uint8_t*>(space.data()), space.size()};
        const Result<std::size_t> count{file_->read(bytes)};
        if (!count.has_value()) {
            status_ = count.status();
            finished_ = true;
        } else if (count.value() == 0U) {
            end_of_input_ = true;
        } else {
            end_ += count.value();
        }
    }

    File* file_;
    std::array<char, BufferSize> buffer_{};
    std::size_t begin_{0U};  ///< First unread byte in buffer_.
    std::size_t end_{0U};    ///< One past the last byte read into buffer_.
    std::uint64_t line_number_{0U};
    Status status_{Status::kOk};
    bool end_of_input_{false};  ///< File::read() reported the end of the file.
    bool finished_{false};      ///< The input is exhausted or failed: next() returns false.
};

}  // namespace common

#endif  // COMMON_FILE_LINE_READER_HPP
