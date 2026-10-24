#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <utility>
#include <vector>

#include "common/core/status.hpp"
#include "common/time/clock.hpp"
#include "common/time/timer_wheel.hpp"
#include "test_framework.hpp"

using namespace std::chrono_literals;

namespace {

// Explicit time points make the tests deterministic.
constexpr common::TimePoint kT0{common::Duration{1'000'000'000}};

/// Records the ids of fired timers, in firing order.
struct Recorder final {
    std::vector<common::TimerId> fired;
};

void record(const common::TimerId id, void* const context) noexcept {
    static_cast<Recorder*>(context)->fired.push_back(id);
}

using SmallWheel = common::TimerWheel<8U, 8U>;

common::TimerId schedule(SmallWheel& wheel, const common::TimePoint deadline, Recorder& recorder) {
    return wheel.schedule(deadline, &record, &recorder).value_or(common::TimerId{});
}

}  // namespace

TEST_CASE(timer_wheel_fires_at_the_deadline_never_early) {
    SmallWheel wheel{1ms, kT0};
    Recorder recorder{};
    CHECK(wheel.tick() == 1ms);
    CHECK(SmallWheel::capacity() == 8U);
    const common::TimerId id{schedule(wheel, kT0 + 5ms + 500us, recorder)};
    CHECK(wheel.is_active(id));
    CHECK(wheel.active() == 1U);

    CHECK(wheel.advance(kT0 + 5ms, 10U) == 0U);
    CHECK(wheel.advance(kT0 + 5ms + 499us, 10U) == 0U);  // Same tick, before the exact deadline.
    CHECK(recorder.fired.empty());
    CHECK(wheel.advance(kT0 + 5ms + 500us, 10U) == 1U);
    REQUIRE(recorder.fired.size() == 1U);
    CHECK(recorder.fired[0U] == id);
    CHECK(!wheel.is_active(id));
    CHECK(wheel.active() == 0U);
    CHECK(wheel.advance(kT0 + 1s, 10U) == 0U);

    // A deadline that has passed fires on the next advance().
    const common::TimerId late{schedule(wheel, kT0, recorder)};
    CHECK(wheel.advance(kT0 + 1s, 10U) == 1U);
    CHECK(recorder.fired.back() == late);
}

TEST_CASE(timer_wheel_order_and_budget) {
    SmallWheel wheel{1ms, kT0};
    Recorder recorder{};
    // Scheduled out of order, in different slots and two in the same tick.
    const common::TimerId c{schedule(wheel, kT0 + 7ms, recorder)};
    const common::TimerId a{schedule(wheel, kT0 + 2ms + 100us, recorder)};
    const common::TimerId d{schedule(wheel, kT0 + 9ms, recorder)};
    const common::TimerId b{schedule(wheel, kT0 + 2ms + 50us, recorder)};
    const common::TimerId e{schedule(wheel, kT0 + 3ms, recorder)};

    // Budget 2: the two earliest; the rest stay due, in order, for the next calls.
    CHECK(wheel.advance(kT0 + 20ms, 2U) == 2U);
    REQUIRE(recorder.fired.size() == 2U);
    CHECK(recorder.fired[0U] == b);
    CHECK(recorder.fired[1U] == a);
    CHECK(wheel.active() == 3U);
    CHECK(wheel.is_active(e));  // Due but not fired: still active.

    // A due timer can still be cancelled.
    CHECK(common::is_ok(wheel.cancel(c)));
    CHECK(wheel.advance(kT0 + 20ms, 0U) == 0U);
    CHECK(wheel.advance(kT0 + 20ms, 10U) == 2U);
    REQUIRE(recorder.fired.size() == 4U);
    CHECK(recorder.fired[2U] == e);
    CHECK(recorder.fired[3U] == d);
    CHECK(wheel.active() == 0U);
}

TEST_CASE(timer_wheel_cancel_reschedule_and_stale_ids) {
    SmallWheel wheel{1ms, kT0};
    Recorder recorder{};
    const common::TimerId first{schedule(wheel, kT0 + 5ms, recorder)};
    CHECK(common::is_ok(wheel.cancel(first)));
    CHECK(wheel.cancel(first) == common::Status::kNotFound);

    // The entry is reused with a new generation; the old id cannot touch the new timer.
    const common::TimerId second{schedule(wheel, kT0 + 5ms, recorder)};
    CHECK(second.index == first.index);
    CHECK(second != first);
    CHECK(wheel.cancel(first) == common::Status::kNotFound);
    CHECK(wheel.reschedule(first, kT0 + 1ms) == common::Status::kNotFound);
    CHECK(wheel.is_active(second));
    CHECK(!wheel.is_active(common::TimerId{}));
    CHECK(wheel.cancel(common::TimerId{}) == common::Status::kNotFound);

    // Earlier and later.
    CHECK(common::is_ok(wheel.reschedule(second, kT0 + 2ms)));
    CHECK(wheel.advance(kT0 + 2ms, 10U) == 1U);
    const common::TimerId third{schedule(wheel, kT0 + 3ms, recorder)};
    CHECK(common::is_ok(wheel.reschedule(third, kT0 + 30ms)));
    CHECK(wheel.advance(kT0 + 29ms, 10U) == 0U);
    CHECK(wheel.advance(kT0 + 30ms, 10U) == 1U);
    CHECK((recorder.fired == std::vector<common::TimerId>{second, third}));
    CHECK(wheel.reschedule(third, kT0 + 40ms) == common::Status::kNotFound);  // Already fired.
}

TEST_CASE(timer_wheel_capacity_and_arguments) {
    SmallWheel wheel{1ms, kT0};
    Recorder recorder{};
    CHECK(wheel.schedule(kT0 + 1ms, nullptr, &recorder).status() == common::Status::kInvalidArgument);
    std::vector<common::TimerId> ids{};
    for (std::uint32_t i{0U}; i < 8U; ++i) {
        const common::Result<common::TimerId> id{wheel.schedule(kT0 + 1ms, &record, &recorder)};
        REQUIRE(id.has_value());
        ids.push_back(id.value());
    }
    CHECK(wheel.schedule(kT0 + 1ms, &record, &recorder).status() == common::Status::kResourceLimit);
    CHECK(common::is_ok(wheel.cancel(ids[3U])));
    CHECK(wheel.schedule(kT0 + 1ms, &record, &recorder).has_value());  // A free entry again.
    CHECK(wheel.advance(kT0 + 1ms, 100U) == 8U);
    CHECK(wheel.active() == 0U);
}

TEST_CASE(timer_wheel_long_timeouts_and_jumps) {
    SmallWheel wheel{1ms, kT0};  // One revolution is 8 ms.
    Recorder recorder{};
    const common::TimerId far{schedule(wheel, kT0 + 100ms, recorder)};
    std::size_t fired_early{0U};
    for (std::int64_t ms{1}; ms < 100; ++ms) {
        fired_early += wheel.advance(kT0 + std::chrono::milliseconds{ms}, 10U);
    }
    CHECK(fired_early == 0U);
    CHECK(wheel.advance(kT0 + 100ms, 10U) == 1U);
    CHECK(recorder.fired.back() == far);

    // After a long stall, one call fires everything that expired, in deadline order.
    recorder.fired.clear();
    const common::TimePoint base{kT0 + 100ms};
    const common::TimerId x{schedule(wheel, base + 50ms, recorder)};
    const common::TimerId y{schedule(wheel, base + 3ms, recorder)};
    const common::TimerId z{schedule(wheel, base + 1h, recorder)};
    const common::TimerId w{schedule(wheel, base + 17ms, recorder)};
    CHECK(wheel.advance(base + 10min, 10U) == 3U);
    CHECK((recorder.fired == std::vector<common::TimerId>{y, w, x}));
    CHECK(wheel.is_active(z));
    CHECK(wheel.advance(base + 1h, 10U) == 1U);

    // If now goes backwards, still only timers whose deadline has passed fire.
    const common::TimerId later{schedule(wheel, base + 3h, recorder)};
    CHECK(wheel.advance(base + 2h, 10U) == 0U);
    const common::TimerId between{schedule(wheel, base + 100min, recorder)};  // Before the wheel's cursor.
    CHECK(wheel.advance(base + 90min, 10U) == 0U);  // Backwards.
    CHECK(wheel.is_active(later));
    CHECK(wheel.is_active(between));
    CHECK(wheel.advance(base + 100min, 10U) == 1U);
    CHECK(recorder.fired.back() == between);
    CHECK(wheel.advance(base + 3h, 10U) == 1U);
    CHECK(recorder.fired.back() == later);

    // Deadlines before the wheel's start count as tick 0.
    const common::TimerId ancient{schedule(wheel, common::TimePoint{}, recorder)};
    CHECK(wheel.advance(base + 3h, 10U) == 1U);
    CHECK(recorder.fired.back() == ancient);
}

namespace {

/// Context of callbacks that act on their wheel.
struct Actor final {
    SmallWheel* wheel{nullptr};
    common::TimePoint rearm_at{};
    common::TimerId victim{};
    std::uint32_t calls{0U};
    common::Status cancel_status{common::Status::kOk};
};

void rearm(const common::TimerId /*id*/, void* const context) noexcept {
    Actor& actor{*static_cast<Actor*>(context)};
    ++actor.calls;
    // Already due: it must still wait for the next advance().
    static_cast<void>(actor.wheel->schedule(actor.rearm_at, &rearm, context));
}

void cancel_victim(const common::TimerId /*id*/, void* const context) noexcept {
    Actor& actor{*static_cast<Actor*>(context)};
    ++actor.calls;
    actor.cancel_status = actor.wheel->cancel(actor.victim);
}

}  // namespace

TEST_CASE(timer_wheel_callbacks_may_use_the_wheel) {
    SmallWheel wheel{1ms, kT0};
    Actor actor{};
    actor.wheel = &wheel;
    actor.rearm_at = kT0;  // In the past.
    REQUIRE(wheel.schedule(kT0 + 1ms, &rearm, &actor).has_value());
    CHECK(wheel.advance(kT0 + 5ms, 100U) == 1U);  // The re-armed timer does not fire in the same call.
    CHECK(actor.calls == 1U);
    CHECK(wheel.active() == 1U);
    CHECK(wheel.advance(kT0 + 5ms, 100U) == 1U);
    CHECK(actor.calls == 2U);

    // A callback cancels a timer that is due in the same call: it does not fire.
    SmallWheel second{1ms, kT0};
    Recorder recorder{};
    Actor canceller{};
    canceller.wheel = &second;
    REQUIRE(second.schedule(kT0 + 1ms, &cancel_victim, &canceller).has_value());
    canceller.victim = second.schedule(kT0 + 2ms, &record, &recorder).value_or(common::TimerId{});
    CHECK(second.advance(kT0 + 3ms, 100U) == 1U);
    CHECK(common::is_ok(canceller.cancel_status));
    CHECK(recorder.fired.empty());
    CHECK(second.active() == 0U);
}

TEST_CASE(timer_wheel_matches_reference_model) {
    common::TimerWheel<64U, 16U> wheel{1ms, kT0};
    Recorder recorder{};
    std::map<std::pair<std::uint32_t, std::uint32_t>, common::TimePoint> model{};  // Active timers.
    std::vector<common::TimerId> retired{};
    std::uint64_t state{12345U};
    const auto random = [&state](const std::uint64_t bound) {
        state = (state * 6364136223846793005U) + 1442695040888963407U;
        return (state >> 33U) % bound;
    };
    // A random multiple of 100 us below bound * 100 us.
    const auto random_micros = [&random](const std::uint64_t bound) {
        return std::chrono::microseconds{100 * static_cast<std::int64_t>(random(bound))};
    };
    const auto key = [](const common::TimerId id) { return std::make_pair(id.index, id.generation); };

    common::TimePoint now{kT0};
    std::uint32_t violations{0U};
    std::size_t total_fired{0U};
    for (std::uint32_t step{0U}; step < 20'000U; ++step) {
        const std::uint64_t action{random(10U)};
        if ((action < 4U) && (model.size() < 64U)) {
            // Deadlines from 5 ms ago to 300 ms ahead (more than one 16 ms revolution), in steps of 100 us.
            const common::TimePoint deadline{now - 5ms + random_micros(3050U)};
            const common::Result<common::TimerId> id{wheel.schedule(deadline, &record, &recorder)};
            violations += id.has_value() ? 0U : 1U;
            model[key(id.value_or(common::TimerId{}))] = deadline;
        } else if ((action < 6U) && (!model.empty())) {
            auto entry{model.begin()};
            std::advance(entry, static_cast<std::ptrdiff_t>(random(model.size())));
            const common::TimerId id{entry->first.first, entry->first.second};
            if (action == 4U) {
                violations += common::is_ok(wheel.cancel(id)) ? 0U : 1U;
                retired.push_back(id);
                model.erase(entry);
            } else {
                const common::TimePoint deadline{now + random_micros(1000U)};
                violations += common::is_ok(wheel.reschedule(id, deadline)) ? 0U : 1U;
                entry->second = deadline;
            }
        } else if ((action == 6U) && (!retired.empty())) {
            const common::TimerId stale{retired[random(retired.size())]};
            violations += (wheel.cancel(stale) == common::Status::kNotFound) ? 0U : 1U;
        } else {
            // Advance by up to 3 ms, sometimes by half a second, with a random budget.
            now += (random(50U) == 0U) ? common::Duration{500ms} : common::Duration{random_micros(30U)};
            const std::size_t budget{(random(4U) == 0U) ? std::size_t{1000U} : (1U + random(6U))};
            std::size_t due{0U};
            for (const auto& entry : model) {
                due += (entry.second <= now) ? 1U : 0U;
            }
            recorder.fired.clear();
            const std::size_t fired{wheel.advance(now, budget)};
            total_fired += fired;
            violations += (fired == ((due < budget) ? due : budget)) ? 0U : 1U;
            violations += (fired == recorder.fired.size()) ? 0U : 1U;
            common::TimePoint previous{common::TimePoint::min()};
            for (const common::TimerId id : recorder.fired) {
                const auto entry{model.find(key(id))};
                if (entry == model.end()) {
                    ++violations;  // Not an active timer.
                } else {
                    violations += (entry->second <= now) ? 0U : 1U;        // Never early.
                    violations += (entry->second >= previous) ? 0U : 1U;  // In deadline order.
                    previous = entry->second;
                    violations += wheel.is_active(id) ? 1U : 0U;
                    retired.push_back(id);
                    model.erase(entry);
                }
            }
            // The fired timers were the earliest due ones: none left behind is earlier.
            for (const auto& entry : model) {
                violations += ((entry.second <= now) && (entry.second < previous)) ? 1U : 0U;
            }
        }
        violations += (wheel.active() == model.size()) ? 0U : 1U;
        if (retired.size() > 256U) {
            retired.erase(retired.begin(), retired.begin() + 128);
        }
    }
    CHECK(violations == 0U);
    CHECK(total_fired > 1000U);
}
