#include <cstdint>
#include <string>

#include "cyane/proto/json.hpp"
#include "test_framework.hpp"

CYANE_TEST(json_escapes_quotes_and_backslashes) {
    CYANE_CHECK_EQ(cyane::proto::json_string(R"(say "hi")"), std::string{R"("say \"hi\"")"});
    CYANE_CHECK_EQ(cyane::proto::json_string(R"(back\slash)"), std::string{R"("back\\slash")"});
}

CYANE_TEST(json_escapes_control_characters) {
    CYANE_CHECK_EQ(cyane::proto::json_string("a\nb"), std::string{R"("a\nb")"});
    CYANE_CHECK_EQ(cyane::proto::json_string("a\tb"), std::string{R"("a\tb")"});
    CYANE_CHECK_EQ(cyane::proto::json_string(std::string{"a\0b", 3}), std::string{R"("a\u0000b")"});
    CYANE_CHECK_EQ(cyane::proto::json_string("\x01"), std::string{R"("\u0001")"});
}

CYANE_TEST(json_passes_utf8_and_section_sign_through) {
    CYANE_CHECK_EQ(cyane::proto::json_string("§a绿色"), std::string{"\"§a绿色\""});
}

CYANE_TEST(json_chat_text_wraps_in_component) {
    CYANE_CHECK_EQ(cyane::proto::chat_text("hi"), std::string{R"({"text":"hi"})"});
    CYANE_CHECK_EQ(cyane::proto::chat_text(R"(a"b)"), std::string{R"({"text":"a\"b"})"});
}
