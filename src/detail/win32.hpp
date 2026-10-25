/// @file win32.hpp
/// @brief Internal (Windows): <windows.h> without its min/max macros, and UTF-8 paths for the
///        wide-character Windows API.

#ifndef COMMON_SRC_DETAIL_WIN32_HPP
#define COMMON_SRC_DETAIL_WIN32_HPP

#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "common/core/status.hpp"

namespace common {
namespace detail {

// ReadFile() and WriteFile() take a DWORD count; like Linux, transfer at most 0x7ffff000 bytes per call.
constexpr std::size_t kMaxTransfer{0x7FFFF000U};

/// The count to pass to ReadFile() or WriteFile() for a transfer of @p requested bytes.
[[nodiscard]] inline DWORD transfer_count(const std::size_t requested) noexcept {
    return static_cast<DWORD>((requested < kMaxTransfer) ? requested : kMaxTransfer);
}

/// A path as the wide-character ("W") functions of the Windows API take it: the UTF-8 text that the
/// library's functions receive, converted to NUL-terminated UTF-16. The "A" functions would read the
/// bytes in the process code page instead.
class WidePath final {
public:
    /// Room for the converted path and its NUL, the same limit as PATH_MAX on Linux. Windows accepts
    /// paths longer than MAX_PATH (260) only where long paths are enabled.
    static constexpr std::size_t kCapacity{4096U};

    /// Converts @p path, with @p prefix (e.g. L"\\\\.\\" for a device name) in front of it.
    /// @return kInvalidArgument for nullptr, text that is not UTF-8, or a path that does not fit.
    [[nodiscard]] Status assign(const char* const path, const wchar_t* const prefix = L"") noexcept {
        Status status{((path != nullptr) && (prefix != nullptr)) ? Status::kOk : Status::kInvalidArgument};
        std::size_t length{0U};
        while (is_ok(status) && (length < kCapacity) && (prefix[length] != L'\0')) {
            text_[length] = prefix[length];
            ++length;
        }
        if (is_ok(status) && (length == kCapacity)) {
            status = Status::kInvalidArgument;  // No room left for the path.
        }
        if (is_ok(status)) {
            // A length of -1 converts up to and including the NUL; 0 means failure (invalid UTF-8,
            // or the result does not fit).
            const int converted{::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, &text_[length],
                                                      static_cast<int>(kCapacity - length))};
            status = (converted > 0) ? Status::kOk : Status::kInvalidArgument;
        }
        return status;
    }

    /// The converted path; empty before a successful assign().
    [[nodiscard]] const wchar_t* c_str() const noexcept {
        return text_.data();
    }

private:
    std::array<wchar_t, kCapacity> text_{};
};

/// A LARGE_INTEGER holding @p value, for the file position and size functions.
[[nodiscard]] inline LARGE_INTEGER large_integer(const std::int64_t value) noexcept {
    LARGE_INTEGER result{};
    result.QuadPart = value;
    return result;
}

}  // namespace detail
}  // namespace common

#endif  // COMMON_SRC_DETAIL_WIN32_HPP
