#include "common/file/file.hpp"

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#if defined(_WIN32)
#include "detail/win32.hpp"
#include "detail/win32_status.hpp"
#else
#include "detail/posix_status.hpp"
#endif

namespace common {
namespace {

// Largest offset an off_t (and on Windows a LARGE_INTEGER) can hold.
constexpr std::uint64_t kMaxOffset{static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())};

// Permission bits accepted for new files and directories (octal 07777).
constexpr std::uint32_t kMaxPermissions{0xFFFU};

/// @return kOk if @p options do not contradict each other: truncation and append mode need write
///         access, and the permission bits must fit in 0xFFF.
Status validate(const FileOptions& options) noexcept {
    const bool writable{options.access != FileAccess::kReadOnly};
    const bool truncates{options.creation == FileCreation::kCreateOrTruncate};
    const bool valid{(writable || ((!truncates) && (!options.append))) && (options.permissions <= kMaxPermissions)};
    return valid ? Status::kOk : Status::kInvalidArgument;
}

#if defined(_WIN32)

// Shares the file like POSIX does: while it is open, others may read, write, rename and delete it.
constexpr DWORD kShareAll{FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE};

Status last_error() noexcept {
    return detail::last_win32_error();
}

[[nodiscard]] bool is_valid_handle(const NativeHandle handle) noexcept {
    return (handle != nullptr) && (handle != INVALID_HANDLE_VALUE);
}

/// @return true if CloseHandle() succeeded; the handle is released either way.
[[nodiscard]] bool close_handle(const NativeHandle handle) noexcept {
    return ::CloseHandle(handle) != FALSE;
}

/// The Status of a call on @p path that failed with @p error. A directory where a file is expected is
/// kInvalidArgument, as on Linux (EISDIR), where Windows reports ERROR_ACCESS_DENIED.
Status path_error(const wchar_t* const path, const DWORD error) noexcept {
    const DWORD attributes{(error == ERROR_ACCESS_DENIED) ? ::GetFileAttributesW(path) : INVALID_FILE_ATTRIBUTES};
    const bool directory{(attributes != INVALID_FILE_ATTRIBUTES) && ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U)};
    return directory ? Status::kInvalidArgument : detail::status_from_win32(error);
}

/// The access rights to open a file with. Write access includes reading the attributes, so that
/// size() works on a write-only File as fstat(2) does. In append mode they leave out
/// FILE_WRITE_DATA, so that Windows writes all data at the end of the file, as O_APPEND does.
DWORD desired_access(const FileOptions& options) noexcept {
    const DWORD generic_write{FILE_GENERIC_WRITE | FILE_READ_ATTRIBUTES};
    const DWORD write_access{options.append ? (generic_write & (~static_cast<DWORD>(FILE_WRITE_DATA)))
                                            : generic_write};
    DWORD access{FILE_GENERIC_READ};
    switch (options.access) {
        case FileAccess::kReadOnly:
            access = FILE_GENERIC_READ;
            break;
        case FileAccess::kWriteOnly:
            access = write_access;
            break;
        case FileAccess::kReadWrite:
            access = FILE_GENERIC_READ | write_access;
            break;
    }
    return access;
}

DWORD creation_disposition(const FileCreation creation) noexcept {
    DWORD disposition{OPEN_EXISTING};
    switch (creation) {
        case FileCreation::kOpenExisting:
            disposition = OPEN_EXISTING;
            break;
        case FileCreation::kCreateOrOpen:
            disposition = OPEN_ALWAYS;
            break;
        case FileCreation::kCreateOrTruncate:
            disposition = CREATE_ALWAYS;
            break;
        case FileCreation::kCreateNew:
            disposition = CREATE_NEW;
            break;
    }
    return disposition;
}

/// An OVERLAPPED that makes a synchronous ReadFile() or WriteFile() transfer at @p offset.
OVERLAPPED at_offset(const std::uint64_t offset) noexcept {
    OVERLAPPED position{};
    position.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFU);
    position.OffsetHigh = static_cast<DWORD>(offset >> 32U);
    return position;
}

/// One ReadFile() of up to buffer.size() bytes, at @p offset unless it is nullptr. The end of the file
/// and the end of a pipe whose writers have all closed it read as 0 bytes, as with read(2).
Result<std::size_t> read_handle(const HANDLE handle, const Span<std::uint8_t> buffer,
                                const std::uint64_t* const offset) noexcept {
    OVERLAPPED position{(offset != nullptr) ? at_offset(*offset) : OVERLAPPED{}};
    DWORD count{0U};
    Result<std::size_t> result{Result<std::size_t>::success(0U)};
    if (::ReadFile(handle, buffer.data(), detail::transfer_count(buffer.size()), &count,
                   (offset != nullptr) ? &position : nullptr) != FALSE) {
        result = Result<std::size_t>::success(count);
    } else {
        const DWORD error{::GetLastError()};
        if ((error != ERROR_HANDLE_EOF) && (error != ERROR_BROKEN_PIPE)) {
            result = Result<std::size_t>::failure(detail::status_from_win32(error));
        }
    }
    return result;
}

/// One WriteFile() of up to data.size() bytes, at @p offset unless it is nullptr.
Result<std::size_t> write_handle(const HANDLE handle, const Span<const std::uint8_t> data,
                                 const std::uint64_t* const offset) noexcept {
    OVERLAPPED position{(offset != nullptr) ? at_offset(*offset) : OVERLAPPED{}};
    DWORD count{0U};
    return (::WriteFile(handle, data.data(), detail::transfer_count(data.size()), &count,
                        (offset != nullptr) ? &position : nullptr) != FALSE)
               ? Result<std::size_t>::success(count)
               : Result<std::size_t>::failure(last_error());
}

/// Runs a transfer at an offset and puts the file position back afterwards: on a synchronous handle,
/// ReadFile() and WriteFile() at an offset move the position, unlike pread(2) and pwrite(2).
template <typename Transfer>
Result<std::size_t> keeping_position(const HANDLE handle, const Transfer& transfer) noexcept {
    LARGE_INTEGER position{};
    Result<std::size_t> result{Result<std::size_t>::failure(Status::kSystemError)};
    if (::SetFilePointerEx(handle, detail::large_integer(0), &position, FILE_CURRENT) == FALSE) {
        result = Result<std::size_t>::failure(last_error());
    } else {
        result = transfer();
        if ((::SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) == FALSE) && result.has_value()) {
            result = Result<std::size_t>::failure(last_error());
        }
    }
    return result;
}

/// Sets the end of the file open at @p handle to @p length bytes, without moving the file position.
Status set_end_of_file(const HANDLE handle, const std::uint64_t length) noexcept {
    FILE_END_OF_FILE_INFO info{};
    info.EndOfFile = detail::large_integer(static_cast<std::int64_t>(length));
    return (::SetFileInformationByHandle(handle, FileEndOfFileInfo, &info, sizeof(info)) != FALSE) ? Status::kOk
                                                                                                  : last_error();
}

/// Opens @p path only to query its attributes, following symbolic links as stat(2) does. Asking for
/// neither read nor write access never conflicts with how others opened the file;
/// FILE_FLAG_BACKUP_SEMANTICS allows directories.
HANDLE open_attributes(const wchar_t* const path) noexcept {
    return ::CreateFileW(path, FILE_READ_ATTRIBUTES, kShareAll, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
                         nullptr);
}

/// Renames @p from to @p to with POSIX semantics (Windows 10 1607 and later, on NTFS): an existing
/// @p to is replaced even while it is open, as with rename(2), and open handles keep the old file.
/// @return kNotSupported where Windows or the file system does not offer these semantics.
Status rename_posix(const wchar_t* const from, const wchar_t* const to) noexcept {
    std::size_t length{0U};  // Of the new name, in UTF-16 code units, without the NUL.
    while ((length < detail::WidePath::kCapacity) && (to[length] != L'\0')) {
        ++length;
    }
    // FILE_RENAME_INFO ends in the new name, of variable length: the structure is assembled in an
    // aligned byte buffer, its fixed part followed by the name and a NUL.
    // Deviation (MISRA C++:2023 Rule 21.2.4): offsetof gives the position of the name.
    constexpr std::size_t kNameOffset{offsetof(FILE_RENAME_INFO, FileName)};
    alignas(FILE_RENAME_INFO) std::array<std::uint8_t, kNameOffset + (sizeof(wchar_t) * detail::WidePath::kCapacity)>
        request{};
    FILE_RENAME_INFO header{};
    header.Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    header.RootDirectory = nullptr;
    header.FileNameLength = static_cast<DWORD>(length * sizeof(wchar_t));
    static_cast<void>(std::memcpy(request.data(), &header, kNameOffset));
    static_cast<void>(std::memcpy(&request[kNameOffset], to, (length + 1U) * sizeof(wchar_t)));

    // Renaming needs DELETE access. FILE_FLAG_OPEN_REPARSE_POINT renames a symbolic link itself, and
    // FILE_FLAG_BACKUP_SEMANTICS allows directories, as with rename(2).
    const HANDLE source{::CreateFileW(from, DELETE | SYNCHRONIZE, kShareAll, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    Status status{(source != INVALID_HANDLE_VALUE) ? Status::kOk : last_error()};
    if (is_ok(status)) {
        if (::SetFileInformationByHandle(source, FileRenameInfoEx, request.data(),
                                         static_cast<DWORD>(kNameOffset + ((length + 1U) * sizeof(wchar_t)))) == FALSE) {
            const DWORD error{::GetLastError()};
            const bool unsupported{(error == ERROR_INVALID_PARAMETER) || (error == ERROR_NOT_SUPPORTED) ||
                                   (error == ERROR_INVALID_FUNCTION)};
            status = unsupported ? Status::kNotSupported : detail::status_from_win32(error);
        }
        static_cast<void>(::CloseHandle(source));
    }
    return status;
}

#else

static_assert(sizeof(off_t) == sizeof(std::int64_t), "a 64-bit off_t is required");

// An EINTR restart makes no progress; the bound only guards against a signal storm.
constexpr std::uint32_t kMaxInterruptRestarts{1000U};

// Linux transfers at most 0x7ffff000 bytes per read(2)/write(2) call.
constexpr std::size_t kMaxTransfer{0x7FFFF000U};

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

[[nodiscard]] bool is_valid_handle(const NativeHandle descriptor) noexcept {
    return descriptor >= 0;
}

/// @return true if close(2) succeeded; Linux releases the descriptor either way.
[[nodiscard]] bool close_handle(const NativeHandle descriptor) noexcept {
    return ::close(descriptor) == 0;
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

std::uint32_t open_flags(const FileOptions& options) noexcept {
    std::uint32_t flags{to_bits(O_CLOEXEC)};
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
    switch (options.creation) {
        case FileCreation::kOpenExisting:
            break;
        case FileCreation::kCreateOrOpen:
            flags |= to_bits(O_CREAT);
            break;
        case FileCreation::kCreateOrTruncate:
            flags |= to_bits(O_CREAT) | to_bits(O_TRUNC);
            break;
        case FileCreation::kCreateNew:
            flags |= to_bits(O_CREAT) | to_bits(O_EXCL);
            break;
    }
    if (options.append) {
        flags |= to_bits(O_APPEND);
    }
    return flags;
}

#endif

}  // namespace

File::~File() {
    if (is_open()) {
        static_cast<void>(close_handle(descriptor_));
    }
}

File::File(File&& other) noexcept : descriptor_{other.descriptor_} {
    other.descriptor_ = kInvalidNativeHandle;
}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        static_cast<void>(close());
        descriptor_ = other.descriptor_;
        other.descriptor_ = kInvalidNativeHandle;
    }
    return *this;
}

Status File::open(const char* const path, const FileOptions& options) noexcept {
    Status status{Status::kInvalidArgument};
    if ((path != nullptr) && (!is_open())) {
        status = validate(options);
    }
#if defined(_WIN32)
    detail::WidePath wide{};
    if (is_ok(status)) {
        status = wide.assign(path);
    }
    if (is_ok(status)) {
        // Without security attributes, child processes do not inherit the handle (as O_CLOEXEC). Windows
        // has no permission bits: a new file gets the default security of its directory.
        const HANDLE handle{::CreateFileW(wide.c_str(), desired_access(options), kShareAll, nullptr,
                                          creation_disposition(options.creation), FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (handle != INVALID_HANDLE_VALUE) {
            descriptor_ = handle;
        } else {
            status = path_error(wide.c_str(), ::GetLastError());
        }
    }
#else
    if (is_ok(status)) {
        const mode_t mode{options.permissions};
        int descriptor{-1};
        status = open_with_restart(path, open_flags(options), mode, descriptor);
        if (is_ok(status)) {
            descriptor_ = descriptor;
        }
    }
#endif
    return status;
}

Status File::adopt(const NativeHandle descriptor) noexcept {
    Status status{Status::kInvalidArgument};
    if (is_valid_handle(descriptor) && (!is_open())) {
        descriptor_ = descriptor;
        status = Status::kOk;
    }
    return status;
}

NativeHandle File::release() noexcept {
    const NativeHandle descriptor{descriptor_};
    descriptor_ = kInvalidNativeHandle;
    return descriptor;
}

Status File::close() noexcept {
    Status status{Status::kOk};
    if (is_open()) {
        const bool closed{close_handle(descriptor_)};
        descriptor_ = kInvalidNativeHandle;  // Released even when closing reports an error; never retry.
        if (!closed) {
            status = last_error();
        }
    }
    return status;
}

Result<std::size_t> File::read(const Span<std::uint8_t> buffer) noexcept {
    Result<std::size_t> result{Result<std::size_t>::failure(Status::kInvalidArgument)};
    if (is_open()) {
#if defined(_WIN32)
        result = read_handle(descriptor_, buffer, nullptr);
#else
        result = transfer_with_restart(
            [this, &buffer]() noexcept { return ::read(descriptor_, buffer.data(), transfer_size(buffer.size())); });
#endif
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
#if defined(_WIN32)
        result = write_handle(descriptor_, data, nullptr);
#else
        result = transfer_with_restart(
            [this, &data]() noexcept { return ::write(descriptor_, data.data(), transfer_size(data.size())); });
#endif
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
#if defined(_WIN32)
            const HANDLE handle{descriptor_};
            result = keeping_position(handle, [handle, &buffer, &offset]() noexcept {
                return read_handle(handle, buffer, &offset);
            });
#else
            const off_t position{static_cast<off_t>(offset)};
            result = transfer_with_restart([this, &buffer, position]() noexcept {
                return ::pread(descriptor_, buffer.data(), transfer_size(buffer.size()), position);
            });
#endif
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
#if defined(_WIN32)
        const HANDLE handle{descriptor_};
        const std::uint64_t position{offset + done};
        const Result<std::size_t> count{keeping_position(handle, [handle, &rest, &position]() noexcept {
            return write_handle(handle, rest, &position);
        })};
#else
        const off_t position{static_cast<off_t>(offset + done)};
        const Result<std::size_t> count{transfer_with_restart([this, &rest, position]() noexcept {
            return ::pwrite(descriptor_, rest.data(), transfer_size(rest.size()), position);
        })};
#endif
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
#if defined(_WIN32)
    DWORD whence{FILE_BEGIN};
    switch (origin) {
        case SeekOrigin::kBegin:
            whence = FILE_BEGIN;
            break;
        case SeekOrigin::kCurrent:
            whence = FILE_CURRENT;
            break;
        case SeekOrigin::kEnd:
            whence = FILE_END;
            break;
    }
#else
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
#endif
    Result<std::uint64_t> result{Result<std::uint64_t>::failure(Status::kInvalidArgument)};
    if (is_open()) {
#if defined(_WIN32)
        LARGE_INTEGER position{};
        result = (::SetFilePointerEx(descriptor_, detail::large_integer(offset), &position, whence) == FALSE)
                     ? Result<std::uint64_t>::failure(last_error())
                     : Result<std::uint64_t>::success(static_cast<std::uint64_t>(position.QuadPart));
#else
        const off_t position{::lseek(descriptor_, offset, whence)};
        result = (position < 0) ? Result<std::uint64_t>::failure(last_error())
                                : Result<std::uint64_t>::success(static_cast<std::uint64_t>(position));
#endif
    }
    return result;
}

Result<std::uint64_t> File::size() const noexcept {
    Result<std::uint64_t> result{Result<std::uint64_t>::failure(Status::kInvalidArgument)};
    if (is_open()) {
#if defined(_WIN32)
        LARGE_INTEGER size{};
        result = (::GetFileSizeEx(descriptor_, &size) == FALSE)
                     ? Result<std::uint64_t>::failure(last_error())
                     : Result<std::uint64_t>::success(static_cast<std::uint64_t>(size.QuadPart));
#else
        struct stat info {};
        result = (::fstat(descriptor_, &info) != 0)
                     ? Result<std::uint64_t>::failure(last_error())
                     : Result<std::uint64_t>::success(static_cast<std::uint64_t>(info.st_size));
#endif
    }
    return result;
}

Status File::truncate(const std::uint64_t length) noexcept {
    Status status{Status::kInvalidArgument};
    if (is_open()) {
        if (length > kMaxOffset) {
            status = Status::kOutOfRange;
        } else {
#if defined(_WIN32)
            status = set_end_of_file(descriptor_, length);
#else
            const off_t new_size{static_cast<off_t>(length)};
            status = call_with_restart([this, new_size]() noexcept { return ::ftruncate(descriptor_, new_size); });
#endif
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
#if defined(_WIN32)
        // Reserving storage (the allocation size) never shrinks the file here: a smaller allocation
        // size would cut it. A longer file already has its storage, because Windows files are not
        // sparse unless marked so.
        LARGE_INTEGER size{};
        status = (::GetFileSizeEx(descriptor_, &size) != FALSE) ? Status::kOk : last_error();
        if (is_ok(status) && (static_cast<std::uint64_t>(size.QuadPart) < length)) {
            FILE_ALLOCATION_INFO allocation{};
            allocation.AllocationSize = detail::large_integer(static_cast<std::int64_t>(length));
            status = (::SetFileInformationByHandle(descriptor_, FileAllocationInfo, &allocation, sizeof(allocation)) !=
                      FALSE)
                         ? Status::kOk
                         : last_error();
            if (is_ok(status)) {
                status = set_end_of_file(descriptor_, length);  // Grows the file, as posix_fallocate does.
            }
        }
#elif defined(__APPLE__)
        // macOS and iOS have no posix_fallocate: F_PREALLOCATE reserves storage after the end of the
        // file (contiguous if it can), and ftruncate() then grows the file into it.
        struct stat info {};
        status = (::fstat(descriptor_, &info) == 0) ? Status::kOk : last_error();
        const std::uint64_t current{is_ok(status) ? static_cast<std::uint64_t>(info.st_size) : 0U};
        if (is_ok(status) && (current < length)) {
            fstore_t store{};
            store.fst_flags = static_cast<std::uint32_t>(F_ALLOCATECONTIG) | static_cast<std::uint32_t>(F_ALLOCATEALL);
            store.fst_posmode = F_PEOFPOSMODE;
            store.fst_offset = 0;
            store.fst_length = static_cast<off_t>(length - current);
            if (::fcntl(descriptor_, F_PREALLOCATE, &store) == -1) {
                store.fst_flags = static_cast<std::uint32_t>(F_ALLOCATEALL);  // Not contiguous, then.
                status = (::fcntl(descriptor_, F_PREALLOCATE, &store) == -1) ? last_error() : Status::kOk;
            }
            if (is_ok(status)) {
                const off_t new_size{static_cast<off_t>(length)};
                status = call_with_restart([this, new_size]() noexcept { return ::ftruncate(descriptor_, new_size); });
            }
        }
#else
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
#endif
    }
    return status;
}

Status File::sync() noexcept {
    Status status{Status::kInvalidArgument};
    if (is_open()) {
#if defined(_WIN32)
        status = (::FlushFileBuffers(descriptor_) != FALSE) ? Status::kOk : last_error();
#elif defined(__APPLE__)
        // fsync() on macOS and iOS leaves the data in the drive's cache; F_FULLFSYNC flushes that too,
        // as fsync() does on Linux. A file system without it gets fsync().
        status = call_with_restart([this]() noexcept { return (::fcntl(descriptor_, F_FULLFSYNC) == -1) ? -1 : 0; });
        if (!is_ok(status)) {
            status = call_with_restart([this]() noexcept { return ::fsync(descriptor_); });
        }
#else
        status = call_with_restart([this]() noexcept { return ::fsync(descriptor_); });
#endif
    }
    return status;
}

Status File::sync_data() noexcept {
    Status status{Status::kInvalidArgument};
    if (is_open()) {
#if defined(_WIN32) || defined(__APPLE__)
        status = sync();  // No cheaper call for the data alone (on macOS and iOS, none that flushes it).
#else
        status = call_with_restart([this]() noexcept { return ::fdatasync(descriptor_); });
#endif
    }
    return status;
}

Status remove_file(const char* const path) noexcept {
    Status status{Status::kInvalidArgument};
    if (path != nullptr) {
#if defined(_WIN32)
        detail::WidePath wide{};
        status = wide.assign(path);
        if (is_ok(status) && (::DeleteFileW(wide.c_str()) == FALSE)) {
            status = path_error(wide.c_str(), ::GetLastError());
        }
#else
        status = (::unlink(path) == 0) ? Status::kOk : last_error();
#endif
    }
    return status;
}

Status rename_file(const char* const from, const char* const to) noexcept {
    Status status{Status::kInvalidArgument};
    if ((from != nullptr) && (to != nullptr)) {
#if defined(_WIN32)
        detail::WidePath wide_from{};
        detail::WidePath wide_to{};
        status = wide_from.assign(from);
        if (is_ok(status)) {
            status = wide_to.assign(to);
        }
        if (is_ok(status)) {
            status = rename_posix(wide_from.c_str(), wide_to.c_str());
        }
        if (status == Status::kNotSupported) {
            // Without POSIX semantics (e.g. FAT or a network share), replacing fails while the target
            // is open elsewhere.
            status = (::MoveFileExW(wide_from.c_str(), wide_to.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE)
                         ? Status::kOk
                         : last_error();
        }
#else
        // Deviation (MISRA C++:2023 Rule 30.0.1): rename() is declared in <cstdio>, but it is the POSIX
        // call that atomically replaces a file; no stream I/O is used.
        status = (std::rename(from, to) == 0) ? Status::kOk : last_error();
#endif
    }
    return status;
}

Result<bool> file_exists(const char* const path) noexcept {
    Result<bool> result{Result<bool>::failure(Status::kInvalidArgument)};
    if (path != nullptr) {
#if defined(_WIN32)
        detail::WidePath wide{};
        if (is_ok(wide.assign(path))) {
            const HANDLE handle{open_attributes(wide.c_str())};
            if (handle != INVALID_HANDLE_VALUE) {
                static_cast<void>(::CloseHandle(handle));
                result = Result<bool>::success(true);
            } else {
                const DWORD error{::GetLastError()};
                // ERROR_PATH_NOT_FOUND also covers a component of the path that is not a directory.
                const bool missing{(error == ERROR_FILE_NOT_FOUND) || (error == ERROR_PATH_NOT_FOUND)};
                result = missing ? Result<bool>::success(false)
                                 : Result<bool>::failure(detail::status_from_win32(error));
            }
        }
#else
        struct stat info {};
        if (::stat(path, &info) == 0) {
            result = Result<bool>::success(true);
        } else {
            const int error{errno};
            // ENOTDIR: a component of the path is not a directory, so the path cannot exist.
            const bool missing{(error == ENOENT) || (error == ENOTDIR)};
            result = missing ? Result<bool>::success(false) : Result<bool>::failure(detail::status_from_errno(error));
        }
#endif
    }
    return result;
}

Result<std::uint64_t> file_size(const char* const path) noexcept {
    Result<std::uint64_t> result{Result<std::uint64_t>::failure(Status::kInvalidArgument)};
    if (path != nullptr) {
#if defined(_WIN32)
        detail::WidePath wide{};
        if (is_ok(wide.assign(path))) {
            const HANDLE handle{open_attributes(wide.c_str())};
            if (handle == INVALID_HANDLE_VALUE) {
                result = Result<std::uint64_t>::failure(last_error());
            } else {
                BY_HANDLE_FILE_INFORMATION info{};
                if (::GetFileType(handle) != FILE_TYPE_DISK) {
                    // A device such as NUL or a COM port: keep kInvalidArgument.
                } else if (::GetFileInformationByHandle(handle, &info) == FALSE) {
                    result = Result<std::uint64_t>::failure(last_error());
                } else if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U) {
                    result = Result<std::uint64_t>::success((std::uint64_t{info.nFileSizeHigh} << 32U) |
                                                            std::uint64_t{info.nFileSizeLow});
                } else {
                    // A directory: keep kInvalidArgument.
                }
                static_cast<void>(::CloseHandle(handle));
            }
        }
#else
        struct stat info {};
        if (::stat(path, &info) != 0) {
            result = Result<std::uint64_t>::failure(last_error());
        } else if ((std::uint32_t{info.st_mode} & kFileTypeMask) == kRegularFile) {
            result = Result<std::uint64_t>::success(static_cast<std::uint64_t>(info.st_size));
        } else {
            // Not a regular file: keep kInvalidArgument.
        }
#endif
    }
    return result;
}

Status create_directory(const char* const path, const std::uint32_t permissions) noexcept {
    Status status{Status::kInvalidArgument};
    if ((path != nullptr) && (permissions <= kMaxPermissions)) {
#if defined(_WIN32)
        // The permission bits have no Windows equivalent: the directory inherits the security of its parent.
        detail::WidePath wide{};
        status = wide.assign(path);
        if (is_ok(status) && (::CreateDirectoryW(wide.c_str(), nullptr) == FALSE)) {
            status = last_error();
        }
#else
        const mode_t mode{permissions};
        status = (::mkdir(path, mode) == 0) ? Status::kOk : last_error();
#endif
    }
    return status;
}

Status sync_directory(const char* const path) noexcept {
    Status status{Status::kInvalidArgument};
    if (path != nullptr) {
#if defined(_WIN32)
        // Flushing a directory needs a handle with write access, which FILE_FLAG_BACKUP_SEMANTICS allows
        // to open.
        detail::WidePath wide{};
        status = wide.assign(path);
        if (is_ok(status)) {
            const HANDLE handle{::CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, kShareAll, nullptr,
                                              OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
            if (handle == INVALID_HANDLE_VALUE) {
                status = last_error();
            } else {
                BY_HANDLE_FILE_INFORMATION info{};
                if (::GetFileInformationByHandle(handle, &info) == FALSE) {
                    status = last_error();
                } else if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U) {
                    status = Status::kInvalidArgument;  // Not a directory, as ENOTDIR on Linux.
                } else {
                    status = (::FlushFileBuffers(handle) != FALSE) ? Status::kOk : last_error();
                }
                static_cast<void>(::CloseHandle(handle));
            }
        }
#else
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
#endif
    }
    return status;
}

}  // namespace common
