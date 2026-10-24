/// @file timer_wheel.hpp
/// @brief Hashed timing wheel: many timeouts with O(1) schedule and cancel, and bounded work per cycle.

#ifndef COMMON_TIME_TIMER_WHEEL_HPP
#define COMMON_TIME_TIMER_WHEEL_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "common/core/assert.hpp"
#include "common/core/config.hpp"
#include "common/core/status.hpp"
#include "common/numeric/integer.hpp"
#include "common/time/clock.hpp"

namespace common {

/// Handle of a timer scheduled on a TimerWheel.
///
/// The generation tells apart the successive timers that use the same entry of a wheel: once a
/// timer has fired or been cancelled, its id stays inactive even after the entry is reused, so a
/// stale id cannot cancel or move someone else's timer. A default-constructed TimerId refers to
/// no timer.
struct TimerId final {
    std::uint32_t index{std::numeric_limits<std::uint32_t>::max()};  ///< Entry of the timer in its wheel.
    std::uint32_t generation{0U};  ///< Times the entry was released before this timer (modulo 2^32).
};

[[nodiscard]] constexpr bool operator==(const TimerId lhs, const TimerId rhs) noexcept {
    return (lhs.index == rhs.index) && (lhs.generation == rhs.generation);
}

[[nodiscard]] constexpr bool operator!=(const TimerId lhs, const TimerId rhs) noexcept {
    return !(lhs == rhs);
}

/// Called by TimerWheel::advance(), on the thread that calls it, when a timer expires. Receives
/// the id that schedule() returned (already inactive) and the context given to schedule().
/// Keep it short: it delays the remaining timers and the rest of the cycle.
using TimerCallback = void (*)(TimerId id, void* context) noexcept;

namespace detail {
namespace timer_wheel {

/// Number of whole ticks of @p tick_ns from @p start_ns to @p time_ns (nanoseconds on the same
/// clock); 0 if time_ns is before start_ns. Saturates instead of overflowing.
/// @pre tick_ns > 0
[[nodiscard]] constexpr std::uint64_t ticks_since(const std::int64_t start_ns, const std::int64_t time_ns,
                                                  const std::int64_t tick_ns) noexcept {
    const std::int64_t offset{saturating_sub(time_ns, start_ns)};
    return (offset > 0) ? static_cast<std::uint64_t>(offset / tick_ns) : std::uint64_t{0U};
}

}  // namespace timer_wheel
}  // namespace detail

/// Hashed timing wheel for many concurrent timeouts, e.g. order acknowledgements, retransmission
/// timers and diagnostic session timeouts, driven by the cyclic thread that owns it.
///
/// Time is counted in ticks of length tick() from the start time given to the constructor. Each
/// timer is filed in one of @p SlotCount slots by the tick that contains its deadline (modulo
/// SlotCount), so schedule(), cancel() and reschedule() are O(1). advance(now, budget) visits the
/// slots of the ticks that have elapsed since its previous call and fires the expired timers:
/// - Never early: a timer fires only in an advance() call whose now >= its deadline. Deadlines are
///   kept exactly (the tick only sets the bookkeeping granularity), so a timer fires in the first
///   such call unless the budget runs out: at most one tick late when advance() runs every tick.
///   A timer one or more revolutions (SlotCount ticks) ahead stays in its slot until its deadline.
/// - In order: one call fires its timers in deadline order, so timers of earlier ticks fire before
///   timers of later ticks. The order of equal deadlines is unspecified.
/// - Bounded: at most budget callbacks per call. Due timers over the budget stay due and fire
///   first in the next call. A call visits at most SlotCount slots, each once, however far now has
///   jumped (e.g. after a stall), so its work does not grow with the length of the jump.
///
/// Choose SlotCount * tick longer than the usual timeout: a timer further ahead is looked at once
/// per revolution until it expires. All storage is inline: entries are linked by 32-bit index in
/// std::arrays, with no pointers into the object. The wheel takes about 52 bytes per timer and 4
/// bytes per slot: give it static storage duration.
///
/// RT-path safe: schedule(), cancel(), reschedule(), is_active() and advance() never allocate,
/// block or throw, and each takes a bounded number of steps. Not thread-safe: the wheel belongs to
/// one thread, which calls advance() and runs the callbacks. Other threads pass their requests to
/// that thread, e.g. through an SpscQueue.
///
/// @code
///   void on_ack_timeout(common::TimerId id, void* context) noexcept;  // e.g. cancels the order
///
///   static common::TimerWheel<1024U> g_timeouts{1ms, common::MonotonicClock::now()};
///
///   // Order sent: arm its acknowledgement timeout (kResourceLimit when 1024 timers are active).
///   const common::Result<common::TimerId> timer{g_timeouts.schedule(now + 50ms, &on_ack_timeout, &order)};
///   order.ack_timer = timer.value_or(common::TimerId{});
///   // Acknowledgement received in time (kNotFound if the timeout has already fired):
///   static_cast<void>(g_timeouts.cancel(order.ack_timer));
///   // Once per control cycle, with the time the cycle already read, at most 32 callbacks:
///   g_timeouts.advance(now, 32U);
/// @endcode
template <std::size_t MaxTimers, std::size_t SlotCount = 256U>
class TimerWheel final {
    static_assert(MaxTimers > 0U, "MaxTimers must be positive");
    static_assert(MaxTimers <= (std::size_t{std::numeric_limits<std::uint32_t>::max()} - 1U),
                  "MaxTimers must not exceed 2^32 - 2");
    static_assert(is_power_of_two(SlotCount), "SlotCount must be a power of two");
    static_assert(SlotCount <= std::size_t{std::numeric_limits<std::uint32_t>::max()},
                  "SlotCount must fit in 32 bits");

public:
    /// Counts ticks of length @p tick from @p start (normally the current time). A good tick is
    /// the period of the thread that calls advance(): a longer tick means fewer slot visits per
    /// call, a shorter one fewer timers per slot. O(MaxTimers): construct it during initialisation.
    /// @pre tick > 0
    TimerWheel(const Duration tick, const TimePoint start) noexcept
        : tick_{tick},
          tick_ns_{(tick.count() > 0) ? tick.count() : std::int64_t{1}},
          start_ns_{start.time_since_epoch().count()} {
        COMMON_ASSERT(tick.count() > 0);
        slot_heads_.fill(kNil);
        // Chain every entry into the free list, so that entry 0 is handed out first.
        for (std::size_t i{0U}; i < MaxTimers; ++i) {
            entries_[i].next = ((i + 1U) < MaxTimers) ? static_cast<std::uint32_t>(i + 1U) : kNil;
        }
    }

    ~TimerWheel() = default;
    TimerWheel(const TimerWheel&) = delete;
    TimerWheel(TimerWheel&&) = delete;
    TimerWheel& operator=(const TimerWheel&) = delete;
    TimerWheel& operator=(TimerWheel&&) = delete;

    /// Arms a timer: the first advance() whose now >= @p deadline calls @p callback(id, @p context).
    /// A deadline that has already passed fires on the next advance(). When called from a
    /// callback, the timer fires in a later advance() call, never in the current one. O(1).
    /// @return the timer's id, kInvalidArgument if @p callback is nullptr, or kResourceLimit when
    ///         MaxTimers timers are active.
    [[nodiscard]] Result<TimerId> schedule(const TimePoint deadline, const TimerCallback callback,
                                           void* const context) noexcept {
        Result<TimerId> result{Result<TimerId>::failure(Status::kResourceLimit)};
        if (callback == nullptr) {
            result = Result<TimerId>::failure(Status::kInvalidArgument);
        } else if (free_head_ != kNil) {
            const std::uint32_t index{free_head_};
            Entry& entry{entries_[index]};
            free_head_ = entry.next;
            entry.deadline_ns = deadline.time_since_epoch().count();
            entry.callback = callback;
            entry.context = context;
            file_in_wheel(index);
            ++active_;
            result = Result<TimerId>::success(TimerId{index, entry.generation});
        } else {
            // Every entry is in use: keep kResourceLimit.
        }
        return result;
    }

    /// Disarms a timer, including one that is due but has not fired yet. From a callback, this
    /// keeps a timer that is due in the same advance() call from firing. O(1).
    /// @return kOk, or kNotFound if @p id is unknown or stale, or its timer has already fired or
    ///         been cancelled.
    Status cancel(const TimerId id) noexcept {
        Status status{Status::kNotFound};
        if (is_active(id)) {
            unlink(id.index);
            release(id.index);
            status = Status::kOk;
        }
        return status;
    }

    /// Moves an active timer to a new @p deadline, earlier or later, as if it were cancelled and
    /// scheduled again, but keeping its id. This includes a due timer that has not fired yet; from
    /// a callback, the timer then fires in a later advance() call at the earliest. A callback
    /// cannot reschedule its own timer (its id is no longer active): it calls schedule() instead.
    /// O(1).
    /// @return kOk, or kNotFound under the same conditions as cancel().
    Status reschedule(const TimerId id, const TimePoint deadline) noexcept {
        Status status{Status::kNotFound};
        if (is_active(id)) {
            unlink(id.index);
            entries_[id.index].deadline_ns = deadline.time_since_epoch().count();
            file_in_wheel(id.index);
            status = Status::kOk;
        }
        return status;
    }

    /// @return true from schedule() until the timer is cancelled or fires (its id becomes inactive
    ///         just before its callback runs).
    [[nodiscard]] bool is_active(const TimerId id) const noexcept {
        bool armed{false};
        if (id.index < MaxTimers) {
            const Entry& entry{entries_[id.index]};
            armed = (entry.state != State::kFree) && (entry.generation == id.generation);
        }
        return armed;
    }

    /// Fires the timers whose deadline is at or before @p now, earliest deadline first, at most
    /// @p max_expirations of them; due timers over the budget stay due for the next call. Pass the
    /// time the cycle already read. now should not decrease from call to call; if it does, still
    /// only timers whose deadline <= now fire.
    ///
    /// Each timer is removed, and its id becomes inactive, before its callback runs. Its entry is
    /// free again at that point, so the callback can always schedule a follow-up timer. Callbacks
    /// may call schedule(), cancel() and reschedule() on this wheel: a due timer that they cancel
    /// or reschedule does not fire in this call, and a timer that they schedule fires in a later
    /// call at the earliest, so a callback that re-arms itself cannot make the call loop.
    ///
    /// Work per call: at most SlotCount slots are visited, each once, together with the timers filed
    /// in them; the k timers that became due are sorted by deadline (O(k log k), in place) and
    /// merged with those still waiting from earlier calls; then at most @p max_expirations
    /// callbacks run.
    /// @pre not called from a callback of this wheel
    /// @return the number of timers fired.
    std::size_t advance(const TimePoint now, const std::size_t max_expirations) noexcept {
        COMMON_ASSERT(!advancing_);
        advancing_ = true;
        const std::int64_t now_ns{now.time_since_epoch().count()};
        const std::size_t newly_due{collect_due(now_ns)};
        sort_newly_due(newly_due);
        merge_into_due_list(newly_due);
        const std::size_t fired{fire_due(now_ns, max_expirations)};
        advancing_ = false;
        return fired;
    }

    /// Number of active timers: armed, or due and not yet fired.
    [[nodiscard]] std::size_t active() const noexcept {
        return active_;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return MaxTimers;
    }

    [[nodiscard]] Duration tick() const noexcept {
        return tick_;
    }

private:
    /// End-of-list marker of the index-linked lists; never a valid index (MaxTimers <= 2^32 - 2).
    static constexpr std::uint32_t kNil{std::numeric_limits<std::uint32_t>::max()};
    static constexpr std::uint64_t kSlotMask{SlotCount - 1U};

    enum class State : std::uint8_t {
        kFree,   ///< In the free list.
        kArmed,  ///< In a wheel slot.
        kDue,    ///< In the due list: expired, waiting for advance() to fire it within its budget.
    };

    /// One timer. The free list is linked through next only; wheel slots and the due list are
    /// linked in both directions, so that any entry can be removed in O(1).
    struct Entry final {
        std::int64_t deadline_ns{0};  // An integer: TimePoint's default constructor is not noexcept.
        TimerCallback callback{nullptr};
        void* context{nullptr};
        std::uint32_t next{kNil};
        std::uint32_t prev{kNil};
        std::uint32_t generation{0U};  // Incremented each time the entry is released.
        std::uint32_t slot{0U};        // Wheel slot while kArmed.
        State state{State::kFree};
    };

    /// Files an entry (deadline already set) in the slot of its deadline's tick. A tick that the
    /// wheel has already passed maps to the current tick's slot, which the next advance() visits
    /// first: every armed timer is filed at a tick >= cursor_.
    void file_in_wheel(const std::uint32_t index) noexcept {
        Entry& entry{entries_[index]};
        const std::uint64_t deadline_tick{detail::timer_wheel::ticks_since(start_ns_, entry.deadline_ns, tick_ns_)};
        const std::uint64_t slot_tick{(deadline_tick > cursor_) ? deadline_tick : cursor_};
        const std::uint32_t slot{static_cast<std::uint32_t>(slot_tick & kSlotMask)};
        entry.state = State::kArmed;
        entry.slot = slot;
        entry.prev = kNil;
        entry.next = slot_heads_[slot];
        if (entry.next != kNil) {
            entries_[entry.next].prev = index;
        }
        slot_heads_[slot] = index;
    }

    /// Removes an armed or due entry from its list.
    void unlink(const std::uint32_t index) noexcept {
        Entry& entry{entries_[index]};
        COMMON_ASSERT(entry.state != State::kFree);
        if (entry.prev != kNil) {
            entries_[entry.prev].next = entry.next;
        } else if (entry.state == State::kDue) {
            due_head_ = entry.next;
        } else {
            slot_heads_[entry.slot] = entry.next;
        }
        if (entry.next != kNil) {
            entries_[entry.next].prev = entry.prev;
        } else if (entry.state == State::kDue) {
            due_tail_ = entry.prev;
        } else {
            // Wheel slots are not tracked at the tail.
        }
        entry.prev = kNil;
        entry.next = kNil;
    }

    /// Returns an unlinked entry to the free list. Its id becomes inactive.
    void release(const std::uint32_t index) noexcept {
        Entry& entry{entries_[index]};
        ++entry.generation;  // Wraps after 2^32 uses of the entry.
        entry.state = State::kFree;
        entry.callback = nullptr;
        entry.context = nullptr;
        entry.next = free_head_;
        free_head_ = index;
        --active_;
    }

    /// Moves every armed timer whose deadline <= now into newly_due_ (unsorted) and advances the
    /// cursor to now's tick. @return the number of timers moved.
    [[nodiscard]] std::size_t collect_due(const std::int64_t now_ns) noexcept {
        const std::uint64_t now_tick{detail::timer_wheel::ticks_since(start_ns_, now_ns, tick_ns_)};
        const std::uint64_t last_tick{(now_tick > cursor_) ? now_tick : cursor_};
        // Expired timers are filed at ticks cursor_ .. last_tick: cursor_ itself still holds the
        // timers that were not due at the previous call. SlotCount ticks or more cover every slot,
        // and each slot is visited once, whatever the length of the jump.
        const std::uint64_t span{last_tick - cursor_};
        const std::uint64_t visits{(span < (SlotCount - 1U)) ? (span + 1U) : SlotCount};
        std::size_t collected{0U};
        for (std::uint64_t i{0U}; i < visits; ++i) {
            collected = collect_slot(static_cast<std::uint32_t>((cursor_ + i) & kSlotMask), now_ns, collected);
        }
        cursor_ = last_tick;
        return collected;
    }

    /// Moves the expired timers of one slot to newly_due_[collected...]. @return the new count.
    [[nodiscard]] std::size_t collect_slot(const std::uint32_t slot, const std::int64_t now_ns,
                                           const std::size_t collected) noexcept {
        std::size_t total{collected};
        std::uint32_t index{slot_heads_[slot]};
        std::size_t visited{0U};
        // A slot holds at most MaxTimers entries.
        while ((index != kNil) && (visited < MaxTimers)) {
            const std::uint32_t next{entries_[index].next};
            if (entries_[index].deadline_ns <= now_ns) {
                unlink(index);
                COMMON_ASSERT(total < MaxTimers);  // Each entry is collected at most once per call.
                newly_due_[total] = index;
                ++total;
            }
            index = next;
            ++visited;
        }
        return total;
    }

    /// Sorts newly_due_[0, count) by deadline, in place and without allocating.
    void sort_newly_due(const std::size_t count) noexcept {
        std::uint32_t* const first{newly_due_.data()};
        std::sort(first, first + count, [this](const std::uint32_t lhs, const std::uint32_t rhs) noexcept {
            return entries_[lhs].deadline_ns < entries_[rhs].deadline_ns;
        });
    }

    /// Merges newly_due_[0, count), sorted by deadline, into the due list, which stays sorted.
    /// Among equal deadlines, timers that were already waiting stay first.
    void merge_into_due_list(const std::size_t count) noexcept {
        // Usually every new timer expires after every waiting one, and the merge is an append.
        std::uint32_t position{kNil};
        if ((count > 0U) && (due_tail_ != kNil) &&
            (entries_[newly_due_[0U]].deadline_ns < entries_[due_tail_].deadline_ns)) {
            position = due_head_;
        }
        for (std::size_t i{0U}; i < count; ++i) {
            const std::uint32_t index{newly_due_[i]};
            const std::int64_t deadline_ns{entries_[index].deadline_ns};
            // position only moves forward, so the whole merge is O(count + due list length).
            std::size_t step{0U};
            while ((position != kNil) && (step < MaxTimers) && (entries_[position].deadline_ns <= deadline_ns)) {
                position = entries_[position].next;
                ++step;
            }
            insert_due_before(index, position);
        }
    }

    /// Links an entry into the due list in front of @p position (kNil: at the tail).
    void insert_due_before(const std::uint32_t index, const std::uint32_t position) noexcept {
        Entry& entry{entries_[index]};
        const std::uint32_t prev{(position != kNil) ? entries_[position].prev : due_tail_};
        entry.state = State::kDue;
        entry.prev = prev;
        entry.next = position;
        if (prev != kNil) {
            entries_[prev].next = index;
        } else {
            due_head_ = index;
        }
        if (position != kNil) {
            entries_[position].prev = index;
        } else {
            due_tail_ = index;
        }
    }

    /// Fires due timers from the head of the due list. Callbacks never add to the due list, so it
    /// holds at most MaxTimers timers and the loop ends after at most MaxTimers callbacks.
    [[nodiscard]] std::size_t fire_due(const std::int64_t now_ns, const std::size_t max_expirations) noexcept {
        std::size_t fired{0U};
        // The deadline test only matters when now went backwards since an earlier call.
        while ((fired < max_expirations) && (fired < MaxTimers) && (due_head_ != kNil) &&
               (entries_[due_head_].deadline_ns <= now_ns)) {
            const std::uint32_t index{due_head_};
            const TimerId id{index, entries_[index].generation};
            const TimerCallback callback{entries_[index].callback};
            void* const context{entries_[index].context};
            unlink(index);
            release(index);
            ++fired;
            callback(id, context);
        }
        return fired;
    }

    Duration tick_;
    std::int64_t tick_ns_;      // tick_ in nanoseconds, never 0 (even if assertions are disabled).
    std::int64_t start_ns_;     // Start of tick 0.
    std::uint64_t cursor_{0U};  // Tick of the latest advance(); every armed timer is filed at a tick >= cursor_.
    std::size_t active_{0U};
    std::uint32_t free_head_{0U};
    std::uint32_t due_head_{kNil};  // Due list: expired timers not fired yet, sorted by deadline.
    std::uint32_t due_tail_{kNil};
    bool advancing_{false};
    std::array<std::uint32_t, SlotCount> slot_heads_{};
    std::array<Entry, MaxTimers> entries_{};
    std::array<std::uint32_t, MaxTimers> newly_due_{};  // Scratch of advance(): timers that just expired.
};

}  // namespace common

#endif  // COMMON_TIME_TIMER_WHEEL_HPP
