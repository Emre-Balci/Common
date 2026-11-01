/// @file signal.hpp
/// @brief Observer pattern without heap: a Signal calls the slots connected to it.

#ifndef COMMON_EVENT_SIGNAL_HPP
#define COMMON_EVENT_SIGNAL_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

#include "common/core/assert.hpp"
#include "common/core/status.hpp"
#include "common/functional/inplace_function.hpp"

namespace common {

/// Handle of a slot connected to a Signal.
///
/// As with TimerId, the generation tells apart the successive slots that use the same entry of a
/// signal: once a slot is disconnected, its handle stays stale even after the entry is reused, so a
/// stale handle cannot disconnect someone else's slot. A default-constructed Connection refers to no
/// slot.
struct Connection final {
    std::uint32_t index{std::numeric_limits<std::uint32_t>::max()};  ///< Entry of the slot in its signal.
    std::uint32_t generation{0U};  ///< Times the entry was released before this slot (modulo 2^32).
};

[[nodiscard]] constexpr bool operator==(const Connection lhs, const Connection rhs) noexcept {
    return (lhs.index == rhs.index) && (lhs.generation == rhs.generation);
}

[[nodiscard]] constexpr bool operator!=(const Connection lhs, const Connection rhs) noexcept {
    return !(lhs == rhs);
}

template <typename Signature, std::size_t MaxSlots, std::size_t SlotCapacity = kInplaceFunctionCapacity>
class Signal;  // Declared only: Signature must be a function type void(Args...).

/// Notifies up to @p MaxSlots observers ("slots") of an event: notify(args...) calls every connected
/// slot with the arguments, in the order the slots were connected. A slot is any noexcept callable
/// returning void that fits an InplaceFunction<void(Args...), SlotCapacity>, typically a lambda that
/// captures the observer.
///
/// - connect() returns a Connection to disconnect() the slot later; a ScopedConnection does that on
///   destruction. Connections are checked with a generation, so a stale one is harmless (kNotFound).
/// - During a notification a slot may connect and disconnect slots (itself included). A slot
///   disconnected during a notification is not called later in it; its callable is destroyed when the
///   notification ends, never while it runs, and only then is its entry free again: inside a
///   notification, connect() can therefore return kResourceLimit before size() reaches MaxSlots. A
///   slot connected during a notification is first called by the next one.
/// - notify() must not be called from one of the signal's own slots (no recursion; COMMON_ASSERT),
///   nor may the signal be destroyed from one.
/// - The member is notify(), not emit(): Qt defines emit as a macro.
///
/// Every member is O(MaxSlots) at most and never allocates, blocks or throws: safe on the real-time
/// path. Not thread-safe: a signal belongs to one thread; to notify another thread, go through a
/// queue or an ActiveObject. Give a large signal static storage duration; it must outlive the
/// ScopedConnections that refer to it.
///
/// @code
///   common::Signal<void(double), 8U> speed_changed{};             // in the sensor unit
///   const common::Result<common::Connection> connection{
///       speed_changed.connect([this](const double speed) noexcept { on_speed(speed); })};
///   common::ScopedConnection scoped{speed_changed, connection.value()};  // after checking has_value()
///   speed_changed.notify(12.5);                                    // each cycle: calls on_speed(12.5)
/// @endcode
template <typename... Args, std::size_t MaxSlots, std::size_t SlotCapacity>
class Signal<void(Args...), MaxSlots, SlotCapacity> final {
    static_assert(MaxSlots > 0U, "MaxSlots must be positive");
    static_assert(MaxSlots < std::size_t{std::numeric_limits<std::uint32_t>::max()}, "MaxSlots must fit in 32 bits");
    static_assert(!std::disjunction<std::is_rvalue_reference<Args>...>::value,
                  "every slot receives the same arguments: rvalue references are not allowed");

public:
    using Slot = InplaceFunction<void(Args...), SlotCapacity>;

    Signal() noexcept = default;

    /// @pre not called from a slot of this signal
    ~Signal() {
        COMMON_ASSERT(!notifying_);
    }

    Signal(const Signal&) = delete;
    Signal(Signal&&) = delete;
    Signal& operator=(const Signal&) = delete;
    Signal& operator=(Signal&&) = delete;

    /// Connects @p slot: every later notification calls it, after the slots connected before it.
    /// @return its Connection; kInvalidArgument for an empty callable (a null function pointer, an
    ///         empty InplaceFunction); kResourceLimit when no entry is free.
    template <typename F>
    [[nodiscard]] Result<Connection> connect(F&& slot) noexcept {
        Slot function{std::forward<F>(slot)};
        Result<Connection> result{Result<Connection>::failure(Status::kInvalidArgument)};
        if (!function.empty()) {
            result = Result<Connection>::failure(Status::kResourceLimit);
            std::size_t index{MaxSlots};
            for (std::size_t candidate{0U}; (candidate < MaxSlots) && (index == MaxSlots); ++candidate) {
                if (entries_[candidate].state == EntryState::kFree) {
                    index = candidate;
                }
            }
            if (index < MaxSlots) {
                Entry& entry{entries_[index]};
                entry.slot = std::move(function);
                entry.state = EntryState::kConnected;
                order_[order_size_] = static_cast<std::uint32_t>(index);
                ++order_size_;
                ++size_;
                result = Result<Connection>::success(Connection{static_cast<std::uint32_t>(index), entry.generation});
            }
        }
        return result;
    }

    /// Disconnects a slot. Inside a notification, the slot is not called later in it.
    /// @return kOk, or kNotFound if @p connection is unknown or stale.
    Status disconnect(const Connection connection) noexcept {
        Status status{Status::kNotFound};
        if (connected(connection)) {
            release(connection.index);
            status = Status::kOk;
        }
        return status;
    }

    /// Disconnects every slot.
    void disconnect_all() noexcept {
        for (std::size_t index{0U}; index < MaxSlots; ++index) {
            if (entries_[index].state == EntryState::kConnected) {
                release(static_cast<std::uint32_t>(index));
            }
        }
    }

    /// @return true from connect() until the slot is disconnected.
    [[nodiscard]] bool connected(const Connection connection) const noexcept {
        bool live{false};
        if (connection.index < MaxSlots) {
            const Entry& entry{entries_[connection.index]};
            live = (entry.state == EntryState::kConnected) && (entry.generation == connection.generation);
        }
        return live;
    }

    /// Calls every connected slot with @p args, as lvalues, in connection order.
    /// @pre not called from a slot of this signal
    /// @return the number of slots called.
    std::size_t notify(Args... args) noexcept {
        COMMON_ASSERT(!notifying_);
        notifying_ = true;
        // Slots connected during this notification are appended after count: they are not called.
        const std::size_t count{order_size_};
        std::size_t called{0U};
        for (std::size_t position{0U}; position < count; ++position) {
            const Entry& entry{entries_[order_[position]]};
            if (entry.state == EntryState::kConnected) {
                entry.slot(args...);
                ++called;
            }
        }
        notifying_ = false;
        free_released();
        return called;
    }

    /// Number of connected slots.
    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0U;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return MaxSlots;
    }

private:
    enum class EntryState : std::uint8_t {
        kFree,
        kConnected,
        kReleased,  ///< Disconnected during a notification; freed when it ends.
    };

    struct Entry final {
        Slot slot{};
        std::uint32_t generation{0U};  // Incremented each time the slot is disconnected.
        EntryState state{EntryState::kFree};
    };

    /// Disconnects the connected entry @p index: at once, or after the notification in progress.
    void release(const std::uint32_t index) noexcept {
        Entry& entry{entries_[index]};
        ++entry.generation;  // Wraps after 2^32 uses of the entry.
        --size_;
        if (notifying_) {
            entry.state = EntryState::kReleased;
        } else {
            entry.slot.reset();
            entry.state = EntryState::kFree;
            remove_from_order(index);
        }
    }

    /// Removes @p index from the connection order, keeping the order of the others.
    void remove_from_order(const std::uint32_t index) noexcept {
        std::size_t kept{0U};
        for (std::size_t position{0U}; position < order_size_; ++position) {
            if (order_[position] != index) {
                order_[kept] = order_[position];
                ++kept;
            }
        }
        order_size_ = kept;
    }

    /// After a notification: frees the entries released during it.
    void free_released() noexcept {
        std::size_t kept{0U};
        for (std::size_t position{0U}; position < order_size_; ++position) {
            Entry& entry{entries_[order_[position]]};
            if (entry.state == EntryState::kReleased) {
                entry.slot.reset();
                entry.state = EntryState::kFree;
            } else {
                order_[kept] = order_[position];
                ++kept;
            }
        }
        order_size_ = kept;
    }

    std::array<Entry, MaxSlots> entries_{};
    std::array<std::uint32_t, MaxSlots> order_{};  // Entries in connection order (connected or released).
    std::size_t order_size_{0U};
    std::size_t size_{0U};
    bool notifying_{false};
};

/// Owns a Connection and disconnects it when destroyed or reset(): an observer object holds one per
/// slot it connected, so it cannot be called after it is gone. Move-only, and type-erased, so that
/// one member type works with every Signal. The signal must outlive it.
///
/// Not thread-safe, like the signal it refers to.
class ScopedConnection final {
public:
    /// Manages no connection.
    ScopedConnection() noexcept = default;

    /// Manages @p connection of @p signal (a Signal of any signature).
    template <typename SignalType>
    ScopedConnection(SignalType& signal, const Connection connection) noexcept
        : signal_{&signal}, disconnect_{&disconnect_from<SignalType>}, connection_{connection} {}

    ~ScopedConnection() {
        reset();
    }

    ScopedConnection(const ScopedConnection&) = delete;
    ScopedConnection& operator=(const ScopedConnection&) = delete;

    /// Takes over the connection of @p other, which then manages none.
    ScopedConnection(ScopedConnection&& other) noexcept
        : signal_{other.signal_}, disconnect_{other.disconnect_}, connection_{other.connection_} {
        other.forget();
    }

    /// Disconnects the connection it manages, then takes over that of @p other.
    ScopedConnection& operator=(ScopedConnection&& other) noexcept {
        if (this != &other) {
            reset();
            signal_ = other.signal_;
            disconnect_ = other.disconnect_;
            connection_ = other.connection_;
            other.forget();
        }
        return *this;
    }

    /// Disconnects now (if the slot is still connected) and manages no connection any more.
    void reset() noexcept {
        if (signal_ != nullptr) {
            static_cast<void>(disconnect_(signal_, connection_));  // kNotFound if disconnected already.
        }
        forget();
    }

    /// Stops managing the connection without disconnecting it. @return the connection.
    [[nodiscard]] Connection release() noexcept {
        const Connection connection{connection_};
        forget();
        return connection;
    }

    /// The connection managed (a default Connection if none).
    [[nodiscard]] Connection connection() const noexcept {
        return connection_;
    }

private:
    using Disconnect = Status (*)(void* signal, Connection connection) noexcept;

    template <typename SignalType>
    static Status disconnect_from(void* const signal, const Connection connection) noexcept {
        return static_cast<SignalType*>(signal)->disconnect(connection);
    }

    void forget() noexcept {
        signal_ = nullptr;
        disconnect_ = nullptr;
        connection_ = Connection{};
    }

    void* signal_{nullptr};
    Disconnect disconnect_{nullptr};
    Connection connection_{};
};

}  // namespace common

#endif  // COMMON_EVENT_SIGNAL_HPP
