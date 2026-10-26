#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "common/concurrency/spsc_queue.hpp"
#include "common/core/status.hpp"
#include "common/functional/inplace_function.hpp"
#include "test_framework.hpp"

namespace {

using VoidFunction = common::InplaceFunction<void()>;
using IntFunction = common::InplaceFunction<std::int32_t(std::int32_t)>;

/// Counts its live instances, copies and moves, to check that every stored callable is destroyed
/// exactly once.
struct Counters final {
    std::int32_t live{0};
    std::int32_t copies{0};
    std::int32_t moves{0};
    std::int32_t calls{0};
};

class CountingCallable final {
public:
    explicit CountingCallable(Counters& counters) noexcept : counters_{&counters} {
        ++counters_->live;
    }
    CountingCallable(const CountingCallable& other) noexcept : counters_{other.counters_} {
        ++counters_->live;
        ++counters_->copies;
    }
    CountingCallable(CountingCallable&& other) noexcept : counters_{other.counters_} {
        ++counters_->live;
        ++counters_->moves;
    }
    CountingCallable& operator=(const CountingCallable&) = delete;
    CountingCallable& operator=(CountingCallable&&) = delete;
    ~CountingCallable() {
        --counters_->live;
    }

    void operator()() const noexcept {
        ++counters_->calls;
    }

private:
    Counters* counters_;
};

std::int32_t twice(const std::int32_t value) noexcept {
    return 2 * value;
}

class Accumulator final {
public:
    void add(const std::int32_t value) noexcept {
        total_ += value;
    }
    [[nodiscard]] std::int32_t total() const noexcept {
        return total_;
    }
    std::int32_t total_{0};
};

// Compile-time properties the class promises.
static_assert(std::is_nothrow_default_constructible<VoidFunction>::value, "");
static_assert(std::is_nothrow_copy_constructible<VoidFunction>::value, "");
static_assert(std::is_nothrow_move_constructible<VoidFunction>::value, "");
static_assert(std::is_nothrow_copy_assignable<VoidFunction>::value, "");
static_assert(std::is_nothrow_move_assignable<VoidFunction>::value, "");
static_assert(!std::is_convertible<void (*)() noexcept, VoidFunction>::value, "the converting constructor is explicit");
static_assert(sizeof(VoidFunction) == 48U, "32 bytes of storage and a table pointer, aligned to 16");
static_assert(alignof(VoidFunction) == common::kInplaceFunctionAlignment, "");
static_assert(VoidFunction::capacity() == common::kInplaceFunctionCapacity, "");

struct Big final {
    std::array<std::byte, 64U> bytes{};
    void operator()() const noexcept {}
};
struct alignas(32) OverAligned final {
    void operator()() const noexcept {}
};
struct Throwing final {
    void operator()() const {}
};
struct Mutating final {
    void operator()() noexcept {}  // not const
};
struct ReturnsStatus final {
    common::Status operator()() const noexcept {
        return common::Status::kOk;
    }
};
struct ReturnsWide final {
    std::int64_t operator()(std::int32_t) const noexcept {
        return 0;
    }
};
struct ReturnsExact final {
    std::int32_t operator()(std::int32_t) const noexcept {
        return 0;
    }
};

static_assert(!VoidFunction::fits<Big>(), "too big");
static_assert(!VoidFunction::fits<OverAligned>(), "over-aligned");
static_assert(!VoidFunction::fits<Throwing>(), "may throw");
static_assert(!VoidFunction::fits<Mutating>(), "not callable as const");
static_assert(!VoidFunction::fits<ReturnsStatus>(), "a Status would be dropped");
static_assert(!IntFunction::fits<ReturnsWide>(), "int64_t only converts to int32_t");
static_assert(IntFunction::fits<ReturnsExact>(), "");
static_assert(IntFunction::fits<decltype(&twice)>(), "");
static_assert(common::InplaceFunction<void(Accumulator&, std::int32_t)>::fits<decltype(&Accumulator::add)>(), "");

}  // namespace

TEST_CASE(inplace_function_empty_state) {
    const VoidFunction empty{};
    CHECK(empty.empty());
    CHECK(!static_cast<bool>(empty));

    VoidFunction function{[]() noexcept {}};
    CHECK(!function.empty());
    CHECK(static_cast<bool>(function));
    function.reset();
    CHECK(function.empty());
    function.reset();  // Resetting an empty function does nothing.
    CHECK(function.empty());
}

TEST_CASE(inplace_function_calls_lambdas_with_captures) {
    std::int32_t total{0};
    std::int32_t step{5};  // Not const: a constant would not need capturing (Clang -Wunused-lambda-capture).
    const common::InplaceFunction<void(std::int32_t)> add{[&total, step](const std::int32_t value) noexcept {
        total += value + step;
    }};
    add(1);
    add(2);
    CHECK(total == 13);

    const IntFunction scale{[step](const std::int32_t value) noexcept { return value * step; }};
    CHECK(scale(3) == 15);
}

TEST_CASE(inplace_function_passes_arguments_as_declared) {
    std::int32_t target{0};
    const common::InplaceFunction<void(std::int32_t&)> by_reference{[](std::int32_t& value) noexcept { value = 7; }};
    by_reference(target);
    CHECK(target == 7);

    const common::InplaceFunction<std::int32_t(const std::array<std::int32_t, 4U>&)> by_const_reference{
        [](const std::array<std::int32_t, 4U>& values) noexcept { return values[0U] + values[3U]; }};
    const std::array<std::int32_t, 4U> values{{1, 2, 3, 4}};
    CHECK(by_const_reference(values) == 5);
}

TEST_CASE(inplace_function_stores_function_and_member_pointers) {
    const IntFunction doubled{&twice};
    CHECK(!doubled.empty());
    CHECK(doubled(21) == 42);

    using Function = std::int32_t (*)(std::int32_t) noexcept;
    const Function no_function{nullptr};
    const IntFunction from_null{no_function};
    CHECK(from_null.empty());

    Accumulator accumulator{};
    const common::InplaceFunction<void(Accumulator&, std::int32_t)> add{&Accumulator::add};
    add(accumulator, 4);
    add(accumulator, 5);
    CHECK(accumulator.total() == 9);

    using Member = void (Accumulator::*)(std::int32_t) noexcept;
    const Member no_member{nullptr};
    const common::InplaceFunction<void(Accumulator&, std::int32_t)> from_null_member{no_member};
    CHECK(from_null_member.empty());

    const common::InplaceFunction<std::int32_t(const Accumulator&)> read_member{&Accumulator::total_};
    CHECK(read_member(accumulator) == 9);
}

TEST_CASE(inplace_function_from_another_capacity) {
    using Small = common::InplaceFunction<void(), 8U>;
    const Small empty_small{};
    const VoidFunction from_empty{empty_small};
    CHECK(from_empty.empty());  // As std::function does for an empty std::function.

    std::int32_t calls{0};
    const Small small{[&calls]() noexcept { ++calls; }};
    const VoidFunction from_small{small};
    CHECK(!from_small.empty());
    from_small();
    CHECK(calls == 1);
}

TEST_CASE(inplace_function_copies_moves_and_destroys_exactly_once) {
    Counters counters{};
    {
        VoidFunction first{CountingCallable{counters}};
        CHECK(counters.live == 1);  // The temporary is gone; the stored copy lives.

        VoidFunction copy{first};
        CHECK(counters.live == 2);
        CHECK(counters.copies == 1);

        VoidFunction moved{std::move(copy)};
        CHECK(counters.live == 2);  // Moving destroys the source's callable.
        CHECK(copy.empty());        // NOLINT(bugprone-use-after-move): documented state.
        CHECK(!moved.empty());

        moved();
        first();
        CHECK(counters.calls == 2);

        first = moved;  // Copy assignment destroys first's callable, copies moved's.
        CHECK(counters.live == 2);

        VoidFunction other{};
        other = std::move(first);
        CHECK(counters.live == 2);
        CHECK(first.empty());  // NOLINT(bugprone-use-after-move): documented state.

        other = VoidFunction{};  // Assigning an empty function destroys the callable.
        CHECK(counters.live == 1);
    }
    CHECK(counters.live == 0);
}

TEST_CASE(inplace_function_self_assignment) {
    Counters counters{};
    {
        VoidFunction function{CountingCallable{counters}};
        VoidFunction& alias{function};
        function = alias;
        CHECK(counters.live == 1);
        function = std::move(alias);
        CHECK(counters.live == 1);
        CHECK(!function.empty());
        function();
        CHECK(counters.calls == 1);
    }
    CHECK(counters.live == 0);
}

TEST_CASE(inplace_function_fills_its_capacity) {
    struct Full final {
        std::array<std::int32_t, common::kInplaceFunctionCapacity / sizeof(std::int32_t)> values{};
        std::int32_t operator()() const noexcept {
            std::int32_t sum{0};
            for (const std::int32_t value : values) {
                sum += value;
            }
            return sum;
        }
    };
    static_assert(sizeof(Full) == common::kInplaceFunctionCapacity, "");
    Full full{};
    full.values.fill(2);
    const common::InplaceFunction<std::int32_t()> sum{full};
    CHECK(sum() == static_cast<std::int32_t>(2U * full.values.size()));
}

TEST_CASE(inplace_function_as_queue_element) {
    static common::SpscQueue<VoidFunction, 8U> queue{};
    std::int32_t total{0};
    for (std::int32_t i{1}; i <= 4; ++i) {
        CHECK(queue.try_push(VoidFunction{[&total, i]() noexcept { total += i; }}));
    }
    VoidFunction command{};
    std::int32_t popped{0};
    for (std::int32_t i{0}; (i < 8) && queue.try_pop(command); ++i) {
        command();
        ++popped;
    }
    CHECK(popped == 4);
    CHECK(total == 10);
}
