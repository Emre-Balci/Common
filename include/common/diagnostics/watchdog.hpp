/// @file watchdog.hpp
/// @brief Deadline supervision of threads through heartbeats.

#ifndef COMMON_DIAGNOSTICS_WATCHDOG_HPP
#define COMMON_DIAGNOSTICS_WATCHDOG_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "common/containers/static_vector.hpp"
#include "common/core/config.hpp"
#include "common/core/status.hpp"
#include "common/time/clock.hpp"

namespace common {

/// Liveness signal of one supervised thread. The thread calls beat() each time it makes
/// progress, e.g. once per cycle; a Watchdog on another thread checks that beats keep coming.
///
/// beat() is one atomic store: wait-free and safe on the real-time path. The heartbeat fills a
/// cache line of its own, so beating does not slow down neighbouring data.
class Heartbeat final {
    static_assert(std::atomic<std::int64_t>::is_always_lock_free, "64-bit atomics must be lock-free");

public:
    Heartbeat() noexcept = default;
    ~Heartbeat() = default;
    Heartbeat(const Heartbeat&) = delete;
    Heartbeat(Heartbeat&&) = delete;
    Heartbeat& operator=(const Heartbeat&) = delete;
    Heartbeat& operator=(Heartbeat&&) = delete;

    /// Supervised thread only. Pass the time the cycle already read to avoid a second clock read.
    void beat(const TimePoint now = MonotonicClock::now()) noexcept {
        last_beat_ns_.store(now.time_since_epoch().count(), std::memory_order_release);
    }

    /// Any thread. @return the time of the last beat, or TimePoint::min() before the first one.
    [[nodiscard]] TimePoint last_beat() const noexcept {
        return TimePoint{Duration{last_beat_ns_.load(std::memory_order_acquire)}};
    }

private:
    alignas(kCacheLineSize) std::atomic<std::int64_t> last_beat_ns_{std::numeric_limits<std::int64_t>::min()};
};

enum class Liveness : std::uint8_t {
    kAlive,    ///< The last beat (or registration) is no older than the timeout.
    kExpired,  ///< The timeout has passed without a beat.
};

/// Supervises up to @p MaxEntries heartbeats, each with its own timeout.
///
/// A supervisor thread (normally not real-time) calls check() periodically, more often than the
/// shortest timeout. An entry expires when its heartbeat has not beaten for longer than its
/// timeout, and becomes alive again when beats resume. Each alive-to-expired transition is
/// counted and reported once to the optional expiry handler, which can e.g. command a safe
/// state, cancel open orders, or log the fault.
///
/// The watchdog itself is not thread-safe: it belongs to the supervisor thread. The heartbeats
/// are the only state shared with the supervised threads.
///
/// @code
///   static common::Heartbeat g_control_alive{};        // control thread: g_control_alive.beat();
///   common::Watchdog<4U> watchdog{};                   // supervisor:
///   const auto entry{watchdog.add(g_control_alive, 5ms, "control")};
///   for (...) { static_cast<void>(watchdog.check()); ... }
/// @endcode
template <std::size_t MaxEntries>
class Watchdog final {
public:
    /// Called by check() on the supervisor thread. Must not block for long: it delays the checks
    /// of the remaining entries.
    using ExpiryHandler = void (*)(std::size_t index, const char* name, void* context) noexcept;

    Watchdog() noexcept = default;

    /// Supervises @p heartbeat, which must beat at least every @p timeout. Supervision starts at
    /// @p now, so a heartbeat that never beats expires at now + timeout. @p heartbeat must outlive
    /// the watchdog; @p name, if not nullptr, must have static storage duration.
    /// @return the entry index, kInvalidArgument if timeout <= 0, or kResourceLimit when all
    ///         MaxEntries entries are in use.
    [[nodiscard]] Result<std::size_t> add(const Heartbeat& heartbeat, const Duration timeout,
                                          const char* const name = nullptr,
                                          const TimePoint now = MonotonicClock::now()) noexcept {
        Result<std::size_t> result{Result<std::size_t>::failure(Status::kInvalidArgument)};
        if (timeout.count() > 0) {
            const std::size_t index{entries_.size()};
            Entry entry{};
            entry.heartbeat = &heartbeat;
            entry.timeout = timeout;
            entry.armed_at = now;
            entry.name = (name != nullptr) ? name : "";
            result = entries_.push_back(entry) ? Result<std::size_t>::success(index)
                                               : Result<std::size_t>::failure(Status::kResourceLimit);
        }
        return result;
    }

    /// Installs a handler for alive-to-expired transitions; nullptr removes it.
    void set_expiry_handler(const ExpiryHandler handler, void* const context) noexcept {
        handler_ = handler;
        handler_context_ = context;
    }

    /// Supervisor thread. Re-evaluates every entry at @p now: O(size()), no allocation, and it
    /// never blocks (unless the expiry handler does).
    /// @return the number of entries that are currently expired.
    std::size_t check(const TimePoint now = MonotonicClock::now()) noexcept {
        std::size_t expired{0U};
        for (std::size_t index{0U}; index < entries_.size(); ++index) {
            Entry& entry{entries_[index]};
            const TimePoint last{entry.heartbeat->last_beat()};
            const TimePoint reference{(last > entry.armed_at) ? last : entry.armed_at};
            // A beat stamped slightly after now (read on another CPU) gives a negative age: alive.
            if ((now - reference) <= entry.timeout) {
                entry.state = Liveness::kAlive;
            } else {
                ++expired;
                if (entry.state == Liveness::kAlive) {
                    entry.state = Liveness::kExpired;
                    ++entry.expirations;
                    if (handler_ != nullptr) {
                        handler_(index, entry.name, handler_context_);
                    }
                }
            }
        }
        return expired;
    }

    /// State found by the last check() (kAlive before the first one).
    /// @pre index < size()
    [[nodiscard]] Liveness state(const std::size_t index) const noexcept {
        return entries_[index].state;
    }

    /// Number of alive-to-expired transitions so far.
    /// @pre index < size()
    [[nodiscard]] std::uint64_t expirations(const std::size_t index) const noexcept {
        return entries_[index].expirations;
    }

    /// Name given to add() ("" if none).
    /// @pre index < size()
    [[nodiscard]] const char* name(const std::size_t index) const noexcept {
        return entries_[index].name;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return entries_.size();
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return MaxEntries;
    }

private:
    struct Entry final {
        const Heartbeat* heartbeat{nullptr};
        Duration timeout{};
        TimePoint armed_at{};
        const char* name{""};
        Liveness state{Liveness::kAlive};
        std::uint64_t expirations{0U};
    };

    StaticVector<Entry, MaxEntries> entries_{};
    ExpiryHandler handler_{nullptr};
    void* handler_context_{nullptr};
};

}  // namespace common

#endif  // COMMON_DIAGNOSTICS_WATCHDOG_HPP
