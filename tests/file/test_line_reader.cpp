#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "common/containers/span.hpp"
#include "common/core/status.hpp"
#include "common/file/file.hpp"
#include "common/file/line_reader.hpp"
#include "common/file/whole_file.hpp"
#include "temp_directory.hpp"
#include "test_framework.hpp"

namespace {

/// Reads every line of @p text, which is written to a file first.
template <std::size_t BufferSize>
std::vector<std::string> read_lines(const test::TempDirectory& directory, const std::string& text,
                                    common::Status& status) {
    const std::string path{directory.entry("lines")};
    const common::Span<const std::uint8_t> bytes{reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
    std::vector<std::string> lines{};
    status = common::write_file(path.c_str(), bytes, 0x1A4U);
    common::File file{};
    if (common::is_ok(status)) {
        status = file.open(path.c_str(), common::FileOptions{});
    }
    if (common::is_ok(status)) {
        common::LineReader<BufferSize> reader{file};
        common::Span<const char> line{};
        for (std::uint32_t count{0U}; (count < 100'000U) && reader.next(line); ++count) {
            lines.emplace_back(line.data(), line.size());
            if (reader.line_number() != lines.size()) {
                lines.emplace_back("<wrong line number>");
            }
        }
        status = reader.status();
    }
    return lines;
}

}  // namespace

TEST_CASE(line_reader_line_endings) {
    test::TempDirectory directory{};
    REQUIRE(directory.valid());
    common::Status status{common::Status::kOk};

    CHECK((read_lines<64U>(directory, "a\nbb\r\nccc", status) == std::vector<std::string>{"a", "bb", "ccc"}));
    CHECK(common::is_ok(status));
    CHECK((read_lines<64U>(directory, "a\n\n\nb\n", status) == std::vector<std::string>{"a", "", "", "b"}));
    CHECK(read_lines<64U>(directory, "", status).empty());
    CHECK(common::is_ok(status));
    CHECK((read_lines<64U>(directory, "\n", status) == std::vector<std::string>{""}));
    // A "\r" that does not precede "\n" belongs to the line, and so does a NUL.
    CHECK((read_lines<64U>(directory, "a\rb\r\r\n", status) == std::vector<std::string>{"a\rb\r"}));
    CHECK((read_lines<64U>(directory, std::string{"x\0y\n", 4U}, status) ==
           std::vector<std::string>{std::string{"x\0y", 3U}}));
}

TEST_CASE(line_reader_line_length_limit) {
    test::TempDirectory directory{};
    REQUIRE(directory.valid());
    common::Status status{common::Status::kOk};
    static_assert(common::LineReader<8U>::kMaxLineLength == 7U, "BufferSize - 1");

    // Exactly kMaxLineLength bytes before the "\n" fit, a "\r" included; one more does not.
    CHECK((read_lines<8U>(directory, "1234567\n12345\r\nend", status) ==
           std::vector<std::string>{"1234567", "12345", "end"}));
    CHECK(common::is_ok(status));
    CHECK((read_lines<8U>(directory, "1234567", status) == std::vector<std::string>{"1234567"}));
    CHECK(common::is_ok(status));

    const std::vector<std::string> lines{read_lines<8U>(directory, "ok\n12345678\nnever", status)};
    CHECK((lines == std::vector<std::string>{"ok"}));
    CHECK(status == common::Status::kOutOfRange);  // Line 2 is too long.
    CHECK((read_lines<8U>(directory, "123456\r\n", status) == std::vector<std::string>{"123456"}));
    CHECK(read_lines<8U>(directory, "1234567\r\n", status).empty());
    CHECK(status == common::Status::kOutOfRange);
    CHECK(read_lines<8U>(directory, "12345678", status).empty());  // Also without a final "\n".
    CHECK(status == common::Status::kOutOfRange);
}

TEST_CASE(line_reader_many_lines_across_refills) {
    test::TempDirectory directory{};
    REQUIRE(directory.valid());
    std::vector<std::string> expected{};
    std::string text{};
    for (std::size_t i{0U}; i < 2000U; ++i) {
        std::string line((i * 37U) % 200U, static_cast<char>('a' + static_cast<char>(i % 26U)));
        line += std::to_string(i);
        text += line;
        text += ((i % 3U) == 0U) ? "\r\n" : "\n";
        expected.push_back(line);
    }
    common::Status status{common::Status::kOk};
    CHECK(read_lines<256U>(directory, text, status) == expected);
    CHECK(common::is_ok(status));
    CHECK(read_lines<4096U>(directory, text, status) == expected);
    CHECK(common::is_ok(status));
}

TEST_CASE(line_reader_from_a_pipe_in_pieces) {
    std::array<int, 2U> pipe_ends{{-1, -1}};
    REQUIRE(::pipe(pipe_ends.data()) == 0);
    common::File input{};
    REQUIRE(common::is_ok(input.adopt(pipe_ends[0U])));

    // The writer sends the text a few bytes at a time, splitting lines and "\r\n" pairs.
    const std::string text{"first line\r\nsecond\nthird line is longer\r\n\nlast"};
    std::thread writer{[&text, write_end = pipe_ends[1U]]() {
        for (std::size_t offset{0U}; offset < text.size(); offset += 3U) {
            const std::size_t count{((text.size() - offset) < 3U) ? (text.size() - offset) : 3U};
            if (::write(write_end, text.data() + offset, count) != static_cast<ssize_t>(count)) {
                break;  // The test then sees the text cut short.
            }
            std::this_thread::yield();
        }
        static_cast<void>(::close(write_end));
    }};

    common::LineReader<32U> reader{input};
    std::vector<std::string> lines{};
    common::Span<const char> line{};
    for (std::uint32_t count{0U}; (count < 100U) && reader.next(line); ++count) {
        lines.emplace_back(line.data(), line.size());
    }
    writer.join();
    CHECK((lines == std::vector<std::string>{"first line", "second", "third line is longer", "", "last"}));
    CHECK(common::is_ok(reader.status()));
    CHECK(reader.line_number() == 5U);
    CHECK(!reader.next(line));  // Stays at the end.
    CHECK(line.empty());
}

TEST_CASE(line_reader_reports_read_errors) {
    common::File closed{};
    common::LineReader<16U> reader{closed};
    common::Span<const char> line{};
    CHECK(!reader.next(line));
    CHECK(reader.status() == common::Status::kInvalidArgument);
    CHECK(reader.line_number() == 0U);
    CHECK(!reader.next(line));  // Once false, always false.
}
