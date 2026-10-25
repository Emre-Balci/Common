/// @file file.hpp
/// @brief Owning handle for files (POSIX descriptors, Windows handles), and basic file-system
///        operations.
///
/// File operations block in the kernel (page cache, storage, network file systems), so nothing in
/// this header is safe on the real-time path. Use it during initialisation, or on a non-real-time
/// thread that the real-time threads feed through a queue.

#ifndef COMMON_FILE_FILE_HPP
#define COMMON_FILE_FILE_HPP

#include <cstddef>
#include <cstdint>

#include "common/containers/span.hpp"
#include "common/core/config.hpp"
#include "common/core/status.hpp"

namespace common {

enum class FileAccess : std::uint8_t {
    kReadOnly,
    kWriteOnly,
    kReadWrite,
};

enum class FileCreation : std::uint8_t {
    kOpenExisting,      ///< The file must exist (kNotFound otherwise).
    kCreateOrOpen,      ///< Creates the file if needed and keeps existing contents.
    kCreateOrTruncate,  ///< Creates the file if needed and discards existing contents.
    kCreateNew,         ///< Creates the file; kAlreadyExists if it exists (O_EXCL).
};

/// How File::open() opens a file. Descriptors are always opened with O_CLOEXEC; on Windows, child
/// processes never inherit the handle.
struct FileOptions final {
    FileAccess access{FileAccess::kReadOnly};
    FileCreation creation{FileCreation::kOpenExisting};
    /// Every write goes to the end of the file (O_APPEND). Requires write access. On Windows the
    /// handle then lacks the right to overwrite data, so truncate() and preallocate() fail with
    /// kPermissionDenied.
    bool append{false};
    /// Permission bits (at most 0xFFF, i.e. octal 07777) for a newly created file, before the
    /// process umask is applied. The default 0x1A4 is octal 0644 (rw-r--r--). Windows has no
    /// permission bits: they are checked, and a new file gets the default security of its directory.
    std::uint32_t permissions{0x1A4U};
};

enum class SeekOrigin : std::uint8_t {
    kBegin,
    kCurrent,
    kEnd,
};

/// Owns one file descriptor (on Windows, a file HANDLE) and closes it on destruction. Move-only.
///
/// Reads and writes restart after EINTR (a bounded number of times). read() and write() make
/// one system call and may transfer fewer bytes than asked; read_exact() and write_all() loop
/// until the whole buffer is transferred. Operations on a closed handle return kInvalidArgument.
///
/// Error mapping: kNotFound (no such file), kAlreadyExists, kPermissionDenied (including a
/// read-only file system), kResourceLimit (no space, quota, too many open files),
/// kInvalidArgument, kInterrupted, kSystemError (I/O error and anything else). Windows errors map
/// to the same codes.
///
/// On Windows, paths are UTF-8, and a file is opened with the sharing of a POSIX file: while it is
/// open, other handles may read, write, rename and delete it. A directory cannot be opened as a File
/// there (kInvalidArgument).
///
/// Not thread-safe; one thread uses a File at a time. Never use it on a real-time thread.
class File final {
public:
    File() noexcept = default;

    /// Closes the descriptor if one is open. Errors cannot be reported here: call close() first
    /// when they matter (e.g. after writing).
    ~File();

    File(File&& other) noexcept;
    File& operator=(File&& other) noexcept;
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    /// Opens @p path.
    /// @return kInvalidArgument for nullptr, a handle that is already open, permission bits above
    ///         0xFFF, or contradictory options (truncation or append without write access);
    ///         otherwise the open(2) result.
    Status open(const char* path, const FileOptions& options) noexcept;

    /// Takes ownership of an open descriptor, e.g. one end of a pipe. On Windows it takes a HANDLE
    /// opened for synchronous (not overlapped) I/O.
    /// @return kInvalidArgument if @p descriptor < 0 (on Windows nullptr or INVALID_HANDLE_VALUE) or
    ///         this handle is already open.
    Status adopt(NativeHandle descriptor) noexcept;

    /// Gives up ownership without closing. @return the descriptor, or kInvalidNativeHandle (-1 on
    /// Linux) if none was open.
    [[nodiscard]] NativeHandle release() noexcept;

    /// Closes the descriptor; afterwards the handle is closed even if an error is reported (Linux
    /// releases the descriptor in every case, so a failed close must not be retried).
    /// @return kOk (also when nothing was open), or the error close(2) reported, e.g. a deferred
    ///         write error of a network file system.
    Status close() noexcept;

    [[nodiscard]] bool is_open() const noexcept {
        return descriptor_ != kInvalidNativeHandle;
    }

    /// @return the descriptor (on Windows, the HANDLE), or kInvalidNativeHandle (-1 on Linux) when
    ///         closed.
    [[nodiscard]] NativeHandle descriptor() const noexcept {
        return descriptor_;
    }

    /// One read(2) at the current position. @return the number of bytes read; 0 at end of file
    /// (or for an empty buffer).
    [[nodiscard]] Result<std::size_t> read(Span<std::uint8_t> buffer) noexcept;

    /// Fills the whole buffer, looping over short reads.
    /// @return kOutOfRange if the file ends first (the buffer then holds the bytes that were read).
    Status read_exact(Span<std::uint8_t> buffer) noexcept;

    /// One write(2) at the current position (at the end in append mode).
    /// @return the number of bytes written, which may be less than data.size().
    [[nodiscard]] Result<std::size_t> write(Span<const std::uint8_t> data) noexcept;

    /// Writes all of @p data, looping over short writes. On error, part of it may be written.
    Status write_all(Span<const std::uint8_t> data) noexcept;

    /// One pread(2) at @p offset; the file position does not change.
    /// @return the number of bytes read, 0 at end of file; kOutOfRange if offset exceeds INT64_MAX.
    [[nodiscard]] Result<std::size_t> read_at(Span<std::uint8_t> buffer, std::uint64_t offset) noexcept;

    /// Fills the whole buffer from @p offset. @return kOutOfRange if the file ends first.
    Status read_exact_at(Span<std::uint8_t> buffer, std::uint64_t offset) noexcept;

    /// Writes all of @p data at @p offset with pwrite(2); the file position does not change.
    /// (In append mode Linux and Windows append regardless of the offset.)
    Status write_all_at(Span<const std::uint8_t> data, std::uint64_t offset) noexcept;

    /// Moves the file position. @return the new position from the start of the file.
    [[nodiscard]] Result<std::uint64_t> seek(std::int64_t offset, SeekOrigin origin) noexcept;

    /// @return the current size of the file in bytes (fstat).
    [[nodiscard]] Result<std::uint64_t> size() const noexcept;

    /// Sets the file size to @p length bytes, cutting or zero-extending it (ftruncate).
    Status truncate(std::uint64_t length) noexcept;

    /// Reserves storage for the first @p length bytes (posix_fallocate; on Windows the allocation
    /// size), so later writes in that range do not have to allocate blocks and cannot fail for lack
    /// of space. The file grows to @p length if it is shorter.
    /// @return kNotSupported if the file system cannot do it.
    Status preallocate(std::uint64_t length) noexcept;

    /// Waits until data and metadata are on stable storage (fsync; FlushFileBuffers on Windows,
    /// which needs write access: kPermissionDenied for a read-only File).
    Status sync() noexcept;

    /// Waits until the data (and the metadata needed to read it back) is on stable storage
    /// (fdatasync); cheaper than sync(). On Windows the same as sync().
    Status sync_data() noexcept;

private:
    NativeHandle descriptor_{kInvalidNativeHandle};
};

/// Deletes @p path (unlink; DeleteFileW on Windows). @return kNotFound if it does not exist.
Status remove_file(const char* path) noexcept;

/// Renames @p from to @p to, atomically replacing an existing @p to on the same file system.
/// Call sync_directory() on the parent directory to make the rename durable.
///
/// On Windows it renames with POSIX semantics where the file system offers them (NTFS on Windows
/// 10 1607 and later): an existing @p to is replaced even while it is open, and open handles keep
/// the old file. Elsewhere (e.g. FAT) it uses MoveFileExW(), which cannot replace an open file.
Status rename_file(const char* from, const char* to) noexcept;

/// @return true if @p path exists (any file type), false if it does not; an error if that cannot
///         be determined (e.g. kPermissionDenied).
[[nodiscard]] Result<bool> file_exists(const char* path) noexcept;

/// @return the size of the regular file @p path; kInvalidArgument for other file types.
[[nodiscard]] Result<std::uint64_t> file_size(const char* path) noexcept;

/// Creates the directory @p path (not its parents) with @p permissions (at most 0xFFF, before the
/// umask; e.g. 0x1ED for octal 0755; checked but without effect on Windows).
/// @return kAlreadyExists if it exists.
Status create_directory(const char* path, std::uint32_t permissions) noexcept;

/// Flushes the directory @p path itself to stable storage, so that files created, renamed or
/// removed in it survive a crash.
Status sync_directory(const char* path) noexcept;

}  // namespace common

#endif  // COMMON_FILE_FILE_HPP
