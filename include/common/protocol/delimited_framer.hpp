/// @file delimited_framer.hpp
/// @brief Splits a byte stream into frames that end at a delimiter byte, e.g. lines of a serial
///        protocol.

#ifndef COMMON_PROTOCOL_DELIMITED_FRAMER_HPP
#define COMMON_PROTOCOL_DELIMITED_FRAMER_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "common/containers/span.hpp"
#include "common/core/assert.hpp"

namespace common {

namespace detail {
namespace delimited_framer {

/// The byte that represents @p character in a byte stream: the character's object representation,
/// read through unsigned char, which may access the bytes of any object (exception to MISRA C++:2023
/// Rule 8.2.5). The character is never converted to an integer, so its numerical value is not used
/// (Rule 7.0.3).
[[nodiscard]] inline std::uint8_t byte_of(const char& character) noexcept {
    return *reinterpret_cast<const std::uint8_t*>(&character);
}

}  // namespace delimited_framer
}  // namespace detail

/// Set of byte values, e.g. the delimiters of a protocol. 32 bytes, O(1) lookup; the operations on
/// byte values are constexpr. A character stands for the byte that represents it in a byte stream,
/// e.g. '\r' for the byte a device sends at the end of a line.
/// @code
///   const common::ByteSet delimiters{common::ByteSet{}.with('\r').with('>')};
/// @endcode
class ByteSet final {
public:
    /// Empty set.
    constexpr ByteSet() noexcept = default;

    /// @return a copy of this set with @p value added.
    [[nodiscard]] constexpr ByteSet with(const std::uint8_t value) const noexcept {
        ByteSet result{*this};
        result.words_[word(value)] |= bit(value);
        return result;
    }

    /// @return a copy of this set with the byte that represents @p character added.
    [[nodiscard]] ByteSet with(const char character) const noexcept {
        return with(detail::delimited_framer::byte_of(character));
    }

    [[nodiscard]] constexpr bool contains(const std::uint8_t value) const noexcept {
        return (words_[word(value)] & bit(value)) != 0U;
    }

    /// @return true if the byte that represents @p character is in the set.
    [[nodiscard]] bool contains(const char character) const noexcept {
        return contains(detail::delimited_framer::byte_of(character));
    }

    [[nodiscard]] constexpr bool empty() const noexcept {
        bool result{true};
        for (std::size_t index{0U}; index < kWords; ++index) {
            result = result && (words_[index] == 0U);
        }
        return result;
    }

    /// @return true if no value is in both sets.
    [[nodiscard]] constexpr bool disjoint(const ByteSet& other) const noexcept {
        bool result{true};
        for (std::size_t index{0U}; index < kWords; ++index) {
            result = result && ((words_[index] & other.words_[index]) == 0U);
        }
        return result;
    }

private:
    static constexpr std::size_t kBitsPerWord{64U};
    static constexpr std::size_t kWords{256U / kBitsPerWord};

    [[nodiscard]] static constexpr std::size_t word(const std::uint8_t value) noexcept {
        return value / kBitsPerWord;
    }

    [[nodiscard]] static constexpr std::uint64_t bit(const std::uint8_t value) noexcept {
        return std::uint64_t{1U} << (value % kBitsPerWord);
    }

    std::array<std::uint64_t, kWords> words_{};
};

/// Reassembles frames from a byte stream that arrives in arbitrary pieces, e.g. from SerialPort
/// reads. A frame is the bytes up to a delimiter byte; the delimiter ends the frame and is not part
/// of it, but delimiter() reports which one it was. Ignored bytes are dropped wherever they occur.
///
/// Example: a device that ends response lines with "\r" (sometimes followed by "\n"), sends stray
/// NUL bytes, and prints ">" when it is ready for the next command:
/// @code
///   common::DelimitedFramer<128U> framer{common::ByteSet{}.with('\r').with('>'),
///                                        common::ByteSet{}.with('\n').with('\0')};
///   // For each block of received bytes:
///   common::Span<const std::uint8_t> rest{received};
///   for (std::size_t round{0U}; (!rest.empty()) && (round < kMaxFramesPerRead); ++round) {
///       rest = rest.subspan(framer.consume(rest));
///       if (framer.has_frame()) {
///           if (framer.delimiter() == '>') { /* the response is complete */ }
///           else { handle_line(framer.text()); }
///       }
///   }
/// @endcode
/// Empty frames are reported too (e.g. the empty frame before ">", or between "\r\r"); skip them
/// with text().empty() when they carry no meaning.
///
/// Overlong frames: a frame holds at most MaxFrameSize bytes. Further bytes are dropped until the
/// next delimiter, and the frame is then reported with truncated() set, holding its first
/// MaxFrameSize bytes. The framer recovers at the next delimiter, so one corrupt or oversized line
/// never desynchronises the stream.
///
/// consume() is O(input size), never allocates or blocks, and is safe on the real-time path. The
/// object embeds its buffer. Not thread-safe: one thread feeds a framer.
template <std::size_t MaxFrameSize>
class DelimitedFramer final {
    static_assert(MaxFrameSize > 0U, "MaxFrameSize must be positive");
    // text() and delimiter() view the received bytes as characters, which is valid because char may
    // access the bytes of any object.
    static_assert(std::is_same<std::uint8_t, unsigned char>::value, "std::uint8_t must be unsigned char");

public:
    static constexpr std::size_t kMaxFrameSize{MaxFrameSize};

    /// @pre delimiters is not empty, and no byte is both a delimiter and ignored.
    constexpr DelimitedFramer(const ByteSet& delimiters, const ByteSet& ignored) noexcept
        : delimiters_{delimiters}, ignored_{ignored} {
        COMMON_ASSERT(!delimiters.empty());
        COMMON_ASSERT(delimiters.disjoint(ignored));
    }

    /// Consumes bytes from the start of @p input until a frame is complete or @p input is used up.
    /// A frame completed by an earlier call is discarded first. Call it again with the rest of the
    /// input (input.subspan(returned count)) to get the following frames.
    /// @return the number of bytes consumed; when has_frame() is true, the last one was the delimiter.
    [[nodiscard]] constexpr std::size_t consume(const Span<const std::uint8_t> input) noexcept {
        if (has_frame_) {
            start_next_frame();
        }
        std::size_t consumed{0U};
        while ((!has_frame_) && (consumed < input.size())) {
            const std::uint8_t value{input[consumed]};
            ++consumed;
            if (delimiters_.contains(value)) {
                delimiter_ = value;
                has_frame_ = true;
                if (truncated_) {
                    ++truncated_count_;
                }
            } else if (ignored_.contains(value)) {
                // Dropped wherever it occurs.
            } else if (size_ < MaxFrameSize) {
                buffer_[size_] = value;
                ++size_;
            } else {
                truncated_ = true;
            }
        }
        return consumed;
    }

    /// @return true when consume() has completed a frame; it stays available until the next
    ///         consume() or reset().
    [[nodiscard]] constexpr bool has_frame() const noexcept {
        return has_frame_;
    }

    /// The completed frame as text, without its delimiter: the received bytes viewed as characters.
    /// @pre has_frame()
    [[nodiscard]] Span<const char> text() const noexcept {
        COMMON_ASSERT(has_frame_);
        // char may access the bytes of any object (exception to MISRA C++:2023 Rule 8.2.5).
        return Span<const char>{reinterpret_cast<const char*>(buffer_.data()), size_};
    }

    /// The completed frame as bytes, without its delimiter. @pre has_frame()
    [[nodiscard]] constexpr Span<const std::uint8_t> bytes() const noexcept {
        COMMON_ASSERT(has_frame_);
        return Span<const std::uint8_t>{buffer_.data(), size_};
    }

    /// The character that the delimiter byte ending the frame represents, e.g. '>'. @pre has_frame()
    [[nodiscard]] char delimiter() const noexcept {
        COMMON_ASSERT(has_frame_);
        // char may access the bytes of any object (exception to MISRA C++:2023 Rule 8.2.5).
        return *reinterpret_cast<const char*>(&delimiter_);
    }

    /// @return true if the completed frame was longer than MaxFrameSize, so its end was dropped.
    ///         @pre has_frame()
    [[nodiscard]] constexpr bool truncated() const noexcept {
        COMMON_ASSERT(has_frame_);
        return truncated_;
    }

    /// @return the number of bytes of the frame being assembled (0 when a frame is complete); e.g.
    ///         to detect a partial response left over when a timeout expires.
    [[nodiscard]] constexpr std::size_t pending_size() const noexcept {
        return has_frame_ ? 0U : size_;
    }

    /// Total number of truncated frames since construction.
    [[nodiscard]] constexpr std::uint64_t truncated_count() const noexcept {
        return truncated_count_;
    }

    /// Discards the completed frame or the partial one, e.g. after a timeout or when the device is
    /// reset. The next byte starts a new frame.
    constexpr void reset() noexcept {
        start_next_frame();
    }

private:
    constexpr void start_next_frame() noexcept {
        size_ = 0U;
        has_frame_ = false;
        truncated_ = false;
    }

    std::array<std::uint8_t, MaxFrameSize> buffer_{};
    ByteSet delimiters_;
    ByteSet ignored_;
    std::size_t size_{0U};
    std::uint64_t truncated_count_{0U};
    std::uint8_t delimiter_{0U};
    bool has_frame_{false};
    bool truncated_{false};
};

}  // namespace common

#endif  // COMMON_PROTOCOL_DELIMITED_FRAMER_HPP
