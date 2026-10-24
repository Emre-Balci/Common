#include <cstdint>
#include <cstring>
#include <string_view>

#include "common/containers/span.hpp"
#include "common/containers/static_string.hpp"
#include "test_framework.hpp"

TEST_CASE(static_string_append) {
    common::StaticString<8U> text{};
    CHECK(text.empty());
    CHECK(text.size() == 0U);
    CHECK(std::strcmp(text.c_str(), "") == 0);
    CHECK(common::StaticString<8U>::capacity() == 8U);

    CHECK(text.append("AT"));
    CHECK(text.append(' '));
    CHECK(text.append(std::string_view{"Z"}));
    CHECK(text.size() == 4U);
    CHECK(text.view() == "AT Z");
    CHECK(std::strcmp(text.c_str(), "AT Z") == 0);
    CHECK(text[0U] == 'A');
    CHECK(text.text().size() == 4U);
    CHECK(text.bytes().size() == 4U);
    CHECK(text.bytes()[3U] == static_cast<std::uint8_t>('Z'));

    // An append that does not fit fails as a whole and changes nothing.
    CHECK(!text.append("12345"));
    CHECK(text.view() == "AT Z");
    CHECK(text.append("1234"));
    CHECK(text.full());
    CHECK(!text.append('x'));
    CHECK(std::strcmp(text.c_str(), "AT Z1234") == 0);

    text.truncate(2U);
    CHECK(text.view() == "AT");
    CHECK(std::strcmp(text.c_str(), "AT") == 0);
    text.clear();
    CHECK(text.empty());
}

TEST_CASE(static_string_assign_and_span) {
    common::StaticString<4U> text{};
    CHECK(text.assign("abc"));
    CHECK(text.view() == "abc");
    CHECK(!text.assign("abcde"));
    CHECK(text.view() == "abc");  // Unchanged.

    // Appending a view of itself is allowed: the source lies before the written part.
    common::StaticString<6U> twice{};
    CHECK(twice.assign("abc"));
    CHECK(twice.append(twice.text()));
    CHECK(twice.view() == "abcabc");

    // A NUL inside the string counts as a character.
    common::StaticString<4U> with_nul{};
    CHECK(with_nul.append('a'));
    CHECK(with_nul.append('\0'));
    CHECK(with_nul.size() == 2U);

    constexpr std::string_view kBuilt{[]() {
        common::StaticString<4U> built{};
        static_cast<void>(built.append("ok"));
        return built.size();
    }() == 2U ? "constexpr" : "no"};
    CHECK(kBuilt == "constexpr");
}
