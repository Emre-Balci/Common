#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include "common/containers/span.hpp"
#include "common/core/status.hpp"
#include "common/file/file.hpp"
#include "common/file/mapped_file.hpp"
#include "common/file/whole_file.hpp"
#include "temp_directory.hpp"
#include "test_framework.hpp"

namespace {

/// 10000 bytes: byte i holds i % 251, so every offset is recognisable.
std::string pattern_contents() {
    std::string text(10000U, '\0');
    for (std::size_t i{0U}; i < text.size(); ++i) {
        text[i] = static_cast<char>(i % 251U);
    }
    return text;
}

common::Span<const std::uint8_t> bytes_of(const std::string& text) {
    return common::Span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

std::string text_of(const common::Span<const std::uint8_t> bytes) {
    return std::string{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::string contents_of(const std::string& path) {
    std::array<std::uint8_t, 16384U> buffer{};
    const common::Result<std::size_t> count{common::read_file(path.c_str(), buffer)};
    return count.has_value() ? text_of(common::Span<const std::uint8_t>{buffer.data(), count.value()})
                             : std::string{"<error>"};
}

common::MapOptions options(const common::MapMode mode, const std::uint64_t offset, const std::uint64_t length) {
    common::MapOptions result{};
    result.mode = mode;
    result.offset = offset;
    result.length = length;
    return result;
}

}  // namespace

TEST_CASE(mapped_file_read_only_ranges) {
    test::TempDirectory directory{};
    REQUIRE(directory.valid());
    const std::string path{directory.entry("reference")};
    const std::string contents{pattern_contents()};
    REQUIRE(common::is_ok(common::write_file(path.c_str(), bytes_of(contents), 0x1A4U)));

    common::MappedFile whole{};
    CHECK(!whole.is_mapped());
    CHECK(whole.bytes().empty());
    REQUIRE(common::is_ok(whole.map(path.c_str(), common::MapOptions{})));
    CHECK(whole.is_mapped());
    CHECK(whole.mode() == common::MapMode::kReadOnly);
    CHECK(whole.size() == contents.size());
    CHECK(text_of(whole.bytes()) == contents);
    CHECK(whole.writable_bytes().empty());  // Read-only.
    CHECK(whole.map(path.c_str(), common::MapOptions{}) == common::Status::kInvalidArgument);  // Already mapped.

    // An offset that is not a multiple of the page size: bytes() still starts exactly there.
    common::MappedFile part{};
    REQUIRE(common::is_ok(part.map(path.c_str(), options(common::MapMode::kReadOnly, 5000U, 100U))));
    CHECK(text_of(part.bytes()) == contents.substr(5000U, 100U));
    common::MappedFile tail{};
    REQUIRE(common::is_ok(tail.map(path.c_str(), options(common::MapMode::kReadOnly, 9999U, 0U))));
    CHECK(text_of(tail.bytes()) == contents.substr(9999U));

    // The range must lie inside the file.
    common::MappedFile outside{};
    CHECK(outside.map(path.c_str(), options(common::MapMode::kReadOnly, 10001U, 0U)) == common::Status::kOutOfRange);
    CHECK(outside.map(path.c_str(), options(common::MapMode::kReadOnly, 9000U, 1001U)) ==
          common::Status::kOutOfRange);
    CHECK(outside.map(path.c_str(), options(common::MapMode::kReadOnly, 1U, UINT64_MAX)) ==
          common::Status::kOutOfRange);
    CHECK(!outside.is_mapped());
    // The end of the file is a valid, empty range.
    CHECK(common::is_ok(outside.map(path.c_str(), options(common::MapMode::kReadOnly, 10000U, 0U))));
    CHECK(outside.is_mapped());
    CHECK(outside.size() == 0U);

    CHECK(common::is_ok(whole.unmap()));
    CHECK(!whole.is_mapped());
    CHECK(whole.bytes().empty());
    CHECK(common::is_ok(whole.unmap()));  // Idempotent.
}

TEST_CASE(mapped_file_errors) {
    test::TempDirectory directory{};
    REQUIRE(directory.valid());
    const std::string path{directory.entry("file")};
    REQUIRE(common::is_ok(common::write_file(path.c_str(), bytes_of("contents"), 0x1A4U)));

    common::MappedFile mapping{};
    CHECK(mapping.map(nullptr, common::MapOptions{}) == common::Status::kInvalidArgument);
    CHECK(mapping.map(directory.entry("missing").c_str(), common::MapOptions{}) == common::Status::kNotFound);
    CHECK(mapping.map(directory.path().c_str(), common::MapOptions{}) == common::Status::kInvalidArgument);
    CHECK(mapping.map(path.c_str(), options(static_cast<common::MapMode>(7U), 0U, 0U)) ==
          common::Status::kInvalidArgument);

    common::File closed{};
    CHECK(mapping.map(closed, common::MapOptions{}) == common::Status::kInvalidArgument);

    // The descriptor's access mode must allow the mapping.
    common::File read_only{};
    REQUIRE(common::is_ok(read_only.open(path.c_str(), common::FileOptions{})));
    CHECK(mapping.map(read_only, options(common::MapMode::kReadWrite, 0U, 0U)) == common::Status::kPermissionDenied);
    common::FileOptions write_access{};
    write_access.access = common::FileAccess::kWriteOnly;
    common::File write_only{};
    REQUIRE(common::is_ok(write_only.open(path.c_str(), write_access)));
    CHECK(mapping.map(write_only, common::MapOptions{}) == common::Status::kPermissionDenied);
    CHECK(!mapping.is_mapped());

    // Operations other than unmap() need a mapping.
    CHECK(mapping.sync() == common::Status::kInvalidArgument);
    CHECK(mapping.lock() == common::Status::kInvalidArgument);
    CHECK(mapping.unlock() == common::Status::kInvalidArgument);
    CHECK(mapping.advise(common::MappedFile::Advice::kRandom) == common::Status::kInvalidArgument);
    CHECK(common::is_ok(mapping.unmap()));

    // A copy-on-write mapping needs only read access.
    CHECK(common::is_ok(mapping.map(read_only, options(common::MapMode::kCopyOnWrite, 0U, 0U))));
}

TEST_CASE(mapped_file_empty_file) {
    test::TempDirectory directory{};
    REQUIRE(directory.valid());
    const std::string path{directory.entry("empty")};
    REQUIRE(common::is_ok(common::write_file(path.c_str(), common::Span<const std::uint8_t>{}, 0x1A4U)));

    common::MappedFile mapping{};
    CHECK(common::is_ok(mapping.map(path.c_str(), common::MapOptions{})));
    CHECK(mapping.is_mapped());
    CHECK(mapping.bytes().empty());
    CHECK(common::is_ok(mapping.sync()));
    CHECK(common::is_ok(mapping.lock()));
    CHECK(common::is_ok(mapping.unlock()));
    CHECK(common::is_ok(mapping.advise(common::MappedFile::Advice::kWillNeed)));
    CHECK(common::is_ok(mapping.unmap()));
    CHECK(!mapping.is_mapped());
}

TEST_CASE(mapped_file_write_modes) {
    test::TempDirectory directory{};
    REQUIRE(directory.valid());
    const std::string path{directory.entry("state")};
    REQUIRE(common::is_ok(common::write_file(path.c_str(), bytes_of("0123456789"), 0x1A4U)));

    // Copy-on-write: the mapping changes, the file does not.
    common::MappedFile private_copy{};
    REQUIRE(common::is_ok(private_copy.map(path.c_str(), options(common::MapMode::kCopyOnWrite, 0U, 0U))));
    REQUIRE(private_copy.writable_bytes().size() == 10U);
    private_copy.writable_bytes()[0U] = static_cast<std::uint8_t>('X');
    CHECK(text_of(private_copy.bytes()) == "X123456789");
    CHECK(common::is_ok(private_copy.sync()));  // Nothing to write back.
    CHECK(contents_of(path) == "0123456789");

    // Read-write: changes reach the file.
    common::MappedFile shared{};
    REQUIRE(common::is_ok(shared.map(path.c_str(), options(common::MapMode::kReadWrite, 2U, 3U))));
    CHECK(shared.mode() == common::MapMode::kReadWrite);
    REQUIRE(shared.writable_bytes().size() == 3U);
    static_cast<void>(std::memcpy(shared.writable_bytes().data(), "abc", 3U));
    CHECK(common::is_ok(shared.sync()));
    CHECK(contents_of(path) == "01abc56789");
    CHECK(common::is_ok(shared.unmap()));
}

TEST_CASE(mapped_file_create_state_block) {
    test::TempDirectory directory{};
    REQUIRE(directory.valid());
    const std::string path{directory.entry("block")};

    common::MappedFile block{};
    CHECK(block.create(nullptr, 64U, 0x1A4U) == common::Status::kInvalidArgument);
    CHECK(block.create(path.c_str(), 64U, 0x1000U) == common::Status::kInvalidArgument);
    REQUIRE(common::is_ok(block.create(path.c_str(), 64U, 0x180U)));
    CHECK(block.mode() == common::MapMode::kReadWrite);
    REQUIRE(block.size() == 64U);
    bool zeros{true};
    for (const std::uint8_t byte : block.bytes()) {
        zeros = zeros && (byte == 0U);
    }
    CHECK(zeros);
    CHECK(block.create(path.c_str(), 64U, 0x1A4U) == common::Status::kInvalidArgument);  // Already mapped.
    static_cast<void>(std::memcpy(block.writable_bytes().data(), "persisted", 9U));
    CHECK(common::is_ok(block.sync()));
    CHECK(common::is_ok(block.unmap()));
    CHECK(contents_of(path).substr(0U, 9U) == "persisted");
    CHECK(contents_of(path).size() == 64U);

    // An existing file is resized to exactly the requested size and keeps its leading contents.
    REQUIRE(common::is_ok(block.create(path.c_str(), 16U, 0x1A4U)));
    CHECK(block.size() == 16U);
    CHECK(text_of(block.bytes().first(9U)) == "persisted");
    CHECK(common::is_ok(block.unmap()));
    struct stat info {};
    REQUIRE(::stat(path.c_str(), &info) == 0);
    CHECK(info.st_size == 16);

    REQUIRE(common::is_ok(block.create(path.c_str(), 0U, 0x1A4U)));  // An empty block.
    CHECK(block.is_mapped());
    CHECK(block.size() == 0U);
}

TEST_CASE(mapped_file_lock_and_advise) {
    test::TempDirectory directory{};
    REQUIRE(directory.valid());
    const std::string path{directory.entry("table")};
    REQUIRE(common::is_ok(common::write_file(path.c_str(), bytes_of(pattern_contents()), 0x1A4U)));

    common::MappedFile mapping{};
    REQUIRE(common::is_ok(mapping.map(path.c_str(), common::MapOptions{})));
    // Locking 3 pages fits the default RLIMIT_MEMLOCK; a tighter limit is reported, never ignored.
    const common::Status locked{mapping.lock()};
    CHECK(common::is_ok(locked) || (locked == common::Status::kResourceLimit) ||
          (locked == common::Status::kPermissionDenied));
    CHECK(common::is_ok(mapping.unlock()));
    CHECK(common::is_ok(mapping.advise(common::MappedFile::Advice::kNormal)));
    CHECK(common::is_ok(mapping.advise(common::MappedFile::Advice::kSequential)));
    CHECK(common::is_ok(mapping.advise(common::MappedFile::Advice::kRandom)));
    CHECK(common::is_ok(mapping.advise(common::MappedFile::Advice::kWillNeed)));
    CHECK(mapping.advise(static_cast<common::MappedFile::Advice>(9U)) == common::Status::kInvalidArgument);

    // MapOptions::lock: the same outcome as lock(), and on failure nothing stays mapped.
    common::MapOptions lock_options{};
    lock_options.lock = true;
    common::MappedFile locked_mapping{};
    const common::Status mapped{locked_mapping.map(path.c_str(), lock_options)};
    CHECK(mapped == locked);
    CHECK(locked_mapping.is_mapped() == common::is_ok(mapped));
}

TEST_CASE(mapped_file_move_and_lifetime) {
    test::TempDirectory directory{};
    REQUIRE(directory.valid());
    const std::string path{directory.entry("data")};
    REQUIRE(common::is_ok(common::write_file(path.c_str(), bytes_of("lifetime"), 0x1A4U)));

    common::File file{};
    REQUIRE(common::is_ok(file.open(path.c_str(), common::FileOptions{})));
    common::MappedFile first{};
    REQUIRE(common::is_ok(first.map(file, common::MapOptions{})));
    // The mapping needs neither the descriptor nor the name.
    CHECK(common::is_ok(file.close()));
    CHECK(common::is_ok(common::remove_file(path.c_str())));
    const common::Span<const std::uint8_t> view{first.bytes()};
    CHECK(text_of(view) == "lifetime");

    // A move keeps the address, so spans obtained earlier stay valid.
    common::MappedFile second{std::move(first)};
    CHECK(!first.is_mapped());  // NOLINT(bugprone-use-after-move,hicpp-invalid-access-moved): moved-from state.
    CHECK(second.is_mapped());
    CHECK(second.bytes().data() == view.data());
    CHECK(text_of(view) == "lifetime");

    common::MappedFile third{};
    const std::string other_path{directory.entry("other")};
    REQUIRE(common::is_ok(common::write_file(other_path.c_str(), bytes_of("other"), 0x1A4U)));
    REQUIRE(common::is_ok(third.map(other_path.c_str(), common::MapOptions{})));
    third = std::move(second);  // Unmaps "other" and takes over "lifetime".
    CHECK(!second.is_mapped());  // NOLINT(bugprone-use-after-move,hicpp-invalid-access-moved): moved-from state.
    CHECK(text_of(third.bytes()) == "lifetime");
    CHECK(third.bytes().data() == view.data());
}
