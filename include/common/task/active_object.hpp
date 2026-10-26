/// @file active_object.hpp
/// @brief Active Object: a thread of its own that runs commands posted from any thread, one at a time.

#ifndef COMMON_TASK_ACTIVE_OBJECT_HPP
#define COMMON_TASK_ACTIVE_OBJECT_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "common/concurrency/mpmc_queue.hpp"
#include "common/concurrency/spin.hpp"
#include "common/core/assert.hpp"
#include "common/core/status.hpp"
#include "common/diagnostics/watchdog.hpp"
#include "common/functional/inplace_function.hpp"
#include "common/system/thread.hpp"
#include "common/task/task.hpp"
#include "common/time/clock.hpp"

namespace common {

namespace detail {
namespace active_object {

/// Rounds that stop() waits for post() calls in flight before it gives up (kTimeout).
inline constexpr std::uint32_t kMaxStopWaitRounds{1000U};

}  // namespace active_object
}  // namespace detail

/// An object with a thread of its own and a queue of commands: any thread post()s a command (a
/// noexcept callable returning void, stored in an InplaceFunction without heap), and the object's
/// thread runs the commands one at a time, in queue order. The state that only its commands touch
/// needs no locks: e.g. an order gateway whose public functions post lambdas that talk to the
/// exchange, or a logger that does its I/O off the real-time threads.
///
/// - Composition is the intended use: a class holds an ActiveObject and its public functions post
///   lambdas that capture `this`.
/// - post() is lock-free and bounded (an MpmcQueue of @p QueueCapacity commands): kResourceLimit when
///   the queue is full, kNotRunning once the object is stopping. A post() that returned kOk is never
///   lost: its command runs, or with stop(kCancel) it is discarded and counted in discarded().
/// - The poll hook given to start() runs on the object's thread at least every idle_sleep (and after
///   each batch of commands): the periodic work of that thread, e.g. advancing a TimerWheel of
///   acknowledgement timeouts or polling a SerialPort. With idle_sleep zero the thread busy-polls
///   (cpu_relax()) instead of sleeping: lowest latency, for a thread on an isolated core, which it uses
///   completely. Otherwise a command posted to an idle object starts after up to about idle_sleep.
/// - The thread beats heartbeat() every round, so a Watchdog can supervise it; the heartbeat is parked
///   (TimePoint::max()) while the thread is not running.
/// - stop(kDrain) runs every accepted command, then joins the thread; stop(kCancel) lets the running
///   command finish and discards the queued ones. request_stop() lets a command end its own object's
///   thread (stop() then joins it). The object can be started again after stop().
/// - Without a thread (before start()), run_pending() runs queued commands on the calling thread: for
///   tests, or to embed the object in an existing loop.
///
/// Calls from a command or the poll hook: post(), request_stop() and stopping() are allowed; start(),
/// stop() and run_pending() of the same object are contract violations (COMMON_ASSERT): the thread
/// cannot join itself.
///
/// RT-path safe: post(), request_stop(), stopping(), heartbeat(), executed(), discarded(). start(),
/// stop(), run_pending() and running() belong to one control thread. Give it static storage duration.
///
/// @code
///   class OrderGateway final {
///   public:
///       common::Status send(const Order order) noexcept {   // any thread, never blocks
///           return active_.post([this, order]() noexcept { transmit(order); });
///       }
///       common::Status start(const common::ThreadConfig& config) noexcept {
///           return active_.start(config, std::chrono::milliseconds{1}, &OrderGateway::poll, this);
///       }
///   private:
///       static void poll(const common::TimePoint now, void* const self) noexcept {
///           static_cast<void>(static_cast<OrderGateway*>(self)->timeouts_.advance(now, 32U));
///       }
///       void transmit(const Order& order) noexcept;          // runs on the gateway's thread only
///       common::TimerWheel<1024U> timeouts_{std::chrono::milliseconds{1}, common::MonotonicClock::now()};
///       common::ActiveObject<256U> active_{};
///   };
/// @endcode
template <std::size_t QueueCapacity, std::size_t CommandCapacity = kInplaceFunctionCapacity>
class ActiveObject final {
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "std::atomic<std::uint32_t> must be lock-free");
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "std::atomic<std::uint64_t> must be lock-free");

public:
    using Command = InplaceFunction<void(), CommandCapacity>;

    /// Called on the object's thread at least every idle_sleep, with the current time and the context
    /// given to start().
    using Poll = void (*)(TimePoint now, void* context) noexcept;

    ActiveObject() noexcept {
        heartbeat_.beat(TimePoint::max());  // Parked until the thread runs.
    }

    /// @pre !running(). The commands still queued are destroyed without running (counted in discarded()).
    ~ActiveObject() {
        COMMON_ASSERT(!running_);
        discard_queued();
    }

    ActiveObject(const ActiveObject&) = delete;
    ActiveObject(ActiveObject&&) = delete;
    ActiveObject& operator=(const ActiveObject&) = delete;
    ActiveObject& operator=(ActiveObject&&) = delete;

    /// Starts the object's thread with @p config. Commands posted before start() are run. Allowed again
    /// after stop().
    /// @param idle_sleep how long the thread sleeps when it has nothing to do; zero busy-polls.
    /// @param poll optional periodic work, called on the object's thread with @p poll_context.
    /// @pre not called from a command or the poll hook of this object
    /// @return kOk; kAlreadyRunning; kInvalidArgument if idle_sleep < 0; or an RtThread::start() error.
    Status start(const ThreadConfig& config, const Duration idle_sleep = std::chrono::milliseconds{1},
                 const Poll poll = nullptr, void* const poll_context = nullptr) noexcept {
        COMMON_ASSERT(detail::current_runner() != this);
        Status status{Status::kAlreadyRunning};
        if (running_) {
            // kAlreadyRunning.
        } else if (idle_sleep < Duration::zero()) {
            status = Status::kInvalidArgument;
        } else {
            idle_sleep_ = idle_sleep;
            poll_ = poll;
            poll_context_ = poll_context;
            exit_.store(false, std::memory_order_relaxed);
            cancel_mode_.store(false, std::memory_order_relaxed);
            stopping_.store(false, std::memory_order_seq_cst);
            status = thread_.start(config, &thread_entry, this);
            running_ = is_ok(status);
        }
        return status;
    }

    /// Any thread (the object's own included), lock-free: queues @p command, a noexcept callable that
    /// returns void and fits a Command, to run on the object's thread.
    /// @return kOk; kInvalidArgument for an empty callable; kResourceLimit if the queue is full or too
    ///         contended; kNotRunning once stop() or request_stop() was called.
    template <typename F>
    Status post(F&& command) noexcept {
        Command function{std::forward<F>(command)};
        Status status{Status::kInvalidArgument};
        if (!function.empty()) {
            // The handshake with stop(), as in TaskExecutor::submit().
            static_cast<void>(in_flight_.fetch_add(1U, std::memory_order_seq_cst));
            status = Status::kNotRunning;
            if (!stopping_.load(std::memory_order_seq_cst)) {
                static_cast<void>(queued_.fetch_add(1U, std::memory_order_relaxed));
                if (queue_.try_push(std::move(function))) {
                    status = Status::kOk;
                } else {
                    static_cast<void>(queued_.fetch_sub(1U, std::memory_order_relaxed));
                    status = Status::kResourceLimit;
                }
            }
            static_cast<void>(in_flight_.fetch_sub(1U, std::memory_order_seq_cst));
        }
        return status;
    }

    /// Non-RT: refuses new commands, then ends the thread as @p mode says (see the class) and joins it.
    /// Without a running thread, the queued commands are discarded (never run on the calling thread).
    /// @pre not called from a command or the poll hook of this object
    /// @return kOk; kNotRunning if the thread was not running; kTimeout if post() calls were still in
    ///         flight after the bounded wait (the object stays stopping: call stop() again); or an
    ///         RtThread::join() error.
    Status stop(const StopMode mode = StopMode::kDrain) noexcept {
        COMMON_ASSERT(detail::current_runner() != this);
        if (running_ && (mode == StopMode::kCancel)) {
            // Set before stopping_, so that whoever sees stopping() also sees that the thread exits.
            cancel_mode_.store(true, std::memory_order_relaxed);
            exit_.store(true, std::memory_order_release);
        }
        stopping_.store(true, std::memory_order_seq_cst);
        const Duration step{(idle_sleep_ > Duration::zero()) ? idle_sleep_ : Duration{std::chrono::milliseconds{1}}};
        bool settled{in_flight_.load(std::memory_order_seq_cst) == 0U};
        for (std::uint32_t round{0U}; (round < detail::active_object::kMaxStopWaitRounds) && (!settled); ++round) {
            static_cast<void>(sleep_for(step));
            settled = (in_flight_.load(std::memory_order_seq_cst) == 0U);
        }
        Status status{Status::kTimeout};
        if (settled) {
            status = Status::kNotRunning;
            if (running_) {
                exit_.store(true, std::memory_order_release);
                status = thread_.join();
                running_ = false;
                heartbeat_.beat(TimePoint::max());
            }
            discard_queued();
        }
        return status;
    }

    /// Any thread, a command included: refuses new commands; the thread runs the accepted ones and
    /// leaves its loop. stop() still joins it (running() stays true until then).
    void request_stop() noexcept {
        stopping_.store(true, std::memory_order_seq_cst);
        exit_.store(true, std::memory_order_release);
    }

    /// Any thread: true from stop() or request_stop() until the next start().
    [[nodiscard]] bool stopping() const noexcept {
        return stopping_.load(std::memory_order_seq_cst);
    }

    /// Runs up to @p max_commands queued commands on the calling thread.
    /// @pre !running(); not called from a command of this object
    /// @return the number of commands run.
    std::size_t run_pending(const std::size_t max_commands) noexcept {
        COMMON_ASSERT(detail::current_runner() != this);
        COMMON_ASSERT(!running_);
        const detail::RunnerScope scope{this};
        return run_batch(max_commands);
    }

    /// @return true from a successful start() until stop().
    [[nodiscard]] bool running() const noexcept {
        return running_;
    }

    /// Beaten by the object's thread every round; parked (TimePoint::max()) while it is not running.
    [[nodiscard]] const Heartbeat& heartbeat() const noexcept {
        return heartbeat_;
    }

    /// Any thread: commands run so far.
    [[nodiscard]] std::uint64_t executed() const noexcept {
        return executed_.load(std::memory_order_relaxed);
    }

    /// Any thread: commands destroyed without running (stop(kCancel), stop() without a thread, the destructor).
    [[nodiscard]] std::uint64_t discarded() const noexcept {
        return discarded_.load(std::memory_order_relaxed);
    }

private:
    static void thread_entry(void* const context) noexcept {
        static_cast<ActiveObject*>(context)->service_loop();
    }

    /// @return true once stop(kCancel) asked the thread to exit: no further command may start.
    [[nodiscard]] bool cancelling() const noexcept {
        return exit_.load(std::memory_order_acquire) && cancel_mode_.load(std::memory_order_relaxed);
    }

    /// Runs up to @p max_commands queued commands, checking before each one whether stop(kCancel) was
    /// called. @return how many ran.
    std::size_t run_batch(const std::size_t max_commands) noexcept {
        std::size_t ran{0U};
        Command command{};
        while ((ran < max_commands) && (!cancelling()) && queue_.try_pop(command)) {
            static_cast<void>(queued_.fetch_sub(1U, std::memory_order_acq_rel));
            command();
            command.reset();
            static_cast<void>(executed_.fetch_add(1U, std::memory_order_relaxed));
            ++ran;
        }
        return ran;
    }

    /// The thread's service loop: runs until it is told to exit.
    void service_loop() noexcept {
        const detail::RunnerScope scope{this};
        bool active{true};
        while (active) {
            const std::size_t ran{run_batch(QueueCapacity)};
            if (poll_ != nullptr) {
                poll_(MonotonicClock::now(), poll_context_);
            }
            heartbeat_.beat(MonotonicClock::now());
            const bool exiting{exit_.load(std::memory_order_acquire)};
            if (cancelling()) {
                active = false;
            } else if (exiting && (in_flight_.load(std::memory_order_seq_cst) == 0U) &&
                       (queued_.load(std::memory_order_acquire) == 0U)) {
                // Drained: no post is in flight and none can start (stopping_ is set), so nothing comes.
                active = false;
            } else if (ran > 0U) {
                // More commands may be waiting: no sleep.
            } else if (idle_sleep_ > Duration::zero()) {
                static_cast<void>(sleep_for(idle_sleep_));
            } else {
                cpu_relax();
            }
        }
    }

    /// Destroys the queued commands without running them, with no thread running and no post in flight.
    void discard_queued() noexcept {
        Command command{};
        for (std::size_t count{0U}; (count < QueueCapacity) && queue_.try_pop(command); ++count) {
            static_cast<void>(queued_.fetch_sub(1U, std::memory_order_acq_rel));
            command.reset();
            static_cast<void>(discarded_.fetch_add(1U, std::memory_order_relaxed));
        }
    }

    MpmcQueue<Command, QueueCapacity> queue_{};
    std::atomic<std::uint32_t> in_flight_{0U};  // post() calls between their two steps of the handshake.
    std::atomic<std::size_t> queued_{0U};       // Commands pushed or being pushed, and not yet popped.
    std::atomic<std::uint64_t> executed_{0U};
    std::atomic<std::uint64_t> discarded_{0U};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> exit_{false};
    std::atomic<bool> cancel_mode_{false};
    Duration idle_sleep_{std::chrono::milliseconds{1}};
    Poll poll_{nullptr};
    void* poll_context_{nullptr};
    bool running_{false};
    RtThread thread_{};
    Heartbeat heartbeat_{};
};

}  // namespace common

#endif  // COMMON_TASK_ACTIVE_OBJECT_HPP
