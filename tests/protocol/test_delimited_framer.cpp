#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/containers/span.hpp"
#include "common/protocol/delimited_framer.hpp"
#include "test_framework.hpp"

namespace {

common::Span<const std::uint8_t> bytes_of(const std::string_view text) {
    return common::Span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

struct Frame final {
    std::string text;
    char delimiter;
    bool truncated;
};

/// Feeds @p input in pieces of @p piece bytes and collects every frame.
template <std::size_t N>
std::vector<Frame> split(common::DelimitedFramer<N>& framer, const std::string_view input, const std::size_t piece) {
    std::vector<Frame> frames{};
    for (std::size_t offset{0U}; offset < input.size(); offset += piece) {
        common::Span<const std::uint8_t> rest{bytes_of(input.substr(offset, piece))};
        while (!rest.empty()) {
            rest = rest.subspan(framer.consume(rest));
            if (framer.has_frame()) {
                frames.push_back(Frame{std::string{framer.text().data(), framer.text().size()}, framer.delimiter(),
                                       framer.truncated()});
            }
        }
    }
    return frames;
}

const common::ByteSet kDelimiters{common::ByteSet{}.with('\r').with('>')};
const common::ByteSet kIgnored{common::ByteSet{}.with('\n').with('\0')};

}  // namespace

TEST_CASE(byte_set) {
    constexpr common::ByteSet kBytes{common::ByteSet{}.with(std::uint8_t{0x0DU}).with(std::uint8_t{0xFFU})};
    static_assert(kBytes.contains(std::uint8_t{0x0DU}), "constexpr for byte values");
    static_assert(!kBytes.contains(std::uint8_t{0x0AU}), "constexpr for byte values");
    const common::ByteSet set{common::ByteSet{}.with('a').with(std::uint8_t{0xFFU}).with('\0')};
    CHECK(set.contains('a'));
    CHECK(set.contains(std::uint8_t{0xFFU}));
    CHECK(set.contains('\0'));
    CHECK(!set.contains('b'));
    CHECK(!set.empty());
    CHECK(common::ByteSet{}.empty());
    CHECK(!set.disjoint(common::ByteSet{}.with('a')));
    CHECK(set.disjoint(common::ByteSet{}.with('b')));
    // A character stands for the byte that represents it.
    CHECK(kBytes.contains('\r'));
    CHECK(kBytes.contains('\xFF'));
}

TEST_CASE(byte_set_and_framer_map_every_character_to_its_byte) {
    std::uint32_t mismatches{0U};
    for (std::uint32_t code{0U}; code < 256U; ++code) {
        const char character{static_cast<char>(code)};
        const std::uint8_t byte{static_cast<std::uint8_t>(code)};
        const common::ByteSet from_character{common::ByteSet{}.with(character)};
        mismatches += from_character.contains(byte) ? 0U : 1U;
        mismatches += from_character.contains(static_cast<std::uint8_t>((code + 1U) % 256U)) ? 1U : 0U;
        mismatches += common::ByteSet{}.with(byte).contains(character) ? 0U : 1U;

        // The byte as delimiter ends a frame, and delimiter() returns the character again; the same
        // byte as content comes back unchanged through text() and bytes().
        common::DelimitedFramer<4U> framer{from_character, common::ByteSet{}};
        const std::array<std::uint8_t, 2U> content{{static_cast<std::uint8_t>((code + 1U) % 256U), byte}};
        mismatches += (framer.consume(content) == 2U) ? 0U : 1U;
        if (framer.has_frame()) {
            mismatches += (framer.delimiter() == character) ? 0U : 1U;
            mismatches += (framer.text().size() == 1U) ? 0U : 1U;
            mismatches += (framer.text()[0U] == static_cast<char>((code + 1U) % 256U)) ? 0U : 1U;
            mismatches += (framer.bytes()[0U] == content[0U]) ? 0U : 1U;
        } else {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0U);
}

TEST_CASE(delimited_framer_device_responses) {
    // An echoed command, a search notice, two data lines, a blank line and the prompt, with a "\n"
    // after some "\r" and a stray NUL.
    using namespace std::string_view_literals;  // An sv literal keeps the embedded NUL.
    const std::string_view stream{"01 0C\rSEARCHING...\r\n41 0C 1A F8\r7E8 04 41\0 0C\r\r>"sv};
    for (std::size_t piece{1U}; piece <= stream.size(); ++piece) {
        common::DelimitedFramer<32U> framer{kDelimiters, kIgnored};
        const std::vector<Frame> frames{split(framer, stream, piece)};
        REQUIRE(frames.size() == 6U);
        CHECK(frames[0U].text == "01 0C");
        CHECK(frames[1U].text == "SEARCHING...");
        CHECK(frames[2U].text == "41 0C 1A F8");
        CHECK(frames[3U].text == "7E8 04 41 0C");
        CHECK(frames[4U].text.empty());
        CHECK(frames[4U].delimiter == '\r');
        CHECK(frames[5U].text.empty());
        CHECK(frames[5U].delimiter == '>');
        CHECK(!frames[2U].truncated);
        CHECK(framer.pending_size() == 0U);
    }
}

TEST_CASE(delimited_framer_consume_stops_after_a_frame) {
    common::DelimitedFramer<8U> framer{kDelimiters, kIgnored};
    const common::Span<const std::uint8_t> input{bytes_of("OK\rAB")};
    CHECK(framer.consume(input) == 3U);
    CHECK(framer.has_frame());
    CHECK(framer.bytes().size() == 2U);
    CHECK(framer.bytes()[0U] == static_cast<std::uint8_t>('O'));

    // The next call discards the frame and buffers the partial one.
    CHECK(framer.consume(input.subspan(3U)) == 2U);
    CHECK(!framer.has_frame());
    CHECK(framer.pending_size() == 2U);

    // reset() drops the partial frame, e.g. after a timeout.
    framer.reset();
    CHECK(framer.pending_size() == 0U);
    CHECK(framer.consume(bytes_of("CD>")) == 3U);
    CHECK(std::string(framer.text().data(), framer.text().size()) == "CD");
    CHECK(framer.consume(common::Span<const std::uint8_t>{}) == 0U);
    CHECK(!framer.has_frame());
}

TEST_CASE(delimited_framer_truncates_and_recovers) {
    common::DelimitedFramer<4U> framer{kDelimiters, kIgnored};
    const std::vector<Frame> frames{split(framer, "ABCDEFGH\rOK\rABCD\r", 3U)};
    REQUIRE(frames.size() == 3U);
    CHECK(frames[0U].text == "ABCD");
    CHECK(frames[0U].truncated);
    CHECK(frames[1U].text == "OK");
    CHECK(!frames[1U].truncated);
    CHECK(frames[2U].text == "ABCD");  // Exactly full is not truncated.
    CHECK(!frames[2U].truncated);
    CHECK(framer.truncated_count() == 1U);
}
