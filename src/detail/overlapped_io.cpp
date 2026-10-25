#include "detail/overlapped_io.hpp"

#include <cstddef>
#include <cstdint>

#include "detail/win32.hpp"
#include "detail/win32_status.hpp"

namespace common {
namespace detail {
namespace {

// A wait that times out before the deadline has passed (WaitForSingleObject() counts in milliseconds
// and can end up to a timer tick early) is repeated; the bound only guards against a misbehaving
// driver or a signal storm of spurious wake-ups.
constexpr std::uint32_t kMaxWaits{1000U};

// The longest single wait in milliseconds (INFINITE is 0xFFFFFFFF). Deadline::never() waits in such
// steps, of about 49 days each.
constexpr DWORD kMaxWaitMilliseconds{0xFFFFFFFEU};

constexpr std::int64_t kNanosPerMillisecond{1'000'000};

/// The milliseconds until @p deadline, rounded up so that the wait does not end before it; 0 once it
/// has passed.
DWORD wait_milliseconds(const Deadline& deadline) noexcept {
    DWORD milliseconds{kMaxWaitMilliseconds};
    if (!deadline.is_never()) {
        const std::int64_t nanoseconds{deadline.remaining().count()};
        const std::int64_t rounded_up{(nanoseconds / kNanosPerMillisecond) +
                                      (((nanoseconds % kNanosPerMillisecond) != 0) ? 1 : 0)};
        milliseconds = (rounded_up < std::int64_t{kMaxWaitMilliseconds}) ? static_cast<DWORD>(rounded_up)
                                                                         : kMaxWaitMilliseconds;
    }
    return milliseconds;
}

/// Waits until @p deadline for the overlapped operation that @p overlapped describes to complete,
/// cancels it if the deadline passes first, and collects its result. Once this returns, the operation
/// is over and no longer uses @p overlapped or its buffer.
Result<std::size_t> complete(const HANDLE handle, OVERLAPPED& overlapped, const Deadline& deadline) noexcept {
    // kOk while the operation runs; kTimeout or kSystemError (the wait failed) when it is cancelled.
    Status wait_status{Status::kInterrupted};
    for (std::uint32_t wait{0U}; (wait < kMaxWaits) && (wait_status == Status::kInterrupted); ++wait) {
        const DWORD outcome{::WaitForSingleObject(overlapped.hEvent, wait_milliseconds(deadline))};
        if (outcome == WAIT_OBJECT_0) {
            wait_status = Status::kOk;
        } else if (outcome != WAIT_TIMEOUT) {
            wait_status = Status::kSystemError;
        } else if (deadline.expired()) {
            wait_status = Status::kTimeout;
        } else {
            // Woke before the deadline: wait again.
        }
    }
    if (!is_ok(wait_status)) {
        // Fails with ERROR_NOT_FOUND when the operation completed in the meantime; either way it is
        // over once GetOverlappedResult() returns.
        static_cast<void>(::CancelIoEx(handle, &overlapped));
    }
    DWORD count{0U};
    Result<std::size_t> result{Result<std::size_t>::failure(Status::kSystemError)};
    // TRUE: waits until the operation has completed, which a cancelled one does promptly.
    if (::GetOverlappedResult(handle, &overlapped, &count, TRUE) != FALSE) {
        result = Result<std::size_t>::success(count);
    } else {
        const DWORD error{::GetLastError()};
        // A cancelled write may have transferred part of the data before it was cancelled.
        result = ((!is_ok(wait_status)) && (error == ERROR_OPERATION_ABORTED))
                     ? Result<std::size_t>::success(count)
                     : Result<std::size_t>::failure(transfer_status(error));
    }
    if ((!is_ok(wait_status)) && result.has_value() && (result.value() == 0U)) {
        result = Result<std::size_t>::failure(wait_status);
    }
    return result;
}

}  // namespace

Status transfer_status(const DWORD error) noexcept {
    Status status{Status::kSystemError};
    switch (error) {
        case ERROR_BROKEN_PIPE:         // The other end of a pipe was closed (read).
        case ERROR_NO_DATA:             // Likewise, for a write.
        case ERROR_PIPE_NOT_CONNECTED:  // Likewise, after the other end disconnected.
        case ERROR_DEVICE_NOT_CONNECTED:
        case ERROR_DEVICE_REMOVED:
        case ERROR_DEV_NOT_EXIST:
        case ERROR_NO_SUCH_DEVICE:
        case ERROR_BAD_COMMAND:        // USB serial drivers report an unplugged adapter with one of
        case ERROR_GEN_FAILURE:        // these, depending on the driver.
        case ERROR_ACCESS_DENIED:
        case ERROR_OPERATION_ABORTED:  // The driver aborted the operation; the caller did not cancel it.
            status = Status::kDisconnected;
            break;
        default:
            status = status_from_win32(error);
            break;
    }
    return status;
}

Result<std::size_t> read_overlapped(const HANDLE handle, const HANDLE event, const Span<std::uint8_t> buffer,
                                    const Deadline& deadline) noexcept {
    OVERLAPPED overlapped{};
    overlapped.hEvent = event;
    Result<std::size_t> result{Result<std::size_t>::failure(Status::kSystemError)};
    // ReadFile() resets the event when it starts. It returns TRUE when the read completed at once and
    // FALSE with ERROR_IO_PENDING when it runs on; complete() collects the result in both cases.
    if ((::ReadFile(handle, buffer.data(), transfer_count(buffer.size()), nullptr, &overlapped) != FALSE) ||
        (::GetLastError() == ERROR_IO_PENDING)) {
        result = complete(handle, overlapped, deadline);
    } else {
        result = Result<std::size_t>::failure(transfer_status(::GetLastError()));
    }
    return result;
}

Result<std::size_t> write_overlapped(const HANDLE handle, const HANDLE event, const Span<const std::uint8_t> data,
                                     const Deadline& deadline) noexcept {
    OVERLAPPED overlapped{};
    overlapped.hEvent = event;
    Result<std::size_t> result{Result<std::size_t>::failure(Status::kSystemError)};
    if ((::WriteFile(handle, data.data(), transfer_count(data.size()), nullptr, &overlapped) != FALSE) ||
        (::GetLastError() == ERROR_IO_PENDING)) {
        result = complete(handle, overlapped, deadline);
    } else {
        result = Result<std::size_t>::failure(transfer_status(::GetLastError()));
    }
    return result;
}

}  // namespace detail
}  // namespace common
