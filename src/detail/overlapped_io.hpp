/// @file overlapped_io.hpp
/// @brief Internal (Windows): reads and writes on a handle opened for overlapped I/O, bounded by a
///        Deadline. SerialPort uses them for COM ports; the tests drive them through named pipes.

#ifndef COMMON_SRC_DETAIL_OVERLAPPED_IO_HPP
#define COMMON_SRC_DETAIL_OVERLAPPED_IO_HPP

#include <cstddef>
#include <cstdint>

#include "common/containers/span.hpp"
#include "common/core/status.hpp"
#include "common/time/deadline.hpp"
#include "detail/win32.hpp"

namespace common {
namespace detail {

/// Maps the error of an overlapped read or write: a device that went away (an unplugged USB serial
/// adapter) or a pipe whose other end was closed is kDisconnected; anything else as
/// status_from_win32().
[[nodiscard]] Status transfer_status(DWORD error) noexcept;

/// One overlapped ReadFile() of up to buffer.size() bytes from @p handle, which was opened with
/// FILE_FLAG_OVERLAPPED. It waits until @p deadline for the read to complete and cancels it when the
/// deadline passes. @p event is a manual-reset event that only this call uses while it runs.
/// @return the number of bytes read, which can be 0 when the device completes a read without data;
///         kTimeout if the deadline passed before any byte arrived (an expired deadline still takes
///         bytes that are already there); otherwise transfer_status() of the error.
[[nodiscard]] Result<std::size_t> read_overlapped(HANDLE handle, HANDLE event, Span<std::uint8_t> buffer,
                                                  const Deadline& deadline) noexcept;

/// One overlapped WriteFile() of up to data.size() bytes, waited for and cancelled like
/// read_overlapped().
/// @return the number of bytes written, which may be less than data.size() when the deadline passed
///         during the write; kTimeout if it passed before any byte was written; otherwise
///         transfer_status() of the error.
[[nodiscard]] Result<std::size_t> write_overlapped(HANDLE handle, HANDLE event, Span<const std::uint8_t> data,
                                                   const Deadline& deadline) noexcept;

}  // namespace detail
}  // namespace common

#endif  // COMMON_SRC_DETAIL_OVERLAPPED_IO_HPP
