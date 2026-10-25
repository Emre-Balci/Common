/// @file mapped_file.hpp
/// @brief Memory mapping of a file, set up during initialisation so that the real-time phase can
///        read (and, with care, write) its contents without system calls.
///
/// Typical uses are reference data (instrument definitions, calibration tables) and a persistent
/// state block. map(), create(), unmap(), sync(), lock(), unlock() and advise() make system calls
/// that may block (storage I/O, memory allocation, the address-space lock): call them during
/// initialisation or on a non-real-time thread. The rest of this comment describes Linux.
///
/// What the real-time phase may do:
/// - Read bytes() of a mapping that is populated and locked (MapOptions::populate and
///   MapOptions::lock, or lock() later). Population reads the range in and fills the page tables;
///   locking keeps the pages resident, so reads do not page-fault. Without the lock, the kernel
///   may evict clean file pages under memory pressure, and the next read waits for storage (a
///   major fault). Page migration (memory compaction, NUMA balancing) can still make an access to
///   a locked page wait briefly; real-time systems turn it off (vm.compact_unevictable_allowed=0,
///   the default on PREEMPT_RT, and kernel.numa_balancing=0).
/// - Write writable_bytes() of a populated, locked kCopyOnWrite mapping. Population (and locking)
///   breaks copy-on-write for every page at once, so the mapping is private memory afterwards and
///   writes do not fault.
///
/// What it must not do:
/// - Write a kReadWrite mapping when a deadline matters. To track dirty pages, the kernel maps the
///   pages of a file on a disk-based file system read-only: the first write to a page takes a
///   fault, and writeback (in the background, or by sync()) write-protects the page again, so a
///   later write faults again. That fault may wait for the file system (block allocation, the
///   journal), for writeback of the page, or for dirty-page throttling, and locking prevents none
///   of it. Real-time writers use kCopyOnWrite or anonymous memory, and leave persisting the data
///   to a non-real-time thread. (Files on a memory-based file system such as tmpfs or /dev/shm are
///   not written back.)
/// - Call sync(): it waits for storage.
///
/// The mapping and the file:
/// - kReadOnly and kReadWrite share the page cache with every other user of the file, so they show
///   changes other processes make to it at any time, without synchronisation. kCopyOnWrite shows
///   them too in pages it has not copied yet; after population it holds a private copy of every
///   page. To replace reference data, write a new file and rename_file() it over the old one:
///   existing mappings keep the old contents.
/// - If the file is truncated while it is mapped (by any process), an access to a page that lies
///   wholly beyond the new end of the file raises SIGBUS, in every mode, even when the page was
///   locked (Linux also discards private copies of such pages). An I/O error while a page is read
///   in raises SIGBUS too, and so does the first write to a page of a kReadWrite mapping that needs
///   a block the file system cannot allocate (a sparse file on a full disk); create() reserves the
///   blocks up front to prevent that.
/// - A mapping does not need the descriptor it was made from: it stays valid after the File is
///   closed, and after the file is renamed or removed.
///
/// On Windows, the same holds with these differences:
/// - A mapping starts at a multiple of the allocation granularity (64 KiB) rather than of the page
///   size; bytes() still starts exactly at MapOptions::offset.
/// - Population (MapOptions::populate, and locking) prefetches the pages and touches every one of
///   them; a kCopyOnWrite mapping gets every page written with its own value, which gives it the
///   private copy right away, as on Linux.
/// - lock() is VirtualLock(), which the process's minimum working-set size limits (kResourceLimit
///   beyond it; SetProcessWorkingSetSize() raises it). advise() acts on Advice::kWillNeed only.
/// - A file cannot be truncated while it is mapped: the truncation fails, rather than accesses to the
///   pages beyond the end raising SIGBUS. An I/O error while a page is read in raises the structured
///   exception EXCEPTION_IN_PAGE_ERROR instead of SIGBUS.
/// - A kReadWrite mapping keeps a handle to its file (shared like any File) for sync().

#ifndef COMMON_FILE_MAPPED_FILE_HPP
#define COMMON_FILE_MAPPED_FILE_HPP

#include <cstddef>
#include <cstdint>

#include "common/containers/span.hpp"
#include "common/core/config.hpp"
#include "common/core/status.hpp"
#include "common/file/file.hpp"

namespace common {

/// Access to the mapped bytes, and where writes go.
enum class MapMode : std::uint8_t {
    kReadOnly,     ///< Read-only (PROT_READ, MAP_SHARED). Needs a descriptor open for reading.
    kReadWrite,    ///< Writes reach the file (MAP_SHARED). Needs a descriptor open for reading and writing.
    kCopyOnWrite,  ///< Writable; changes stay private to the mapping (MAP_PRIVATE). Needs read access.
};

/// What MappedFile::map() maps, and how.
struct MapOptions final {
    MapMode mode{MapMode::kReadOnly};
    /// Reads the range in and fills the page tables when mapping (MAP_POPULATE), so that the first
    /// access does not page-fault. Best effort: mmap(2) does not report a failure to populate.
    bool populate{true};
    /// Locks the pages of the range in RAM (mlock(2)), which also populates them. If that is not
    /// allowed, map() fails and nothing stays mapped: kResourceLimit if RLIMIT_MEMLOCK is too
    /// small, kPermissionDenied if it is 0 and the process lacks CAP_IPC_LOCK.
    bool lock{false};
    /// First byte of the file to map. It need not be a multiple of the page size: the kernel
    /// mapping then starts at the page boundary below it, and bytes() starts exactly here.
    std::uint64_t offset{0U};
    /// Number of bytes to map; 0 maps everything from offset to the end of the file.
    /// offset + length must not exceed the file size (kOutOfRange otherwise).
    std::uint64_t length{0U};
};

/// Owns one memory mapping of a range of a regular file and unmaps it on destruction. Move-only.
///
/// An empty file or a zero-length range gives a valid, empty mapping (is_mapped() is true, bytes()
/// is empty) without calling mmap(2), which rejects length 0. Operations other than unmap() on an
/// object that holds no mapping return kInvalidArgument.
///
/// Error mapping: kNotFound, kPermissionDenied (the descriptor's access mode does not allow the
/// mapping, a read-only file system, a file seal), kResourceLimit (memory, address space, the
/// locked-memory limit), kOutOfRange (the range is not inside the file), kOverflow (the range does
/// not fit in the address space), kNotSupported (the file system cannot map files),
/// kInvalidArgument, kSystemError (I/O error and anything else).
///
/// Thread safety: map, move and unmap a MappedFile on one thread. While it stays mapped, any
/// number of threads may read bytes() concurrently. Concurrent accesses to the same bytes where one
/// of them writes are a data race: writers need the caller's synchronisation (e.g. a SeqLock).
/// Spans from bytes() and writable_bytes() stay valid while the mapping exists, also across a move
/// of the MappedFile, and dangle after unmap() or destruction.
class MappedFile final {
public:
    /// Expected access pattern, for advise(). It steers read-ahead and reclaim of pages that are not
    /// resident; locked pages stay resident whatever the advice.
    enum class Advice : std::uint8_t {
        kNormal,      ///< Default read-ahead (MADV_NORMAL).
        kSequential,  ///< Read ahead aggressively; pages may be reclaimed soon after use (MADV_SEQUENTIAL).
        kRandom,      ///< No read-ahead (MADV_RANDOM).
        kWillNeed,    ///< Start reading the whole range in now, asynchronously (MADV_WILLNEED).
    };

    MappedFile() noexcept = default;

    /// Unmaps. Errors cannot be reported here: call unmap() first when they matter.
    ~MappedFile();

    /// Takes over the mapping of @p other, which is left unmapped. The mapped address does not
    /// change, so spans obtained earlier stay valid.
    MappedFile(MappedFile&& other) noexcept;
    /// Unmaps the current mapping (errors cannot be reported) and takes over the one of @p other.
    MappedFile& operator=(MappedFile&& other) noexcept;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    /// Opens @p path (read-only, or read-write for kReadWrite), maps the range that @p options
    /// select, and closes the descriptor again.
    /// @return kInvalidArgument for nullptr, an object that already holds a mapping (unmap() it
    ///         first), a file that is not a regular file, or a mode outside MapMode; kOutOfRange if
    ///         the range does not lie inside the file; otherwise the open(2), mmap(2) or mlock(2)
    ///         result. On failure nothing is mapped.
    Status map(const char* path, const MapOptions& options) noexcept;

    /// Maps the range of @p file that @p options select. The descriptor must be open for reading,
    /// and for writing as well with kReadWrite (kPermissionDenied otherwise). The mapping does not
    /// depend on @p file afterwards: it stays valid when the File is closed.
    /// @return as map(const char*, const MapOptions&); kInvalidArgument if @p file is not open.
    Status map(const File& file, const MapOptions& options) noexcept;

    /// For a persistent state block: creates @p path if it does not exist (with @p permissions, at
    /// most 0xFFF, before the umask; e.g. 0x1A4 for octal 0644), sets its size to exactly @p size
    /// bytes (cutting it, or extending it with zeros), reserves its storage (File::preallocate,
    /// where the file system supports it) and maps all of it kReadWrite, populated. With the
    /// storage reserved, a write to the mapping cannot fail later for lack of space (which would
    /// raise SIGBUS). For durability, call sync() after writing, and sync_directory() on the parent
    /// directory once after the file was created.
    /// @return kInvalidArgument for nullptr, permission bits above 0xFFF or an object that already
    ///         holds a mapping (checked before the file is touched); otherwise the first error of
    ///         opening, resizing, reserving and mapping, in which case the file may already have
    ///         been created or resized.
    Status create(const char* path, std::uint64_t size, std::uint32_t permissions) noexcept;

    /// Removes the mapping (munmap(2), which also unlocks it). Spans obtained earlier dangle
    /// afterwards. Idempotent: kOk if nothing is mapped.
    /// @return kOk, or the munmap(2) error; the mapping is then kept, so unmap() can be retried.
    Status unmap() noexcept;

    /// The mapped range; empty if nothing, or an empty range, is mapped.
    /// RT-safe: no system call. Reading the bytes does not page-fault once the mapping is
    /// populated and locked (see the file comment for the exceptions).
    [[nodiscard]] Span<const std::uint8_t> bytes() const noexcept {
        return view_;
    }

    /// The mapped range for writing; empty for a kReadOnly mapping and if nothing is mapped.
    /// RT-safe: no system call. Whether writing faults depends on the mode (see the file comment).
    [[nodiscard]] Span<std::uint8_t> writable_bytes() noexcept {
        return (mode_ == MapMode::kReadOnly) ? Span<std::uint8_t>{} : view_;
    }

    /// @return the number of bytes in the mapped range.
    [[nodiscard]] std::size_t size() const noexcept {
        return view_.size();
    }

    /// @return true while a mapping, possibly an empty one, is held.
    [[nodiscard]] bool is_mapped() const noexcept {
        return mapped_;
    }

    /// @return the mode of the current mapping; kReadOnly when nothing is mapped.
    [[nodiscard]] MapMode mode() const noexcept {
        return mode_;
    }

    /// Writes the modified pages of a kReadWrite mapping to the file and waits until they are on
    /// stable storage (msync(2) with MS_SYNC); a write-back error is reported here (kSystemError
    /// for EIO). kOk without a system call for the other modes, whose changes never reach the
    /// file, and for an empty mapping. Blocks: never call it on a real-time thread.
    Status sync() noexcept;

    /// Locks the pages of the mapping in RAM (mlock(2); VirtualLock() on Windows), reading them in
    /// first if needed. A locked kReadWrite mapping on a disk-based file system still takes write
    /// faults (see the file comment).
    /// @return kResourceLimit if RLIMIT_MEMLOCK (on Windows, the minimum working-set size) does not
    ///         allow it, kPermissionDenied if the limit is 0 and the process lacks CAP_IPC_LOCK; kOk
    ///         for an empty mapping.
    Status lock() noexcept;

    /// Unlocks the pages of the mapping (munlock(2)); the kernel may evict them again.
    Status unlock() noexcept;

    /// Passes @p advice for the whole mapping to the kernel (madvise(2)); kOk for an empty mapping.
    /// @return kInvalidArgument for a value outside Advice.
    Status advise(Advice advice) noexcept;

private:
    /// Forgets the mapping without unmapping it.
    void reset() noexcept;

    void* region_{nullptr};          ///< Start of the kernel mapping (page-aligned); nullptr when empty.
    std::size_t region_size_{0U};    ///< Length of the kernel mapping: the bytes before the range + size().
    Span<std::uint8_t> view_{};      ///< The requested range inside the kernel mapping.
    MapMode mode_{MapMode::kReadOnly};
    bool mapped_{false};
#if defined(_WIN32)
    void* file_{nullptr};  ///< A kReadWrite mapping's own handle to its file, for sync(); else nullptr.
#endif
};

}  // namespace common

#endif  // COMMON_FILE_MAPPED_FILE_HPP
