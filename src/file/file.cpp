#include "common/file/file.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>

#include "detail/posix_status.hpp"

namespace common {
namespace {

static_assert(sizeof(off_t) == sizeof(std::int64_t), "a 64-bit off_t is required");

// An EINTR restart makes no progress; the bound only guards against a signal storm.
constexpr std::uint32_t kMaxInterruptRestarts{1000U};

// Linux transfers at most 0x7ffff000 bytes per read(2)/write(2) call.
constexpr std::size_t kMaxTransfer{0x7FFFF000U};

// Largest offset an off_t can hold.
constexpr std::uint64_t kMaxOffset{static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())};

// Permission bits accepted for new files and directories (octal 07777).
constexpr std::uint32_t kMaxPermissions{0xFFFU};

// POSIX defines the open(2) flags and file-type bits as int; flags are combined as unsigned values
// and converted once at the call.
constexpr std::uint32_t to_bits(const int flag) noexcept {
    return static_cast<std::uint32_t>(flag);
}

constexpr std::uint32_t kFileTypeMask{to_bits(S_IFMT)};
constexpr std::uint32_t kRegularFile{to_bits(S_IFREG)};

Status last_error() noexcept {
    return detail::status_from_errno(errno);
}

std::size_t transfer_size(const std::size_t requested) noexcept {
    return (requested < kMaxTransfer) ? requested : kMaxTransfer;
}

/// Runs a read/write-style call (returns a byte count, or -1 with errno) and restarts it after
/// EINTR, a bounded number of times.
template <typename Call>
Result<std::size_t> transfer_with_restart(const Call& call) noexcept {
    Result<std::size_t> result{Result<std::size_t>::failure(Status::kInterrupted)};
    for (std::uint32_t attempt{0U}; attempt < kMaxInterruptRestarts; ++attempt) {
        const ssize_t count{call()};
        if (count >= 0) {
            result = Result<std::size_t>::success(static_cast<std::size_t>(count));
            break;
        }
        if (errno != EINTR) {
            result = Result<std::size_t>::failure(last_error());
            break;
        }
    }
    return result;
}

/// Runs a call that returns 0 on success or -1 with errno, restarting it after EINTR (bounded).
template <typename Call>
Status call_with_restart(const Call& call) noexcept {
    Status status{Status::kInterrupted};
    for (std::uint32_t attempt{0U}; attempt < kMaxInterruptRestarts; ++attempt) {
        if (call() == 0) {
            status = Status::kOk;
            break;
        }
        if (errno != EINTR) {
            status = last_error();
            break;
        }
    }
    return status;
}

/// Opens with open(2), restarting after EINTR. @return the descriptor through @p descriptor.
Status open_with_restart(const char* const path, const std::uint32_t flags, const mode_t mode,
                         int& descriptor) noexcept {
    Status status{Status::kInterrupted};
    for (std::uint32_t attempt{0U}; attempt < kMaxInterruptRestarts; ++attempt) {
        // open(2) is variadic in C; it is the only way to open a file.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
        descriptor = ::open(path, static_cast<int>(flags), mode);
        if (descriptor >= 0) {
            status = Status::kOk;
            break;
        }
        if (errno != EINTR) {
            status = last_error();
            break;
        }
    }
    return status;
}

Status open_flags(const FileOptions& options, std::uint32_t& flags) noexcept {
    flags = to_bits(O_CLOEXEC);
    switch (options.access) {
        case FileAccess::kReadOnly:
            flags |= to_bits(O_RDONLY);
            break;
        case FileAccess::kWriteOnly:
            flags |= to_bits(O_WRONLY);
            break;
        case FileAccess::kReadWrite:
            flags |= to_bits(O_RDWR);
            break;
    }
    bool truncates{false};
    switch (options.creation) {
        case FileCreation::kOpenExisting:
            break;
        case FileCreation::kCreateOrOpen:
            flags |= to_bits(O_CREAT);
            break;
        case FileCreation::kCreateOrTruncate:
            flags |= to_bits(O_CREAT) | to_bits(O_TRUNC);
            truncates = true;
            break;
        case FileCreation::kCreateNew:
            flags |= to_bits(O_CREAT) | to_bits(O_EXCL);
            break;
    }
    if (options.append) {
        flags |= to_bits(O_APPEND);
    }
    const bool writable{options.access != FileAccess::kReadOnly};
    const bool valid{(writable || ((!truncates) && (!options.append))) && (options.permissions <= kMaxPermissions)};
    return valid ? Status::kOk : Status::kInvalidArgument;
}

}  // namespace

File::~File() {
    if (is_open()) {
        static_cast<void>(::close(descriptor_));
    }
}

File::File(File&& other) noexcept : descriptor_{other.descriptor_} {
    other.descriptor_ = -1;
}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        static_cast<void>(close());
        descriptor_ = other.descriptor_;
        other.descriptor_ = -1;
    }
    return *this;
}

Status File::open(const char* const path, const FileOptions& options) noexcept {
    Status status{Status::kInvalidArgument};
    std::uint32_t flags{0U};
    if ((path != nullptr) && (!is_open())) {
        status = open_flags(options, flags);
    }
    if (is_ok(status)) {
        const mode_t mode{options.permissions};
        int descriptor{-1};
        status = open_with_restart(path, flags, mode, descriptor);
        if (is_ok(status)) {
            descriptor_ = descriptor;
        }
    }
    return status;
}

Status File::adopt(const std::int32_t descriptor) noexcept {
    Status status{Status::kInvalidArgument};
    if ((descriptor >= 0) && (!is_open())) {
        descriptor_ = descriptor;
        status = Status::kOk;
    }
    return status;
}

std::int32_t File::release() noexcept {
    const std::int32_t descriptor{descriptor_};
    descriptor_ = -1;
    return descriptor;
}

Status File::close() noexcept {
    Status status{Status::kOk};
    if (is_open()) {
        const int rc{::close(descriptor_)};
        descriptor_ = -1;  // Released even when close(2) reports an error; never retry.
        if (rc != 0) {
            status = last_error();
        }
    }
    return status;
}

Result<std::size_t> File::read(const Span<std::uint8_t> buffer) noexcept {
    Result<std::size_t> result{Result<std::size_t>::failure(Status::kInvalidArgument)};
    if (is_open()) {
        result = transfer_with_restart(
            [this, &buffer]() noexcept { return ::read(descriptor_, buffer.data(), transfer_size(buffer.size())); });
    }
    return result;
}

Status File::read_exact(const Span<std::uint8_t> buffer) noexcept {
    Status status{is_open() ? Status::kOk : Status::kInvalidArgument};
    std::size_t done{0U};
    // Every round transfers at least one byte or ends the loop, so buffer.size() rounds suffice.
    for (std::size_t round{0U}; is_ok(status) && (done < buffer.size()) && (round < buffer.size()); ++round) {
        const Result<std::size_t> count{read(buffer.subspan(done))};
        if (!count.has_value()) {
            status = count.status();
        } else if (count.value() == 0U) {
            status = Status::kOutOfRange;  // End of file before the buffer was full.
        } else {
            done += count.value();
        }
    }
    return status;
}

Result<std::size_t> File::write(const Span<const std::uint8_t> data) noexcept {
    Result<std::size_t> result{Result<std::size_t>::failure(Status::kInvalidArgument)};
    if (is_open()) {
        result = transfer_with_restart(
            [this, &data]() noexcept { return ::write(descriptor_, data.data(), transfer_size(data.size())); });
    }
    return result;
}

Status File::write_all(const Span<const std::uint8_t> data) noexcept {
    Status status{is_open() ? Status::kOk : Status::kInvalidArgument};
    std::size_t done{0U};
    for (std::size_t round{0U}; is_ok(status) && (done < data.size()) && (round < data.size()); ++round) {
        const Result<std::size_t> count{write(data.subspan(done))};
        if (!count.has_value()) {
            status = count.status();
        } else if (count.value() == 0U) {
            status = Status::kSystemError;  // No progress for a non-empty write.
        } else {
            done += count.value();
        }
    }
    return status;
}

Result<std::size_t> File::read_at(const Span<std::uint8_t> buffer, const std::uint64_t offset) noexcept {
    Result<std::size_t> result{Result<std::size_t>::failure(Status::kInvalidArgument)};
    if (is_open()) {
        if (offset > kMaxOffset) {
            result = Result<std::size_t>::failure(Status::kOutOfRange);
        } else {
            const off_t position{static_cast<off_t>(offset)};
            result = transfer_with_restart([this, &buffer, position]() noexcept {
                return ::pread(descriptor_, buffer.data(), transfer_size(buffer.size()), position);
            });
        }
    }
    return result;
}

Status File::read_exact_at(const Span<std::uint8_t> buffer, const std::uint64_t offset) noexcept {
    Status status{is_open() ? Status::kOk : Status::kInvalidArgument};
    if (is_ok(status) && ((offset > kMaxOffset) || (buffer.size() > (kMaxOffset - offset)))) {
        status = Status::kOutOfRange;
    }
    std::size_t done{0U};
    for (std::size_t round{0U}; is_ok(status) && (done < buffer.size()) && (round < buffer.size()); ++round) {
        const Result<std::size_t> count{read_at(buffer.subspan(done), offset + done)};
        if (!count.has_value()) {
            status = count.status();
        } else if (count.value() == 0U) {
            status = Status::kOutOfRange;  // End of file before the buffer was full.
        } else {
            done += count.value();
        }
    }
    return status;
}

Status File::write_all_at(const Span<const std::uint8_t> data, const std::uint64_t offset) noexcept {
    Status status{is_open() ? Status::kOk : Status::kInvalidArgument};
    if (is_ok(status) && ((offset > kMaxOffset) || (data.size() > (kMaxOffset - offset)))) {
        status = Status::kOutOfRange;
    }
    std::size_t done{0U};
    for (std::size_t round{0U}; is_ok(status) && (done < data.size()) && (round < data.size()); ++round) {
        const Span<const std::uint8_t> rest{data.subspan(done)};
        const off_t position{static_cast<off_t>(offset + done)};
        const Result<std::size_t> count{transfer_with_restart([this, &rest, position]() noexcept {
            return ::pwrite(descriptor_, rest.data(), transfer_size(rest.size()), position);
        })};
        if (!count.has_value()) {
            status = count.status();
        } else if (count.value() == 0U) {
            status = Status::kSystemError;  // No progress for a non-empty write.
        } else {
            done += count.value();
        }
    }
    return status;
}

// Not const: it changes the file position, which is state of the open file.
Result<std::uint64_t> File::seek(const std::int64_t offset,  // NOLINT(readability-make-member-function-const)
                                 const SeekOrigin origin) noexcept {
    int whence{SEEK_SET};
    switch (origin) {
        case SeekOrigin::kBegin:
            whence = SEEK_SET;
            break;
        case SeekOrigin::kCurrent:
            whence = SEEK_CUR;
            break;
        case SeekOrigin::kEnd:
            whence = SEEK_END;
            break;
    }
    Result<std::uint64_t> result{Result<std::uint64_t>::failure(Status::kInvalidArgument)};
    if (is_open()) {
        const off_t position{::lseek(descriptor_, offset, whence)};
        result = (position < 0) ? Result<std::uint64_t>::failure(last_error())
                                : Result<std::uint64_t>::success(static_cast<std::uint64_t>(position));
    }
    return result;
}

Result<std::uint64_t> File::size() const noexcept {
    Result<std::uint64_t> result{Result<std::uint64_t>::failure(Status::kInvalidArgument)};
    if (is_open()) {
        struct stat info {};
        result = (::fstat(descriptor_, &info) != 0)
                     ? Result<std::uint64_t>::failure(last_error())
                     : Result<std::uint64_t>::success(static_cast<std::uint64_t>(info.st_size));
    }
    return result;
}

Status File::truncate(const std::uint64_t length) noexcept {
    Status status{Status::kInvalidArgument};
    if (is_open()) {
        if (length > kMaxOffset) {
            status = Status::kOutOfRange;
        } else {
            const off_t new_size{static_cast<off_t>(length)};
            status = call_with_restart([this, new_size]() noexcept { return ::ftruncate(descriptor_, new_size); });
        }
    }
    return status;
}

// Not const: it changes the file's size and allocation.
Status File::preallocate(const std::uint64_t length) noexcept {  // NOLINT(readability-make-member-function-const)
    Status status{is_open() ? Status::kOk : Status::kInvalidArgument};
    if (is_ok(status) && (length > kMaxOffset)) {
        status = Status::kOutOfRange;
    }
    if (is_ok(status) && (length > 0U)) {
        status = Status::kInterrupted;
        for (std::uint32_t attempt{0U}; attempt < kMaxInterruptRestarts; ++attempt) {
            // posix_fallocate returns the error number instead of setting errno.
            const int rc{::posix_fallocate(descriptor_, 0, static_cast<off_t>(length))};
            if (rc == 0) {
                status = Status::kOk;
                break;
            }
            if (rc != EINTR) {
                status = detail::status_from_errno(rc);
                break;
            }
        }
    }
    return status;
}

Status File::sync() noexcept {
    Status status{Status::kInvalidArgument};
    if (is_open()) {
        status = call_with_restart([this]() noexcept { return ::fsync(descriptor_); });
    }
    return status;
}

Status File::sync_data() noexcept {
    Status status{Status::kInvalidArgument};
    if (is_open()) {
        status = call_with_restart([this]() noexcept { return ::fdatasync(descriptor_); });
    }
    return status;
}

Status remove_file(const char* const path) noexcept {
    Status status{Status::kInvalidArgument};
    if (path != nullptr) {
        status = (::unlink(path) == 0) ? Status::kOk : last_error();
    }
    return status;
}

Status rename_file(const char* const from, const char* const to) noexcept {
    Status status{Status::kInvalidArgument};
    if ((from != nullptr) && (to != nullptr)) {
        // Deviation (MISRA C++:2023 Rule 30.0.1): rename() is declared in <cstdio>, but it is the POSIX
        // call that atomically replaces a file; no stream I/O is used.
        status = (std::rename(from, to) == 0) ? Status::kOk : last_error();
    }
    return status;
}

Result<bool> file_exists(const char* const path) noexcept {
    Result<bool> result{Result<bool>::failure(Status::kInvalidArgument)};
    if (path != nullptr) {
        struct stat info {};
        if (::stat(path, &info) == 0) {
            result = Result<bool>::success(true);
        } else {
            const int error{errno};
            // ENOTDIR: a component of the path is not a directory, so the path cannot exist.
            const bool missing{(error == ENOENT) || (error == ENOTDIR)};
            result = missing ? Result<bool>::success(false) : Result<bool>::failure(detail::status_from_errno(error));
        }
    }
    return result;
}

Result<std::uint64_t> file_size(const char* const path) noexcept {
    Result<std::uint64_t> result{Result<std::uint64_t>::failure(Status::kInvalidArgument)};
    if (path != nullptr) {
        struct stat info {};
        if (::stat(path, &info) != 0) {
            result = Result<std::uint64_t>::failure(last_error());
        } else if ((std::uint32_t{info.st_mode} & kFileTypeMask) == kRegularFile) {
            result = Result<std::uint64_t>::success(static_cast<std::uint64_t>(info.st_size));
        } else {
            // Not a regular file: keep kInvalidArgument.
        }
    }
    return result;
}

Status create_directory(const char* const path, const std::uint32_t permissions) noexcept {
    Status status{Status::kInvalidArgument};
    if ((path != nullptr) && (permissions <= kMaxPermissions)) {
        const mode_t mode{permissions};
        status = (::mkdir(path, mode) == 0) ? Status::kOk : last_error();
    }
    return status;
}

Status sync_directory(const char* const path) noexcept {
    Status status{Status::kInvalidArgument};
    if (path != nullptr) {
        int descriptor{-1};
        const std::uint32_t flags{to_bits(O_RDONLY) | to_bits(O_DIRECTORY) | to_bits(O_CLOEXEC)};
        status = open_with_restart(path, flags, 0U, descriptor);
        if (is_ok(status)) {
            File directory{};
            static_cast<void>(directory.adopt(descriptor));
            status = directory.sync();
            const Status close_status{directory.close()};
            status = is_ok(status) ? close_status : status;
        }
    }
    return status;
}

}  // namespace common
