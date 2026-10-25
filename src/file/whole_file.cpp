#include "common/file/whole_file.hpp"

#if defined(_WIN32)
#include <process.h>  // _getpid
#else
#include <unistd.h>
#endif

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "common/containers/span.hpp"
#include "common/core/status.hpp"
#include "common/file/file.hpp"

namespace common {
namespace {

// Permission bits accepted for new files (octal 07777).
constexpr std::uint32_t kMaxPermissions{0xFFFU};

// PATH_MAX on Linux, including the terminating NUL.
constexpr std::size_t kPathBufferSize{4096U};

// ".tmp." + up to 10 digits of the process id + "." + up to 10 digits of the counter.
constexpr std::size_t kMaxSuffixLength{26U};

// The longest path write_file_atomically() accepts, so that path + suffix + NUL fit kPathBufferSize.
constexpr std::size_t kMaxAtomicPathLength{kPathBufferSize - kMaxSuffixLength - 1U};

// Temporary names tried before write_file_atomically() gives up with kAlreadyExists.
constexpr std::uint32_t kMaxTemporaryNames{8U};

// Digits are written through this table, so the numerical value of a character is never used
// (MISRA C++:2023 Rule 7.0.3).
constexpr std::array<char, 10U> kDigitChars{{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'}};

// Distinguishes the temporary files of concurrent calls in one process; the process id
// distinguishes processes. The only state shared between calls.
std::atomic<std::uint32_t> g_temporary_counter{0U};  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

// read_text_file() reads chars through a byte view, which is valid because std::uint8_t is unsigned
// char, which may access the bytes of any object.
static_assert(std::is_same<std::uint8_t, unsigned char>::value, "std::uint8_t must be unsigned char");

/// Reads @p file from its current position to the end into @p buffer.
/// @return the number of bytes read; kOutOfRange if the file holds more than buffer.size() bytes.
Result<std::size_t> read_to_end(File& file, const Span<std::uint8_t> buffer) noexcept {
    Status status{Status::kOk};
    std::size_t total{0U};
    bool end_of_file{false};
    // Each round reads at least one byte, or reaches the end, or fails. buffer.size() rounds fill the
    // buffer at most, and one more finds the end or the byte that does not fit.
    for (std::size_t round{0U}; is_ok(status) && (!end_of_file) && (round <= buffer.size()); ++round) {
        std::array<std::uint8_t, 1U> probe{};
        // Once the buffer is full, a one-byte read tells whether the file goes on.
        const bool full{total == buffer.size()};
        const Result<std::size_t> count{full ? file.read(Span<std::uint8_t>{probe}) : file.read(buffer.subspan(total))};
        if (!count.has_value()) {
            status = count.status();
        } else if (count.value() == 0U) {
            end_of_file = true;
        } else if (full) {
            status = Status::kOutOfRange;
        } else {
            total += count.value();
        }
    }
    return is_ok(status) ? Result<std::size_t>::success(total) : Result<std::size_t>::failure(status);
}

/// @return true if @p character separates the components of a path: '/', and on Windows also '\\'.
[[nodiscard]] bool is_separator(const char character) noexcept {
#if defined(_WIN32)
    return (character == '/') || (character == '\\');
#else
    return character == '/';
#endif
}

/// @return true if @p path starts with a drive letter and a colon, as in "C:\\data" or "C:data"
///         (Windows only).
[[nodiscard]] bool has_drive(const Span<const char> path) noexcept {
#if defined(_WIN32)
    return (path.size() >= 2U) && (path[1U] == ':');
#else
    static_cast<void>(path);
    return false;
#endif
}

/// Appends the decimal digits of @p value to @p out at @p position.
/// @pre out has room for 10 more characters after position
void append_decimal(const Span<char> out, std::size_t& position, const std::uint32_t value) noexcept {
    std::array<char, 10U> reversed{};
    std::uint32_t rest{value};
    std::size_t count{0U};
    do {
        reversed[count] = kDigitChars[rest % 10U];
        rest /= 10U;
        ++count;
    } while ((rest != 0U) && (count < reversed.size()));
    for (std::size_t index{count}; index > 0U; --index) {
        out[position] = reversed[index - 1U];
        ++position;
    }
}

/// Copies @p text to @p out at @p position. @pre out has room for it after position
void append_text(const Span<char> out, std::size_t& position, const Span<const char> text) noexcept {
    for (const char character : text) {
        out[position] = character;
        ++position;
    }
}

/// @return the length of @p path, if it is a valid argument of write_file_atomically(): not
///         nullptr, not empty, at most kMaxAtomicPathLength bytes, and not ending with a separator.
///         kInvalidArgument otherwise. Reads at most kMaxAtomicPathLength + 1 characters.
Result<std::size_t> atomic_path_length(const char* const path) noexcept {
    Result<std::size_t> result{Result<std::size_t>::failure(Status::kInvalidArgument)};
    if (path != nullptr) {
        std::size_t length{0U};
        while ((length <= kMaxAtomicPathLength) && (path[length] != '\0')) {
            ++length;
        }
        if ((length > 0U) && (length <= kMaxAtomicPathLength) && (!is_separator(path[length - 1U]))) {
            result = Result<std::size_t>::success(length);
        }
    }
    return result;
}

/// Writes "<path>.tmp.<pid>.<counter>" and a NUL into @p out.
void temporary_name(const Span<const char> path, const std::uint32_t counter,
                    const Span<char> out) noexcept {
    std::size_t position{0U};
    append_text(out, position, path);
    append_text(out, position, Span<const char>{".tmp.", 5U});
    // getpid() cannot fail and returns a positive pid_t; so does _getpid() on Windows.
#if defined(_WIN32)
    append_decimal(out, position, static_cast<std::uint32_t>(::_getpid()));
#else
    append_decimal(out, position, static_cast<std::uint32_t>(::getpid()));
#endif
    out[position] = '.';
    ++position;
    append_decimal(out, position, counter);
    out[position] = '\0';
}

/// Writes the directory part of @p path and a NUL into @p out: "." for a bare file name, "/" for a
/// file in the root directory. On Windows, a root directory keeps its drive ("C:\\" for "C:\\file"),
/// and the directory of "C:file" is "C:", the current directory of drive C.
void parent_directory(const Span<const char> path, const Span<char> out) noexcept {
    std::size_t slash{path.size()};  // path.size(): no separator.
    for (std::size_t index{path.size()}; index > 0U; --index) {
        if (is_separator(path[index - 1U])) {
            slash = index - 1U;
            break;
        }
    }
    std::size_t position{0U};
    if ((slash == path.size()) && has_drive(path)) {
        append_text(out, position, path.first(2U));
    } else if (slash == path.size()) {
        out[position] = '.';
        ++position;
    } else if ((slash == 0U) || ((slash == 2U) && has_drive(path))) {
        append_text(out, position, path.first(slash + 1U));  // The root directory, with its separator.
    } else {
        append_text(out, position, path.first(slash));
    }
    out[position] = '\0';
}

/// Creates a new temporary file next to @p path, exclusively, and opens it in @p file for writing.
/// Its NUL-terminated name is left in @p name.
/// @return kAlreadyExists if all kMaxTemporaryNames names were taken, or the File::open() error.
Status create_temporary_file(const Span<const char> path, const std::uint32_t permissions, File& file,
                             const Span<char> name) noexcept {
    FileOptions options{};
    options.access = FileAccess::kWriteOnly;
    options.creation = FileCreation::kCreateNew;
    options.permissions = permissions;
    Status status{Status::kAlreadyExists};
    for (std::uint32_t attempt{0U}; (attempt < kMaxTemporaryNames) && (status == Status::kAlreadyExists);
         ++attempt) {
        temporary_name(path, g_temporary_counter.fetch_add(1U, std::memory_order_relaxed), name);
        status = file.open(name.data(), options);
    }
    return status;
}

}  // namespace

Result<std::size_t> read_file(const char* const path, const Span<std::uint8_t> buffer) noexcept {
    Result<std::size_t> result{Result<std::size_t>::failure(Status::kInvalidArgument)};
    if (path != nullptr) {
        File file{};
        const Status opened{file.open(path, FileOptions{})};
        // A directory opens for reading; reading it then fails with kInvalidArgument (EISDIR).
        result = is_ok(opened) ? read_to_end(file, buffer) : Result<std::size_t>::failure(opened);
        // Nothing was written, so closing cannot lose data: its result does not matter.
        static_cast<void>(file.close());
    }
    return result;
}

Result<std::size_t> read_text_file(const char* const path, const Span<char> buffer) noexcept {
    Result<std::size_t> result{Result<std::size_t>::failure(Status::kInvalidArgument)};
    if ((path != nullptr) && buffer.empty()) {
        result = Result<std::size_t>::failure(Status::kOutOfRange);  // Not even the terminator fits.
    } else if (path != nullptr) {
        // The last character is kept for the terminator. A pointer to unsigned char may access the
        // bytes of any object (exception to MISRA C++:2023 Rule 8.2.5).
        const Span<std::uint8_t> bytes{reinterpret_cast<std::uint8_t*>(buffer.data()), buffer.size() - 1U};
        result = read_file(path, bytes);
        if (result.has_value()) {
            const Span<const char> text{buffer.first(result.value())};
            bool has_nul{false};
            for (const char character : text) {
                has_nul = has_nul || (character == '\0');
            }
            if (has_nul) {
                result = Result<std::size_t>::failure(Status::kInvalidArgument);
            } else {
                buffer[result.value()] = '\0';
            }
        }
    } else {
        // nullptr: keep kInvalidArgument.
    }
    return result;
}

Status write_file(const char* const path, const Span<const std::uint8_t> contents,
                  const std::uint32_t permissions) noexcept {
    Status status{((path != nullptr) && (permissions <= kMaxPermissions)) ? Status::kOk : Status::kInvalidArgument};
    File file{};
    if (is_ok(status)) {
        FileOptions options{};
        options.access = FileAccess::kWriteOnly;
        options.creation = FileCreation::kCreateOrTruncate;
        options.permissions = permissions;
        status = file.open(path, options);
    }
    if (is_ok(status)) {
        status = file.write_all(contents);
    }
    // Closing can report a deferred write error (e.g. of a network file system).
    const Status closed{file.close()};
    return is_ok(status) ? closed : status;
}

Status write_file_atomically(const char* const path, const Span<const std::uint8_t> contents,
                             const std::uint32_t permissions) noexcept {
    const Result<std::size_t> length{atomic_path_length(path)};
    Status status{(length.has_value() && (permissions <= kMaxPermissions)) ? Status::kOk : Status::kInvalidArgument};
    std::array<char, kPathBufferSize> temporary{};
    File file{};
    if (is_ok(status)) {
        status = create_temporary_file(Span<const char>{path, length.value()}, permissions, file, temporary);
    }
    const bool created{is_ok(status)};
    if (is_ok(status)) {
        status = file.write_all(contents);
    }
    if (is_ok(status)) {
        status = file.sync();  // The data must be on storage before the rename makes it visible.
    }
    const Status closed{file.close()};
    status = is_ok(status) ? closed : status;
    if (is_ok(status)) {
        status = rename_file(temporary.data(), path);
    }
    if (created && (!is_ok(status))) {
        // The rename did not happen, so path is untouched; the temporary file is not needed.
        static_cast<void>(remove_file(temporary.data()));
    } else if (is_ok(status)) {
        // Makes the rename itself durable.
        std::array<char, kPathBufferSize> directory{};
        parent_directory(Span<const char>{path, length.value()}, directory);
        status = sync_directory(directory.data());
    } else {
        // Nothing was created.
    }
    return status;
}

}  // namespace common
