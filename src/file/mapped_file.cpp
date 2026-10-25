#include "common/file/mapped_file.hpp"

#if !defined(_WIN32)
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "common/containers/span.hpp"
#include "common/core/config.hpp"
#include "common/core/status.hpp"
#include "common/file/file.hpp"

#if defined(_WIN32)
#include "detail/win32.hpp"
#include "detail/win32_status.hpp"
#else
#include "detail/posix_status.hpp"
#endif

namespace common {
namespace {

// Permission bits accepted for new files (octal 07777).
constexpr std::uint32_t kMaxPermissions{0xFFFU};

#if defined(_WIN32)

Status last_error() noexcept {
    return detail::last_win32_error();
}

/// The page protection of the file mapping object for @p mode.
DWORD protection_of(const MapMode mode) noexcept {
    DWORD protection{PAGE_READONLY};
    switch (mode) {
        case MapMode::kReadOnly:
            protection = PAGE_READONLY;
            break;
        case MapMode::kReadWrite:
            protection = PAGE_READWRITE;
            break;
        case MapMode::kCopyOnWrite:
            protection = PAGE_WRITECOPY;
            break;
    }
    return protection;
}

/// The access of the view for @p mode.
DWORD view_access_of(const MapMode mode) noexcept {
    DWORD access{FILE_MAP_READ};
    switch (mode) {
        case MapMode::kReadOnly:
            access = FILE_MAP_READ;
            break;
        case MapMode::kReadWrite:
            access = FILE_MAP_WRITE;  // Reading included.
            break;
        case MapMode::kCopyOnWrite:
            access = FILE_MAP_COPY;
            break;
    }
    return access;
}

/// Starts reading the pages of @p size bytes at @p region in (PrefetchVirtualMemory), in larger
/// pieces than page faults would. A hint: it does not wait for the reads.
Status prefetch(void* const region, const std::size_t size) noexcept {
    WIN32_MEMORY_RANGE_ENTRY range{};
    range.VirtualAddress = region;
    range.NumberOfBytes = size;
    return (::PrefetchVirtualMemory(::GetCurrentProcess(), 1U, &range, 0U) != FALSE) ? Status::kOk : last_error();
}

/// Windows has no MAP_POPULATE: prefetches the view and touches every page of it, so that the pages
/// are resident and mapped. Every page of a kCopyOnWrite view is written with its own value, which
/// gives the view its private copy now rather than at the first write, as population does on Linux.
void populate(void* const region, const std::size_t size, const MapMode mode) noexcept {
    static_cast<void>(prefetch(region, size));
    volatile std::uint8_t* const bytes{static_cast<volatile std::uint8_t*>(region)};
    for (std::size_t offset{0U}; offset < size; offset += kPageSize) {
        const std::uint8_t value{bytes[offset]};
        if (mode == MapMode::kCopyOnWrite) {
            bytes[offset] = value;
        }
    }
}

#else

static_assert(sizeof(off_t) == sizeof(std::int64_t), "a 64-bit off_t is required");

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

#endif

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

/// The alignment of the start of a mapping in the file: the page size on Linux, the allocation
/// granularity (64 KiB) on Windows.
[[nodiscard]] std::uint64_t mapping_alignment() noexcept {
#if defined(_WIN32)
    SYSTEM_INFO info{};
    ::GetSystemInfo(&info);
    return (info.dwAllocationGranularity > 0U) ? std::uint64_t{info.dwAllocationGranularity}
                                               : std::uint64_t{kPageSize};
#else
    const long size{::sysconf(_SC_PAGESIZE)};
    return (size > 0) ? static_cast<std::uint64_t>(size) : std::uint64_t{kPageSize};
#endif
}

/// @return the size of the regular file open at @p descriptor; kInvalidArgument for other file types.
Result<std::uint64_t> regular_file_size(const NativeHandle descriptor) noexcept {
    Result<std::uint64_t> result{Result<std::uint64_t>::failure(Status::kInvalidArgument)};
#if defined(_WIN32)
    BY_HANDLE_FILE_INFORMATION info{};
    if (::GetFileType(descriptor) != FILE_TYPE_DISK) {
        // A pipe or a device: keep kInvalidArgument.
    } else if (::GetFileInformationByHandle(descriptor, &info) == FALSE) {
        result = Result<std::uint64_t>::failure(last_error());
    } else if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U) {
        result = Result<std::uint64_t>::success((std::uint64_t{info.nFileSizeHigh} << 32U) |
                                                std::uint64_t{info.nFileSizeLow});
    } else {
        // A directory: keep kInvalidArgument.
    }
#else
    struct stat info {};
    if (::fstat(descriptor, &info) != 0) {
        result = Result<std::uint64_t>::failure(last_error());
    } else if ((std::uint32_t{info.st_mode} & kFileTypeMask) == kRegularFile) {
        result = Result<std::uint64_t>::success(static_cast<std::uint64_t>(info.st_size));
    } else {
        // Not a regular file: keep kInvalidArgument.
    }
#endif
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

/// Maps @p size bytes of the file open at @p descriptor, from @p start (a multiple of
/// mapping_alignment()), as @p options say, and locks them if asked. Nothing stays mapped on failure.
Status map_region(const NativeHandle descriptor, const MapOptions& options, const std::uint64_t start,
                  const std::size_t size, void*& region) noexcept {
#if defined(_WIN32)
    // The view keeps the file mapping object alive, so its handle closes right away.
    const HANDLE section{::CreateFileMappingW(descriptor, nullptr, protection_of(options.mode), 0U, 0U, nullptr)};
    Status status{(section != nullptr) ? Status::kOk : last_error()};
    if (is_ok(status)) {
        region = ::MapViewOfFile(section, view_access_of(options.mode), static_cast<DWORD>(start >> 32U),
                                 static_cast<DWORD>(start & 0xFFFFFFFFU), size);
        status = (region != nullptr) ? Status::kOk : last_error();
        static_cast<void>(::CloseHandle(section));
    }
    if (is_ok(status) && (options.populate || options.lock)) {
        populate(region, size, options.mode);
    }
    if (is_ok(status) && options.lock && (::VirtualLock(region, size) == FALSE)) {
        status = last_error();
        static_cast<void>(::UnmapViewOfFile(region));
        region = nullptr;
    }
    return status;
#else
    void* const mapped{::mmap(nullptr, size, static_cast<int>(protection_of(options.mode)),
                              static_cast<int>(flags_of(options)), descriptor, static_cast<off_t>(start))};
    Status status{Status::kOk};
    if (mapped == MAP_FAILED) {
        status = last_error();
    } else if (options.lock && (::mlock(mapped, size) != 0)) {
        status = last_error();
        static_cast<void>(::munmap(mapped, size));
    } else {
        region = mapped;
    }
    return status;
#endif
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
#if defined(_WIN32)
    file_ = other.file_;
#endif
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
#if defined(_WIN32)
        file_ = other.file_;
#endif
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
        // The kernel mapping starts at the alignment boundary at or below the offset.
        const std::uint64_t lead{options.offset % mapping_alignment()};
        if (length > (std::uint64_t{std::numeric_limits<std::size_t>::max()} - lead)) {
            status = Status::kOverflow;
        } else {
            const std::size_t region_size{lead + length};
            void* region{nullptr};
            status = map_region(file.descriptor(), options, options.offset - lead, region_size, region);
#if defined(_WIN32)
            // sync() flushes a kReadWrite mapping's file to stable storage, which takes a handle to it.
            HANDLE duplicate{nullptr};
            if (is_ok(status) && (options.mode == MapMode::kReadWrite) &&
                (::DuplicateHandle(::GetCurrentProcess(), file.descriptor(), ::GetCurrentProcess(), &duplicate, 0U,
                                   FALSE, DUPLICATE_SAME_ACCESS) == FALSE)) {
                status = last_error();
                static_cast<void>(::UnmapViewOfFile(region));
            }
#endif
            if (is_ok(status)) {
                region_ = region;
                region_size_ = region_size;
                view_ = Span<std::uint8_t>{static_cast<std::uint8_t*>(region), region_size}.subspan(lead, length);
                mode_ = options.mode;
                mapped_ = true;
#if defined(_WIN32)
                file_ = duplicate;
#endif
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
#if defined(_WIN32)
            status = (::UnmapViewOfFile(region_) != FALSE) ? Status::kOk : last_error();
#else
            status = (::munmap(region_, region_size_) == 0) ? Status::kOk : last_error();
#endif
        }
        if (is_ok(status)) {
#if defined(_WIN32)
            if (file_ != nullptr) {
                static_cast<void>(::CloseHandle(file_));
            }
#endif
            reset();
        }
    }
    return status;
}

// Not const: msync(2) writes the mapping back to the file.
Status MappedFile::sync() noexcept {  // NOLINT(readability-make-member-function-const)
    Status status{mapped_ ? Status::kOk : Status::kInvalidArgument};
    if (is_ok(status) && (mode_ == MapMode::kReadWrite) && (region_ != nullptr)) {
#if defined(_WIN32)
        // FlushViewOfFile() writes the modified pages to the file; FlushFileBuffers() then waits until
        // they and the file's metadata are on stable storage.
        status = (::FlushViewOfFile(region_, region_size_) != FALSE) ? Status::kOk : last_error();
        if (is_ok(status)) {
            status = (::FlushFileBuffers(file_) != FALSE) ? Status::kOk : last_error();
        }
#else
        status = (::msync(region_, region_size_, MS_SYNC) == 0) ? Status::kOk : last_error();
#endif
    }
    return status;
}

// Not const: it changes whether the pages of the mapping may be evicted.
Status MappedFile::lock() noexcept {  // NOLINT(readability-make-member-function-const)
    Status status{mapped_ ? Status::kOk : Status::kInvalidArgument};
    if (is_ok(status) && (region_ != nullptr)) {
#if defined(_WIN32)
        status = (::VirtualLock(region_, region_size_) != FALSE) ? Status::kOk : last_error();
#else
        status = (::mlock(region_, region_size_) == 0) ? Status::kOk : last_error();
#endif
    }
    return status;
}

// Not const: it changes whether the pages of the mapping may be evicted.
Status MappedFile::unlock() noexcept {  // NOLINT(readability-make-member-function-const)
    Status status{mapped_ ? Status::kOk : Status::kInvalidArgument};
    if (is_ok(status) && (region_ != nullptr)) {
#if defined(_WIN32)
        // Pages that are not locked are no error, as with munlock(2).
        status = ((::VirtualUnlock(region_, region_size_) != FALSE) || (::GetLastError() == ERROR_NOT_LOCKED))
                     ? Status::kOk
                     : last_error();
#else
        status = (::munlock(region_, region_size_) == 0) ? Status::kOk : last_error();
#endif
    }
    return status;
}

// Not const: it changes how the kernel pages the mapping in and out.
Status MappedFile::advise(const Advice advice) noexcept {  // NOLINT(readability-make-member-function-const)
#if defined(_WIN32)
    bool valid{false};
    switch (advice) {
        case Advice::kNormal:
        case Advice::kSequential:
        case Advice::kRandom:
        case Advice::kWillNeed:
            valid = true;
            break;
    }
    Status status{(mapped_ && valid) ? Status::kOk : Status::kInvalidArgument};
    // Windows has an equivalent of kWillNeed only; the other advice changes nothing there.
    if (is_ok(status) && (region_ != nullptr) && (advice == Advice::kWillNeed)) {
        status = prefetch(region_, region_size_);
    }
#else
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
#endif
    return status;
}

void MappedFile::reset() noexcept {
    region_ = nullptr;
    region_size_ = 0U;
    view_ = Span<std::uint8_t>{};
    mode_ = MapMode::kReadOnly;
    mapped_ = false;
#if defined(_WIN32)
    file_ = nullptr;
#endif
}

}  // namespace common
