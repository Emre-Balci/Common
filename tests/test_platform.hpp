/// @file test_platform.hpp
/// @brief What the tests need from the operating system besides the library, on Linux and Windows:
///        anonymous pipes as NativeHandles, the process id and the current directory.

#ifndef COMMON_TEST_PLATFORM_HPP
#define COMMON_TEST_PLATFORM_HPP

#if defined(_WIN32)
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#include <process.h>
#include <windows.h>

#include <filesystem>
#include <system_error>
#else
#include <unistd.h>
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "common/core/config.hpp"

namespace test {

/// The id of this process, as write_file_atomically() puts it into temporary names.
inline std::uint32_t process_id() {
#if defined(_WIN32)
    return static_cast<std::uint32_t>(::_getpid());
#else
    return static_cast<std::uint32_t>(::getpid());
#endif
}

/// Creates an anonymous pipe (on Windows, for synchronous I/O, as File::adopt() needs it).
/// @return true on success.
inline bool create_pipe(common::NativeHandle& read_end, common::NativeHandle& write_end) {
#if defined(_WIN32)
    return ::CreatePipe(&read_end, &write_end, nullptr, 0U) != FALSE;
#else
    std::array<int, 2U> ends{{-1, -1}};
    const bool created{::pipe(ends.data()) == 0};
    read_end = ends[0U];
    write_end = ends[1U];
    return created;
#endif
}

/// One write of @p size bytes. @return true if all of them were written.
inline bool write_native(const common::NativeHandle handle, const char* const data, const std::size_t size) {
#if defined(_WIN32)
    DWORD count{0U};
    return (::WriteFile(handle, data, static_cast<DWORD>(size), &count, nullptr) != FALSE) && (count == size);
#else
    return ::write(handle, data, size) == static_cast<ssize_t>(size);
#endif
}

/// One read of up to @p size bytes. @return the number of bytes read, 0 at the end of the input
/// (including a pipe whose writers have all closed it), -1 on error.
inline std::int64_t read_native(const common::NativeHandle handle, char* const data, const std::size_t size) {
#if defined(_WIN32)
    DWORD count{0U};
    std::int64_t result{-1};
    if (::ReadFile(handle, data, static_cast<DWORD>(size), &count, nullptr) != FALSE) {
        result = std::int64_t{count};
    } else if (::GetLastError() == ERROR_BROKEN_PIPE) {
        result = 0;
    } else {
        // An error: keep -1.
    }
    return result;
#else
    const ssize_t count{::read(handle, data, size)};
    return std::int64_t{count};
#endif
}

/// @return true if the handle was closed without error.
inline bool close_native(const common::NativeHandle handle) {
#if defined(_WIN32)
    return ::CloseHandle(handle) != FALSE;
#else
    return ::close(handle) == 0;
#endif
}

/// The current directory (UTF-8 on Windows), or "" on error.
inline std::string current_directory() {
#if defined(_WIN32)
    std::error_code error{};
    const std::filesystem::path path{std::filesystem::current_path(error)};
    return error ? std::string{} : path.u8string();
#else
    std::array<char, 4096U> buffer{};
    return (::getcwd(buffer.data(), buffer.size()) != nullptr) ? std::string{buffer.data()} : std::string{};
#endif
}

/// The value of the environment variable @p name, or "" if it is not set.
inline std::string environment(const char* const name) {
#if defined(_WIN32)
    std::array<char, 1024U> value{};
    const DWORD length{::GetEnvironmentVariableA(name, value.data(), static_cast<DWORD>(value.size()))};
    return ((length > 0U) && (length < value.size())) ? std::string{value.data(), length} : std::string{};
#else
    const char* const value{std::getenv(name)};
    return (value != nullptr) ? std::string{value} : std::string{};
#endif
}

/// Makes @p path the current directory. @return true on success.
inline bool change_directory(const std::string& path) {
#if defined(_WIN32)
    std::error_code error{};
    std::filesystem::current_path(std::filesystem::u8path(path), error);
    return !error;
#else
    return ::chdir(path.c_str()) == 0;
#endif
}

}  // namespace test

#endif  // COMMON_TEST_PLATFORM_HPP
