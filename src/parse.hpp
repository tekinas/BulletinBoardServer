#ifndef PARSE
#define PARSE

#include <charconv>
#include <optional>
#include <string_view>

inline auto cmd_line_args(int argc, char **argv) {
    return [=](int i) -> std::optional<std::string_view> {
        if (i < argc) return argv[i];
        return {};
    };
}

template<typename value_type>
std::optional<value_type> parse(std::string_view, auto...) = delete;

template<typename value_type>
    requires std::is_arithmetic_v<value_type>
constexpr std::optional<value_type> parse(std::string_view str) {
    if (value_type value; std::from_chars(str.data(), str.data() + str.size(), value).ec == std::errc{}) return value;
    return {};
}

template<>
constexpr std::optional<bool> parse<bool>(std::string_view str) {
    if (str == "true" or str == "1") return true;
    else if (str == "false" or str == "0") return false;
    return {};
}

struct NVPair {
    std::string_view name, value;
};

template<>
constexpr std::optional<NVPair> parse<NVPair>(std::string_view str, char sep) {
    if (auto const equalPos = str.find(sep); equalPos != std::string_view::npos)
        return NVPair{str.substr(0, equalPos), str.substr(equalPos + 1)};
    return {};
}

#endif
