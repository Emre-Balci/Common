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
    constexpr common::ByteSet kSet{common::ByteSet{}.with('a').with(std::uint8_t{0xFFU}).with('\0')};
    static_assert(kSet.contains('a'), "constexpr");
    CHECK(kSet.contains(std::uint8_t{0xFFU}));
    CHECK(kSet.contains('\0'));
    CHECK(!kSet.contains('b'));
    CHECK(!kSet.empty());
    CHECK(common::ByteSet{}.empty());
    CHECK(!kSet.disjoint(common::ByteSet{}.with('a')));
    CHECK(kSet.disjoint(common::ByteSet{}.with('b')));
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
