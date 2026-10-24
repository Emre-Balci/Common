/// @file whole_file.hpp
/// @brief Reading a small file in one call, and crash-safe replacement of a file's contents.
///
/// read_file() and read_text_file() read a whole file into a caller-provided buffer: configuration
/// and calibration files, or /proc and /sys entries. Those report a size of 0, so these functions
/// read until end of file instead of trusting the size.
///
/// write_file_atomically() replaces a file's contents so that, even after a crash or a power loss,
/// readers find either the complete old or the complete new contents, never a mix. Use it for
/// persisted state such as the last processed sequence number or learned calibration values.
///
/// Every function here blocks in the kernel (storage, page cache, fsync), so none of them is safe
/// on the real-time path: call them during initialisation or on a non-real-time thread. They keep
/// no shared state apart from an atomic counter, so different threads may call them at the same
/// time, each with its own buffer.

#ifndef COMMON_FILE_WHOLE_FILE_HPP
#define COMMON_FILE_WHOLE_FILE_HPP

#include <cstddef>
#include <cstdint>

#include "common/containers/span.hpp"
#include "common/core/config.hpp"
#include "common/core/status.hpp"

namespace common {

/// Reads the whole file @p path into @p buffer.
///
/// Reads until end of file, so files that report a size of 0 (/proc, /sys) work. A file that
/// changes while it is read yields whatever the reads return. Opening a FIFO blocks until a
/// writer opens it.
/// @return the number of bytes read (the contents are the first that many bytes of @p buffer);
///         kOutOfRange if the file holds more than buffer.size() bytes; otherwise File's error
///         codes, e.g. kNotFound, kPermissionDenied, kInvalidArgument (nullptr or a directory).
///         On error, the contents of @p buffer are unspecified.
[[nodiscard]] Result<std::size_t> read_file(const char* path, Span<std::uint8_t> buffer) noexcept;

/// Reads the whole text file @p path into @p buffer and terminates it with a NUL character, so
/// buffer.data() can then be used as a C string.
/// @return the length of the text without the terminator; kOutOfRange if the text and its
///         terminator do not fit (buffer.size() must be at least the file size + 1);
///         kInvalidArgument if the file contains a NUL character, which would silently cut the
///         text (read such files, e.g. /proc/self/cmdline, with read_file()); otherwise the
///         errors of read_file(). On error, the contents of @p buffer are unspecified.
[[nodiscard]] Result<std::size_t> read_text_file(const char* path, Span<char> buffer) noexcept;

/// Creates @p path, or truncates it if it exists, and writes all of @p contents.
///
/// Neither atomic nor durable: a concurrent reader, or any reader after a crash or a failed
/// write, can find the file empty or partly written, and the data may still be only in the page
/// cache when this returns. Use write_file_atomically() for state that must survive a crash.
/// @param permissions permission bits for a newly created file (at most 0xFFF, before the
///        process umask is applied; e.g. 0x1A4 for octal 0644). An existing file keeps its own.
/// @return kOk; kInvalidArgument for nullptr or permissions above 0xFFF; otherwise File's error
///         codes, e.g. kNotFound (missing directory), kResourceLimit (disk full), or the error
///         that closing the file reported.
Status write_file(const char* path, Span<const std::uint8_t> contents, std::uint32_t permissions) noexcept;

/// Replaces the contents of @p path, or creates it, so that even after a crash or a power loss
/// readers find either the complete old or the complete new contents, never a mix.
///
/// It writes @p contents to a new temporary file "<path>.tmp.<pid>.<n>" in the same directory,
/// syncs it (fsync), renames it over @p path, and then syncs the directory so that the rename
/// itself survives a crash. The temporary file is created exclusively, so it never overwrites
/// anything. If its name is taken, e.g. by a file left behind by a crashed process that had the
/// same process id, the next n is tried, up to 8 names. If any step up to the rename fails, the
/// temporary file is removed and @p path is left untouched.
///
/// - @p path must not be empty, must not end with '/', and may be at most 4069 bytes long
///   (PATH_MAX of 4096 minus the longest suffix of 26 bytes and the terminating NUL). The file
///   name plus the suffix must also fit the file system's limit for one name (usually 255 bytes).
/// - The new file gets @p permissions (at most 0xFFF, before the umask), whatever the old file
///   had. A symbolic link at @p path is replaced, not followed. Only the directory has to be
///   writable, not the old file.
/// - A crash during the call can leave a temporary file behind. It never affects @p path and may
///   be deleted.
/// - Threads and processes may replace the same file at the same time: every rename is atomic and
///   the last one wins.
/// @return kOk; kInvalidArgument for an invalid path (including nullptr) or permissions;
///         kNotFound if the directory does not exist; kAlreadyExists if all 8 temporary names
///         were taken; otherwise File's error codes, e.g. kResourceLimit (disk full). If only the
///         final directory sync fails, @p path already has the new contents, but they might not
///         survive a crash; the error is still returned.
Status write_file_atomically(const char* path, Span<const std::uint8_t> contents, std::uint32_t permissions) noexcept;

}  // namespace common

#endif  // COMMON_FILE_WHOLE_FILE_HPP
