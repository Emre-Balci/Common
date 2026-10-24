#include "common/file/mapped_file.hpp"

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "common/containers/span.hpp"
#include "common/core/config.hpp"
#include "common/core/status.hpp"
#include "common/file/file.hpp"
#include "detail/posix_status.hpp"

namespace common {
namespace {

static_assert(sizeof(off_t) == sizeof(std::int64_t), "a 64-bit off_t is required");

// Permission bits accepted for new files (octal 07777).
constexpr std::uint32_t kMaxPermissions{0xFFFU};

// POSIX defines the mmap(2) flags and file-type bits as int; flags are combined as unsigned values
// and converted once at the call.
constexpr std::uint32_t to_bits(const int flag) noexcept {
    return static_cast<std::uint32_t>(flag);
}

constexpr std::uint32_t kFileTypeMask{to_bits(S_IFMT)};
constexpr std::uint32_t kRegularFile{to_bits(S_IFREG)};

/// Maps errno to a Status, with the meanings errors have for memory mapping.
Status mapping_status(const int error) noexcept {
    // ENODEV: the file system does not support memory mapping.
    return (error == ENODEV) ? Status::kNotSupported : detail::status_from_errno(error);
}

Status last_error() noexcept {
    return mapping_status(errno);
}

[[nodiscard]] bool is_valid(const MapMode mode) noexcept {
    bool valid{false};
    switch (mode) {
        case MapMode::kReadOnly:
        case MapMode::kReadWrite:
        case MapMode::kCopyOnWrite:
            valid = true;
            break;
    }
    return valid;  // false for a value outside the enumeration.
}

/// The size of the pages the kernel maps; mappings start at a multiple of it.
[[nodiscard]] std::uint64_t page_size() noexcept {
    const long size{::sysconf(_SC_PAGESIZE)};
    return (size > 0) ? static_cast<std::uint64_t>(size) : std::uint64_t{kPageSize};
}

/// @return the size of the regular file open at @p descriptor; kInvalidArgument for other file types.
Result<std::uint64_t> regular_file_size(const std::int32_t descriptor) noexcept {
    Result<std::uint64_t> result{Result<std::uint64_t>::failure(Status::kInvalidArgument)};
    struct stat info {};
    if (::fstat(descriptor, &info) != 0) {
        result = Result<std::uint64_t>::failure(last_error());
    } else if ((std::uint32_t{info.st_mode} & kFileTypeMask) == kRegularFile) {
        result = Result<std::uint64_t>::success(static_cast<std::uint64_t>(info.st_size));
    } else {
        // Not a regular file: keep kInvalidArgument.
    }
    return result;
}

/// @return the number of bytes @p options select in a file of @p file_size bytes; kOutOfRange if
///         the range does not lie inside the file.
Result<std::uint64_t> range_length(const MapOptions& options, const std::uint64_t file_size) noexcept {
    Result<std::uint64_t> result{Result<std::uint64_t>::failure(Status::kOutOfRange)};
    if (options.offset <= file_size) {
        const std::uint64_t available{file_size - options.offset};
        if (options.length == 0U) {
            result = Result<std::uint64_t>::success(available);
        } else if (options.length <= available) {
            result = Result<std::uint64_t>::success(options.length);
        } else {
            // Beyond the end of the file: keep kOutOfRange.
        }
    }
    return result;
}

std::uint32_t protection_of(const MapMode mode) noexcept {
    return (mode == MapMode::kReadOnly) ? to_bits(PROT_READ) : (to_bits(PROT_READ) | to_bits(PROT_WRITE));
}

std::uint32_t flags_of(const MapOptions& options) noexcept {
    std::uint32_t flags{(options.mode == MapMode::kCopyOnWrite) ? to_bits(MAP_PRIVATE) : to_bits(MAP_SHARED)};
    if (options.populate) {
        flags |= to_bits(MAP_POPULATE);
    }
    return flags;
}

}  // namespace

MappedFile::~MappedFile() {
    static_cast<void>(unmap());
}

MappedFile::MappedFile(MappedFile&& other) noexcept
    : region_{other.region_},
      region_size_{other.region_size_},
      view_{other.view_},
      mode_{other.mode_},
      mapped_{other.mapped_} {
    other.reset();
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        static_cast<void>(unmap());
        region_ = other.region_;
        region_size_ = other.region_size_;
        view_ = other.view_;
        mode_ = other.mode_;
        mapped_ = other.mapped_;
        other.reset();
    }
    return *this;
}

Status MappedFile::map(const char* const path, const MapOptions& options) noexcept {
    Status status{((path != nullptr) && (!mapped_) && is_valid(options.mode)) ? Status::kOk : Status::kInvalidArgument};
    File file{};
    if (is_ok(status)) {
        FileOptions file_options{};
        file_options.access = (options.mode == MapMode::kReadWrite) ? FileAccess::kReadWrite : FileAccess::kReadOnly;
        status = file.open(path, file_options);
    }
    if (is_ok(status)) {
        status = map(file, options);
    }
    // The mapping does not need the descriptor, which closes here; nothing was written through it.
    return status;
}

Status MappedFile::map(const File& file, const MapOptions& options) noexcept {
    Status status{(file.is_open() && (!mapped_) && is_valid(options.mode)) ? Status::kOk : Status::kInvalidArgument};
    std::uint64_t length{0U};
    if (is_ok(status)) {
        const Result<std::uint64_t> file_size{regular_file_size(file.descriptor())};
        const Result<std::uint64_t> range{file_size.has_value() ? range_length(options, file_size.value())
                                                                : file_size};
        status = range.status();
        length = range.value_or(0U);
    }
    if (is_ok(status) && (length == 0U)) {
        // mmap(2) rejects length 0: an empty range is a valid mapping of nothing.
        mode_ = options.mode;
        mapped_ = true;
    } else if (is_ok(status)) {
        // The kernel mapping starts at the page boundary at or below the offset.
        const std::uint64_t page{page_size()};
        const std::uint64_t lead{options.offset % page};
        if (length > (std::uint64_t{std::numeric_limits<std::size_t>::max()} - lead)) {
            status = Status::kOverflow;
        } else {
            const std::size_t region_size{lead + length};
            void* const region{::mmap(nullptr, region_size, static_cast<int>(protection_of(options.mode)),
                                      static_cast<int>(flags_of(options)), file.descriptor(),
                                      static_cast<off_t>(options.offset - lead))};
            if (region == MAP_FAILED) {
                status = last_error();
            } else if (options.lock && (::mlock(region, region_size) != 0)) {
                status = last_error();
                static_cast<void>(::munmap(region, region_size));  // Nothing stays mapped on failure.
            } else {
                region_ = region;
                region_size_ = region_size;
                view_ = Span<std::uint8_t>{static_cast<std::uint8_t*>(region), region_size}.subspan(lead, length);
                mode_ = options.mode;
                mapped_ = true;
            }
        }
    } else {
        // Invalid arguments, or the file cannot be mapped: status says why.
    }
    return status;
}

Status MappedFile::create(const char* const path, const std::uint64_t size, const std::uint32_t permissions) noexcept {
    Status status{((path != nullptr) && (permissions <= kMaxPermissions) && (!mapped_)) ? Status::kOk
                                                                                       : Status::kInvalidArgument};
    File file{};
    if (is_ok(status)) {
        FileOptions file_options{};
        file_options.access = FileAccess::kReadWrite;
        file_options.creation = FileCreation::kCreateOrOpen;
        file_options.permissions = permissions;
        status = file.open(path, file_options);
    }
    if (is_ok(status)) {
        status = file.truncate(size);
    }
    if (is_ok(status)) {
        // Where the file system cannot reserve storage, the file is still usable.
        const Status reserved{file.preallocate(size)};
        status = (reserved == Status::kNotSupported) ? Status::kOk : reserved;
    }
    if (is_ok(status)) {
        MapOptions options{};
        options.mode = MapMode::kReadWrite;
        options.populate = true;
        status = map(file, options);
    }
    return status;
}

Status MappedFile::unmap() noexcept {
    Status status{Status::kOk};
    if (mapped_) {
        if (region_ != nullptr) {
            status = (::munmap(region_, region_size_) == 0) ? Status::kOk : last_error();
        }
        if (is_ok(status)) {
            reset();
        }
    }
    return status;
}

// Not const: msync(2) writes the mapping back to the file.
Status MappedFile::sync() noexcept {  // NOLINT(readability-make-member-function-const)
    Status status{mapped_ ? Status::kOk : Status::kInvalidArgument};
    if (is_ok(status) && (mode_ == MapMode::kReadWrite) && (region_ != nullptr)) {
        status = (::msync(region_, region_size_, MS_SYNC) == 0) ? Status::kOk : last_error();
    }
    return status;
}

// Not const: it changes whether the pages of the mapping may be evicted.
Status MappedFile::lock() noexcept {  // NOLINT(readability-make-member-function-const)
    Status status{mapped_ ? Status::kOk : Status::kInvalidArgument};
    if (is_ok(status) && (region_ != nullptr)) {
        status = (::mlock(region_, region_size_) == 0) ? Status::kOk : last_error();
    }
    return status;
}

// Not const: it changes whether the pages of the mapping may be evicted.
Status MappedFile::unlock() noexcept {  // NOLINT(readability-make-member-function-const)
    Status status{mapped_ ? Status::kOk : Status::kInvalidArgument};
    if (is_ok(status) && (region_ != nullptr)) {
        status = (::munlock(region_, region_size_) == 0) ? Status::kOk : last_error();
    }
    return status;
}

// Not const: it changes how the kernel pages the mapping in and out.
Status MappedFile::advise(const Advice advice) noexcept {  // NOLINT(readability-make-member-function-const)
    int value{MADV_NORMAL};
    bool valid{false};
    switch (advice) {
        case Advice::kNormal:
            value = MADV_NORMAL;
            valid = true;
            break;
        case Advice::kSequential:
            value = MADV_SEQUENTIAL;
            valid = true;
            break;
        case Advice::kRandom:
            value = MADV_RANDOM;
            valid = true;
            break;
        case Advice::kWillNeed:
            value = MADV_WILLNEED;
            valid = true;
            break;
    }
    Status status{(mapped_ && valid) ? Status::kOk : Status::kInvalidArgument};
    if (is_ok(status) && (region_ != nullptr)) {
        status = (::madvise(region_, region_size_, value) == 0) ? Status::kOk : last_error();
    }
    return status;
}

void MappedFile::reset() noexcept {
    region_ = nullptr;
    region_size_ = 0U;
    view_ = Span<std::uint8_t>{};
    mode_ = MapMode::kReadOnly;
    mapped_ = false;
}

}  // namespace common
