#ifndef CONFIG
#define CONFIG

#include "parse.hpp"
#include <cstdint>
#include <ranges>
#include <string>
#include <vector>

struct Peer {
    std::string host, port;
};

struct Config {
    uint8_t thMax = 6;
    std::string bbPort = "9000";
    std::string syncPort = "10000";
    std::string bbFile;
    std::vector<Peer> syncPeers;
    bool daemon = true;
    bool debug = false;
    bool delay = false;
};

template<>
constexpr std::optional<Peer> parse<Peer>(std::string_view str) {
    if (auto const peer = parse<NVPair>(str, ':')) return Peer{std::string{peer->name}, std::string{peer->value}};
    return {};
}

template<typename T>
constexpr auto convertTo = std::views::transform([](auto value) { return T{std::move(value)}; });

template<>
constexpr std::optional<Config> parse<Config>(std::string_view data) {
    Config config;
    for (auto const line : std::views::split(data, '\n') | convertTo<std::string_view>)
        if (auto const nvp = parse<NVPair>(line, '=')) {
            if (nvp->name == "THMAX")
                if (auto const val = parse<uint8_t>(nvp->value)) config.thMax = *val;
                else return {};
            else if (nvp->name == "BBPORT")
                if (parse<uint16_t>(nvp->value)) config.bbPort = nvp->value;
                else return {};
            else if (nvp->name == "SYNCPORT")
                if (parse<uint16_t>(nvp->value)) config.syncPort = nvp->value;
                else return {};
            else if (nvp->name == "BBFILE") config.bbFile = nvp->value;
            else if (nvp->name == "PEER")
                if (auto peer = parse<Peer>(nvp->value)) config.syncPeers.push_back(std::move(*peer));
                else return {};
            else if (nvp->name == "DAEMON")
                if (auto const val = parse<bool>(nvp->value)) config.daemon = *val;
                else return {};
            else if (nvp->name == "DEBUG")
                if (auto const val = parse<bool>(nvp->value)) config.debug = *val;
                else return {};
            else if (nvp->name == "DELAY") {
                if (auto const val = parse<bool>(nvp->value)) config.delay = *val;
                else return {};
            }
        }
    if (config.bbFile.empty()) return {};
    return config;
}

#endif
