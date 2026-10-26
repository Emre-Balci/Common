#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include "common/containers/span.hpp"
#include "common/core/config.hpp"
#include "common/core/status.hpp"
#include "common/file/file.hpp"
#include "temp_directory.hpp"
#include "test_framework.hpp"
#include "test_platform.hpp"

namespace {

using test::TempDirectory;

common::Span<const std::uint8_t> bytes_of(const char* const text) {
    return common::Span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(text), std::strlen(text)};
}

common::FileOptions options(const common::FileAccess access, const common::FileCreation creation) {
    common::FileOptions result{};
    result.access = access;
    result.creation = creation;
    return result;
}

std::string read_all(const std::string& path) {
    common::File file{};
    std::string text{};
    if (common::is_ok(file.open(path.c_str(), common::FileOptions{}))) {
        std::array<std::uint8_t, 256U> buffer{};
        const common::Result<std::size_t> count{file.read(common::Span<std::uint8_t>{buffer})};
        if (count.has_value()) {
            text.assign(reinterpret_cast<const char*>(buffer.data()), count.value());
        }
    }
    return text;
}

}  // namespace

TEST_CASE(file_open_modes) {
    TempDirectory directory{};
    REQUIRE(directory.valid());
    const std::string path{directory.entry("data")};

    common::File file{};
    CHECK(file.open(path.c_str(), common::FileOptions{}) == common::Status::kNotFound);
    CHECK(!file.is_open());
    CHECK(file.descriptor() == common::kInvalidNativeHandle);
    CHECK(file.open(nullptr, common::FileOptions{}) == common::Status::kInvalidArgument);

    const common::FileOptions create_new{options(common::FileAccess::kWriteOnly, common::FileCreation::kCreateNew)};
    REQUIRE(common::is_ok(file.open(path.c_str(), create_new)));
    CHECK(file.is_open());
    CHECK(file.open(path.c_str(), create_new) == common::Status::kInvalidArgument);  // Already open.

    common::File second{};
    CHECK(second.open(path.c_str(), create_new) == common::Status::kAlreadyExists);

    // Contradictory options are rejected before any system call.
    CHECK(second.open(path.c_str(), options(common::FileAccess::kReadOnly, common::FileCreation::kCreateOrTruncate)) ==
          common::Status::kInvalidArgument);
    common::FileOptions append_read_only{};
    append_read_only.append = true;
    CHECK(second.open(path.c_str(), append_read_only) == common::Status::kInvalidArgument);
    common::FileOptions bad_permissions{create_new};
    bad_permissions.permissions = 0x1000U;
    CHECK(second.open(directory.entry("other").c_str(), bad_permissions) == common::Status::kInvalidArgument);
    CHECK(!second.is_open());
    CHECK(common::is_ok(file.close()));
}

TEST_CASE(file_read_write_round_trip) {
    TempDirectory directory{};
    REQUIRE(directory.valid());
    const std::string path{directory.entry("data")};

    common::File file{};
    REQUIRE(common::is_ok(
        file.open(path.c_str(), options(common::FileAccess::kReadWrite, common::FileCreation::kCreateOrTruncate))));
    CHECK(common::is_ok(file.write_all(bytes_of("hello, file"))));
    CHECK(file.size().value_or(0U) == 11U);

    CHECK(file.seek(0, common::SeekOrigin::kBegin).value_or(99U) == 0U);
    std::array<std::uint8_t, 5U> five{};
    CHECK(common::is_ok(file.read_exact(common::Span<std::uint8_t>{five})));
    CHECK(std::memcmp(five.data(), "hello", 5U) == 0);
    CHECK(file.seek(2, common::SeekOrigin::kCurrent).value_or(0U) == 7U);
    CHECK(file.seek(-1, common::SeekOrigin::kEnd).value_or(0U) == 10U);

    // End of file: read() returns 0, read_exact() reports kOutOfRange.
    CHECK(file.seek(0, common::SeekOrigin::kEnd).value_or(0U) == 11U);
    std::array<std::uint8_t, 16U> big{};
    CHECK(file.read(common::Span<std::uint8_t>{big}).value_or(99U) == 0U);
    CHECK(file.seek(0, common::SeekOrigin::kBegin).has_value());
    CHECK(file.read_exact(common::Span<std::uint8_t>{big}) == common::Status::kOutOfRange);
    CHECK(common::is_ok(file.read_exact(common::Span<std::uint8_t>{})));  // Nothing to read.

    // Positional I/O leaves the file position alone.
    CHECK(file.seek(0, common::SeekOrigin::kBegin).has_value());
    CHECK(common::is_ok(file.write_all_at(bytes_of("HELLO"), 0U)));
    std::array<std::uint8_t, 4U> four{};
    CHECK(common::is_ok(file.read_exact_at(common::Span<std::uint8_t>{four}, 7U)));
    CHECK(std::memcmp(four.data(), "file", 4U) == 0);
    CHECK(file.read_at(common::Span<std::uint8_t>{four}, 11U).value_or(99U) == 0U);
    CHECK(file.read_exact_at(common::Span<std::uint8_t>{four}, 9U) == common::Status::kOutOfRange);
    CHECK(file.read_at(common::Span<std::uint8_t>{four}, ~std::uint64_t{0U}).status() == common::Status::kOutOfRange);
    CHECK(file.seek(0, common::SeekOrigin::kCurrent).value_or(99U) == 0U);

    CHECK(common::is_ok(file.truncate(5U)));
    CHECK(file.size().value_or(0U) == 5U);
    CHECK(common::is_ok(file.sync()));
    CHECK(common::is_ok(file.sync_data()));
    CHECK(common::is_ok(file.close()));
    CHECK(read_all(path) == "HELLO");
}

TEST_CASE(file_append_and_preallocate) {
    TempDirectory directory{};
    REQUIRE(directory.valid());
    const std::string path{directory.entry("log")};

    common::FileOptions append{options(common::FileAccess::kReadWrite, common::FileCreation::kCreateOrTruncate)};
    append.append = true;
    common::File file{};
    REQUIRE(common::is_ok(file.open(path.c_str(), append)));
    CHECK(common::is_ok(file.write_all(bytes_of("ab"))));
    CHECK(file.seek(0, common::SeekOrigin::kBegin).has_value());
    CHECK(common::is_ok(file.write_all(bytes_of("cd"))));  // Still appended.
    CHECK(common::is_ok(file.close()));
    CHECK(read_all(path) == "abcd");

    const common::FileOptions existing{options(common::FileAccess::kReadWrite, common::FileCreation::kOpenExisting)};
    REQUIRE(common::is_ok(file.open(path.c_str(), existing)));
    const common::Status reserved{file.preallocate(4096U)};
    CHECK((reserved == common::Status::kOk) || (reserved == common::Status::kNotSupported));
    if (common::is_ok(reserved)) {
        CHECK(file.size().value_or(0U) == 4096U);
    }
    CHECK(common::is_ok(file.preallocate(0U)));
}

TEST_CASE(file_ownership_transfer) {
    std::array<common::NativeHandle, 2U> pipe_ends{common::kInvalidNativeHandle, common::kInvalidNativeHandle};
    REQUIRE(test::create_pipe(pipe_ends[0U], pipe_ends[1U]));

    common::File reader{};
    common::File writer{};
    CHECK(reader.adopt(common::kInvalidNativeHandle) == common::Status::kInvalidArgument);
    REQUIRE(common::is_ok(reader.adopt(pipe_ends[0U])));
    REQUIRE(common::is_ok(writer.adopt(pipe_ends[1U])));
    CHECK(writer.adopt(pipe_ends[1U]) == common::Status::kInvalidArgument);  // Already open.

    common::File moved{std::move(writer)};
    CHECK(!writer.is_open());  // NOLINT: deliberately checks the moved-from state.
    CHECK(moved.descriptor() == pipe_ends[1U]);
    CHECK(common::is_ok(moved.write_all(bytes_of("ping"))));

    common::File assigned{};
    assigned = std::move(reader);
    std::array<std::uint8_t, 4U> received{};
    CHECK(common::is_ok(assigned.read_exact(common::Span<std::uint8_t>{received})));
    CHECK(std::memcmp(received.data(), "ping", 4U) == 0);

    const common::NativeHandle raw{assigned.release()};
    CHECK(raw == pipe_ends[0U]);
    CHECK(!assigned.is_open());
    CHECK(assigned.release() == common::kInvalidNativeHandle);
    CHECK(test::close_native(raw));
}

TEST_CASE(file_closed_handle) {
    common::File file{};
    std::array<std::uint8_t, 4U> buffer{};
    CHECK(file.read(common::Span<std::uint8_t>{buffer}).status() == common::Status::kInvalidArgument);
    CHECK(file.write(bytes_of("x")).status() == common::Status::kInvalidArgument);
    CHECK(file.read_exact(common::Span<std::uint8_t>{buffer}) == common::Status::kInvalidArgument);
    CHECK(file.write_all(bytes_of("x")) == common::Status::kInvalidArgument);
    CHECK(file.seek(0, common::SeekOrigin::kBegin).status() == common::Status::kInvalidArgument);
    CHECK(file.size().status() == common::Status::kInvalidArgument);
    CHECK(file.truncate(0U) == common::Status::kInvalidArgument);
    CHECK(file.sync() == common::Status::kInvalidArgument);
    CHECK(common::is_ok(file.close()));
    CHECK(common::is_ok(file.close()));
}

TEST_CASE(file_system_operations) {
    TempDirectory directory{};
    REQUIRE(directory.valid());
    const std::string first{directory.entry("first")};
    const std::string second{directory.entry("second")};
    const std::string folder{directory.entry("folder")};

    CHECK(!common::file_exists(first.c_str()).value_or(true));
    CHECK(common::file_size(first.c_str()).status() == common::Status::kNotFound);
    CHECK(common::remove_file(first.c_str()) == common::Status::kNotFound);

    common::FileOptions create{options(common::FileAccess::kWriteOnly, common::FileCreation::kCreateNew)};
    create.permissions = 0600U;
    common::File file{};
    REQUIRE(common::is_ok(file.open(first.c_str(), create)));
    CHECK(common::is_ok(file.write_all(bytes_of("new"))));
    CHECK(common::is_ok(file.close()));
    REQUIRE(common::is_ok(file.open(second.c_str(), create)));
    CHECK(common::is_ok(file.write_all(bytes_of("old contents"))));
    CHECK(common::is_ok(file.close()));

#if !defined(_WIN32)  // Windows has no permission bits.
    struct stat info {};
    REQUIRE(::stat(first.c_str(), &info) == 0);
    CHECK((info.st_mode & 0777U) == 0600U);
#endif
    CHECK(common::file_exists(first.c_str()).value_or(false));
    CHECK(common::file_size(first.c_str()).value_or(0U) == 3U);

    // rename_file atomically replaces an existing target.
    CHECK(common::is_ok(common::rename_file(first.c_str(), second.c_str())));
    CHECK(!common::file_exists(first.c_str()).value_or(true));
    CHECK(read_all(second) == "new");
    CHECK(common::rename_file(first.c_str(), second.c_str()) == common::Status::kNotFound);

    CHECK(common::is_ok(common::create_directory(folder.c_str(), 0755U)));
    CHECK(common::create_directory(folder.c_str(), 0755U) == common::Status::kAlreadyExists);
    CHECK(common::file_exists(folder.c_str()).value_or(false));
    CHECK(common::file_size(folder.c_str()).status() == common::Status::kInvalidArgument);
    CHECK(common::is_ok(common::sync_directory(folder.c_str())));
    CHECK(common::sync_directory(second.c_str()) == common::Status::kInvalidArgument);  // Not a directory.
    CHECK(!common::file_exists((second + "/below").c_str()).value_or(true));

    CHECK(common::is_ok(common::remove_file(second.c_str())));
    CHECK(common::remove_file(nullptr) == common::Status::kInvalidArgument);
    CHECK(common::file_exists(nullptr).status() == common::Status::kInvalidArgument);
}

// As with unlink(2), the name goes at once while the file is open (on Windows where remove_file has
// POSIX semantics, as on NTFS), and the open File keeps the old file.
TEST_CASE(file_remove_while_open) {
    TempDirectory directory{};
    REQUIRE(directory.valid());
    const std::string path{directory.entry("open")};
    const common::FileOptions create{options(common::FileAccess::kReadWrite, common::FileCreation::kCreateNew)};
    common::File old_file{};
    REQUIRE(common::is_ok(old_file.open(path.c_str(), create)));
    CHECK(common::is_ok(old_file.write_all(bytes_of("old contents"))));

    CHECK(common::is_ok(common::remove_file(path.c_str())));
    CHECK(!common::file_exists(path.c_str()).value_or(true));
    CHECK(common::remove_file(path.c_str()) == common::Status::kNotFound);

    // The name is free for a new file while the old one is still open.
    common::File new_file{};
    REQUIRE(common::is_ok(new_file.open(path.c_str(), create)));
    CHECK(common::is_ok(new_file.write_all(bytes_of("new"))));
    CHECK(common::is_ok(new_file.close()));
    CHECK(read_all(path) == "new");

    std::array<std::uint8_t, 32U> buffer{};
    const common::Result<std::size_t> count{old_file.read_at(common::Span<std::uint8_t>{buffer}, 0U)};
    REQUIRE(count.has_value());
    const std::string old_contents{reinterpret_cast<const char*>(buffer.data()), count.value()};
    CHECK(old_contents == "old contents");
    CHECK(common::is_ok(old_file.close()));
}
