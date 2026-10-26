#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "common/containers/span.hpp"
#include "common/core/status.hpp"
#include "common/file/file.hpp"
#include "common/file/whole_file.hpp"
#include "temp_directory.hpp"
#include "test_framework.hpp"
#include "test_platform.hpp"

namespace {

common::Span<const std::uint8_t> bytes_of(const std::string& text) {
    return common::Span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

/// The contents of @p path, or "<error>".
std::string contents_of(const std::string& path) {
    std::array<std::uint8_t, 4096U> buffer{};
    const common::Result<std::size_t> count{common::read_file(path.c_str(), buffer)};
    return count.has_value() ? std::string{reinterpret_cast<const char*>(buffer.data()), count.value()}
                             : std::string{"<error>"};
}

#if !defined(_WIN32)
/// Permission bits of @p path, or 0xFFFFFFFF.
std::uint32_t permissions_of(const std::string& path) {
    struct stat info {};
    return (::lstat(path.c_str(), &info) == 0) ? (std::uint32_t{info.st_mode} & 0xFFFU) : 0xFFFFFFFFU;
}

/// The permission bits a file created with @p requested gets under the current umask.
std::uint32_t after_umask(const std::uint32_t requested) {
    const mode_t mask{::umask(0)};
    static_cast<void>(::umask(mask));
    return requested & ~static_cast<std::uint32_t>(mask);
}
#endif

/// @return true if @p path has the permission bits that a file created with @p requested gets
///         under the current umask; always true on Windows, which has no permission bits.
bool has_permissions(const std::string& path, const std::uint32_t requested) {
#if defined(_WIN32)
    static_cast<void>(path);
    static_cast<void>(requested);
    return true;
#else
    return permissions_of(path) == after_umask(requested);
#endif
}

bool has_temporary_files(const test::TempDirectory& directory) {
    bool found{false};
    for (const std::string& name : directory.list()) {
        found = found || (name.find(".tmp.") != std::string::npos);
    }
    return found;
}

}  // namespace

// Runs first in this file: the temporary-name counter is process-wide and only this file uses it, so
// names 0 to 63 cover the next 8 that write_file_atomically() tries.
TEST_CASE(whole_file_atomic_write_gives_up_when_names_are_taken) {
    test::TempDirectory directory{};
    REQUIRE(directory.valid());
    const std::string path{directory.entry("state")};
    const std::string pid{std::to_string(test::process_id())};
    for (std::uint32_t n{0U}; n < 64U; ++n) {
        REQUIRE(common::is_ok(common::write_file((path + ".tmp." + pid + "." + std::to_string(n)).c_str(),
                                                 bytes_of("taken"), 0x1A4U)));
    }
    CHECK(common::write_file_atomically(path.c_str(), bytes_of("new"), 0x1A4U) == common::Status::kAlreadyExists);
    CHECK(contents_of(path) == "<error>");  // path was not created.
}

TEST_CASE(whole_file_read_file) {
    test::TempDirectory directory{};
    REQUIRE(directory.valid());
    const std::string path{directory.entry("data")};
    REQUIRE(common::is_ok(common::write_file(path.c_str(), bytes_of("calibration"), 0x1A4U)));

    std::array<std::uint8_t, 11U> exact{};
    CHECK(common::read_file(path.c_str(), exact).value_or(0U) == 11U);
    CHECK(std::memcmp(exact.data(), "calibration", 11U) == 0);
    std::array<std::uint8_t, 10U> small{};
    CHECK(common::read_file(path.c_str(), small).status() == common::Status::kOutOfRange);
    CHECK(common::read_file(path.c_str(), common::Span<std::uint8_t>{}).status() == common::Status::kOutOfRange);

    const std::string empty_path{directory.entry("empty")};
    REQUIRE(common::is_ok(common::write_file(empty_path.c_str(), common::Span<const std::uint8_t>{}, 0x1A4U)));
    CHECK(common::read_file(empty_path.c_str(), small).value_or(99U) == 0U);
    CHECK(common::read_file(empty_path.c_str(), common::Span<std::uint8_t>{}).value_or(99U) == 0U);

    CHECK(common::read_file(directory.entry("missing").c_str(), small).status() == common::Status::kNotFound);
    CHECK(common::read_file(nullptr, small).status() == common::Status::kInvalidArgument);
    CHECK(common::read_file(directory.path().c_str(), small).status() == common::Status::kInvalidArgument);

#if defined(__linux__)
    // /proc reports a size of 0 but has contents: read_file() reads to the end of file.
    std::array<std::uint8_t, 4096U> proc{};
    const common::Result<std::size_t> stat_count{common::read_file("/proc/self/stat", proc)};
    CHECK(stat_count.value_or(0U) > 0U);
#endif
}

TEST_CASE(whole_file_read_text_file) {
    test::TempDirectory directory{};
    REQUIRE(directory.valid());
    const std::string path{directory.entry("config")};
    REQUIRE(common::is_ok(common::write_file(path.c_str(), bytes_of("rate=10\n"), 0x1A4U)));

    std::array<char, 9U> exact{};  // 8 characters and the terminator.
    exact.fill('#');
    CHECK(common::read_text_file(path.c_str(), exact).value_or(0U) == 8U);
    CHECK(std::strcmp(exact.data(), "rate=10\n") == 0);
    std::array<char, 8U> no_room_for_nul{};
    CHECK(common::read_text_file(path.c_str(), no_room_for_nul).status() == common::Status::kOutOfRange);
    CHECK(common::read_text_file(path.c_str(), common::Span<char>{}).status() == common::Status::kOutOfRange);
    CHECK(common::read_text_file(nullptr, exact).status() == common::Status::kInvalidArgument);
    CHECK(common::read_text_file(nullptr, common::Span<char>{}).status() == common::Status::kInvalidArgument);

    // A NUL would silently cut the text.
    const std::string binary_path{directory.entry("binary")};
    const std::array<std::uint8_t, 3U> with_nul{{'a', 0U, 'b'}};
    REQUIRE(common::is_ok(common::write_file(binary_path.c_str(), with_nul, 0x1A4U)));
    std::array<char, 16U> buffer{};
    CHECK(common::read_text_file(binary_path.c_str(), buffer).status() == common::Status::kInvalidArgument);

    const std::string empty_path{directory.entry("empty")};
    REQUIRE(common::is_ok(common::write_file(empty_path.c_str(), common::Span<const std::uint8_t>{}, 0x1A4U)));
    std::array<char, 1U> just_nul{{'#'}};
    CHECK(common::read_text_file(empty_path.c_str(), just_nul).value_or(99U) == 0U);
    CHECK(just_nul[0U] == '\0');
}

TEST_CASE(whole_file_write_file) {
    test::TempDirectory directory{};
    REQUIRE(directory.valid());
    const std::string path{directory.entry("out")};
    CHECK(common::is_ok(common::write_file(path.c_str(), bytes_of("first version"), 0x180U)));
    CHECK(contents_of(path) == "first version");
    CHECK(has_permissions(path, 0x180U));  // Octal 0600.

    // An existing file is truncated and keeps its permission bits.
    CHECK(common::is_ok(common::write_file(path.c_str(), bytes_of("v2"), 0x1FFU)));
    CHECK(contents_of(path) == "v2");
    CHECK(has_permissions(path, 0x180U));

    CHECK(common::write_file(nullptr, bytes_of("x"), 0x1A4U) == common::Status::kInvalidArgument);
    CHECK(common::write_file(path.c_str(), bytes_of("x"), 0x1000U) == common::Status::kInvalidArgument);
    CHECK(common::write_file(directory.entry("missing/out").c_str(), bytes_of("x"), 0x1A4U) ==
          common::Status::kNotFound);
    CHECK(contents_of(path) == "v2");
}

TEST_CASE(whole_file_write_file_atomically) {
    test::TempDirectory directory{};
    REQUIRE(directory.valid());
    const std::string path{directory.entry("sequence")};
    CHECK(common::is_ok(common::write_file_atomically(path.c_str(), bytes_of("1000"), 0x180U)));
    CHECK(contents_of(path) == "1000");
    CHECK(has_permissions(path, 0x180U));
    CHECK(common::is_ok(common::write_file_atomically(path.c_str(), bytes_of("1001"), 0x1A4U)));
    CHECK(contents_of(path) == "1001");
    CHECK(has_permissions(path, 0x1A4U));  // The new file's bits, not the old file's.
    CHECK(common::is_ok(common::write_file_atomically(path.c_str(), common::Span<const std::uint8_t>{}, 0x1A4U)));
    CHECK(contents_of(path).empty());
    CHECK(!has_temporary_files(directory));

#if !defined(_WIN32)  // Symbolic links need a privilege or developer mode on Windows.
    // A symbolic link at the path is replaced, not followed.
    const std::string target{directory.entry("target")};
    const std::string link{directory.entry("link")};
    REQUIRE(common::is_ok(common::write_file(target.c_str(), bytes_of("target"), 0x1A4U)));
    REQUIRE(::symlink(target.c_str(), link.c_str()) == 0);
    CHECK(common::is_ok(common::write_file_atomically(link.c_str(), bytes_of("replaced"), 0x1A4U)));
    CHECK(contents_of(target) == "target");
    CHECK(contents_of(link) == "replaced");
    struct stat info {};
    CHECK((::lstat(link.c_str(), &info) == 0) && S_ISREG(info.st_mode));
#endif

    // A path in the root directory, and a bare file name (the current directory), sync the right
    // directory. The root is not writable here, so only the relative case can be completed.
    const std::string previous{test::current_directory()};
    REQUIRE(!previous.empty());
    REQUIRE(test::change_directory(directory.path()));
    const common::Status relative{common::write_file_atomically("relative", bytes_of("here"), 0x1A4U)};
    REQUIRE(test::change_directory(previous));
    CHECK(common::is_ok(relative));
    CHECK(contents_of(directory.entry("relative")) == "here");
#if defined(_WIN32)
    // A path with backslashes, as Windows programs write them.
    std::string backslashed{directory.entry("windows")};
    for (char& character : backslashed) {
        character = (character == '/') ? '\\' : character;
    }
    CHECK(common::is_ok(common::write_file_atomically(backslashed.c_str(), bytes_of("native"), 0x1A4U)));
    CHECK(contents_of(directory.entry("windows")) == "native");
    CHECK(common::write_file_atomically((backslashed + "\\").c_str(), bytes_of("x"), 0x1A4U) ==
          common::Status::kInvalidArgument);
#endif
    CHECK(!has_temporary_files(directory));
}

TEST_CASE(whole_file_write_file_atomically_errors) {
    test::TempDirectory directory{};
    REQUIRE(directory.valid());
    CHECK(common::write_file_atomically(nullptr, bytes_of("x"), 0x1A4U) == common::Status::kInvalidArgument);
    CHECK(common::write_file_atomically("", bytes_of("x"), 0x1A4U) == common::Status::kInvalidArgument);
    CHECK(common::write_file_atomically((directory.path() + "/").c_str(), bytes_of("x"), 0x1A4U) ==
          common::Status::kInvalidArgument);
    CHECK(common::write_file_atomically(directory.entry("x").c_str(), bytes_of("x"), 0x1000U) ==
          common::Status::kInvalidArgument);
    CHECK(common::write_file_atomically(directory.entry("missing/x").c_str(), bytes_of("x"), 0x1A4U) ==
          common::Status::kNotFound);

    // The longest accepted path is 4069 bytes.
    const std::string too_long(4070U, 'a');
    CHECK(common::write_file_atomically(too_long.c_str(), bytes_of("x"), 0x1A4U) == common::Status::kInvalidArgument);
    const std::string longest{"/" + std::string(4068U, 'a')};  // Accepted, then rejected by the kernel.
    CHECK(common::write_file_atomically(longest.c_str(), bytes_of("x"), 0x1A4U) != common::Status::kOk);

    // When the rename fails (the path is a non-empty directory), the temporary file is removed and
    // the path is left as it was.
    const std::string occupied{directory.entry("occupied")};
    REQUIRE(common::is_ok(common::create_directory(occupied.c_str(), 0x1C0U)));  // Octal 0700.
    REQUIRE(common::is_ok(common::write_file((occupied + "/inside").c_str(), bytes_of("keep"), 0x1A4U)));
    CHECK(!common::is_ok(common::write_file_atomically(occupied.c_str(), bytes_of("x"), 0x1A4U)));
    CHECK(contents_of(occupied + "/inside") == "keep");
    CHECK(!has_temporary_files(directory));
}

TEST_CASE(whole_file_atomic_replacement_is_never_torn) {
    test::TempDirectory directory{};
    REQUIRE(directory.valid());
    const std::string path{directory.entry("shared")};
    // Each version is a whole line of one repeated letter, so a mix of two versions shows.
    std::vector<std::string> versions{};
    for (char letter{'a'}; letter <= 'h'; ++letter) {
        versions.emplace_back(2000U, letter);
    }
    REQUIRE(common::is_ok(common::write_file_atomically(path.c_str(), bytes_of(versions[0U]), 0x1A4U)));

    std::atomic<bool> writing{true};
    std::atomic<std::uint32_t> failures{0U};
    std::vector<std::thread> writers{};
    for (std::size_t writer{0U}; writer < 4U; ++writer) {
        writers.emplace_back([&path, &versions, &failures, writer]() {
            for (std::size_t round{0U}; round < 8U; ++round) {
                const std::string& version{versions[(writer + round) % versions.size()]};
                if (!common::is_ok(common::write_file_atomically(path.c_str(), bytes_of(version), 0x1A4U))) {
                    failures.fetch_add(1U);
                }
            }
        });
    }
    std::uint32_t torn{0U};
    std::uint32_t reads{0U};
    std::thread reader{[&]() {
        while (writing.load()) {
            const std::string seen{contents_of(path)};
            ++reads;
            const bool whole{(seen.size() == 2000U) && (seen.find_first_not_of(seen[0U]) == std::string::npos)};
            torn += whole ? 0U : 1U;
        }
    }};
    for (std::thread& writer : writers) {
        writer.join();
    }
    writing.store(false);
    reader.join();
    CHECK(failures.load() == 0U);
    CHECK(torn == 0U);
    CHECK(reads > 0U);
    CHECK(!has_temporary_files(directory));
}
