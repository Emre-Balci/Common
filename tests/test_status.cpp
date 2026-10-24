#include <cstdint>
#include <cstring>

#include "common/status.hpp"
#include "test_framework.hpp"

namespace {

common::Result<std::int32_t> parse_digit(const char c) noexcept {
    return ((c >= '0') && (c <= '9')) ? common::Result<std::int32_t>::success(c - '0')
                                      : common::Result<std::int32_t>::failure(common::Status::kInvalidArgument);
}

}  // namespace

TEST_CASE(result_success_and_failure) {
    const common::Result<std::int32_t> ok{parse_digit('7')};
    CHECK(ok.has_value());
    CHECK(ok.value() == 7);
    CHECK(common::is_ok(ok.status()));

    const common::Result<std::int32_t> bad{parse_digit('x')};
    CHECK(!bad.has_value());
    CHECK(bad.status() == common::Status::kInvalidArgument);
    CHECK(bad.value_or(-1) == -1);

    static_assert(common::Result<std::int32_t>::success(3).value() == 3, "Result is constexpr");
    CHECK(std::strcmp(common::to_string(common::Status::kDeadlineMissed), "deadline missed") == 0);
}
