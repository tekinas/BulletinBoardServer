#include "defer.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <optional>
#include <string_view>

inline bool isSpace(char ch) { return std::isspace(static_cast<unsigned char>(ch)); }

inline std::optional<std::string_view> getLine(std::string_view &buffer) {
    auto const ln = buffer.find_first_of('\n');
    if (ln == buffer.npos) return {};
    defer[&] { buffer.remove_prefix(ln + 1); };
    return buffer.substr(0, (ln != 0 and buffer[ln - 1] == '\r') ? ln - 1 : ln);
}

inline std::optional<std::string_view> getWord(std::string_view &buffer) {
    auto const wb = std::ranges::find_if_not(buffer, isSpace);
    auto const we = std::ranges::find_if(wb, buffer.end(), isSpace);
    size_t const pos = wb - buffer.begin(), wsize = we - wb;
    defer[&] { buffer.remove_prefix(pos + wsize); };
    if (not wsize) return {};
    return buffer.substr(pos, wsize);
}

inline std::optional<size_t> getNumber(std::string_view &buffer) {
    size_t number;
    auto const wb = std::ranges::find_if_not(buffer, isSpace);
    auto [ptr, ec] = std::from_chars(wb, buffer.end(), number);
    if (ec != std::errc{}) return {};
    buffer = buffer.substr(ptr - buffer.begin());
    return number;
}

inline std::string_view message(std::string_view buffer) {
    auto const wb = std::ranges::find_if_not(buffer, isSpace);
    if (wb == buffer.end()) return {};
    auto const we = std::ranges::find_last_if_not(buffer, isSpace).begin();
    return buffer.substr(wb - buffer.begin(), (we - wb) + 1);
}
