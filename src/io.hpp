#ifndef IO_UTIL
#define IO_UTIL

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "defer.hpp"
#include <cassert>
#include <climits>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace io {
using IPv4Addr = in_addr;
using IPv4SocketAddr = sockaddr_in;
using IPv6Addr = in6_addr;
using IPv6SocketAddr = sockaddr_in6;

class SystemError {
public:
    int id() const { return err; }

    std::string_view str() const { return std::strerror(err); }

    auto unexpect() const { return std::unexpected{*this}; }

    bool operator==(int ec) const { return ec == err; }

    friend auto sysError();

private:
    SystemError(int err) : err{err} {}

    int err;
};

class GAIError {
public:
    int id() const { return err; }

    std::string_view str() const { return ::gai_strerror(err); }

    auto unexpect() const { return std::unexpected{*this}; }

    bool operator==(int ec) const { return ec == err; }

    friend auto gaiError(int status);

private:
    GAIError(int err) : err{err} {}

    int err;
};

inline auto sysError() { return std::unexpected{SystemError{errno}}; }

inline auto gaiError(int status) { return std::unexpected{GAIError{status}}; }

class SocketAddress {
public:
    SocketAddress() = default;

    SocketAddress(sockaddr const *src, socklen_t len) : len{len} { std::memcpy(&addr, src, len); }

    SocketAddress(IPv4SocketAddr const &src) : len{sizeof src} { std::memcpy(&addr, &src, len); }

    SocketAddress(IPv6SocketAddr const &src) : len{sizeof src} { std::memcpy(&addr, &src, len); }

    auto address() const { return reinterpret_cast<sockaddr const *>(&addr); }

    auto length() const { return len; }

    bool isIpv4() const { return addr.ss_family == AF_INET; }

    bool isIpv6() const { return addr.ss_family == AF_INET6; }

    auto toIpv4() const {
        assert(isIpv4());
        IPv4SocketAddr res;
        std::memcpy(&res, &addr, sizeof res);
        return res;
    }

    auto toIpv6() const {
        assert(isIpv6());
        IPv6SocketAddr res;
        std::memcpy(&res, &addr, sizeof res);
        return res;
    }

    sockaddr *addressPtr() { return reinterpret_cast<sockaddr *>(&addr); }

    socklen_t *lengthPtr() {
        len = sizeof addr;
        return &len;
    }

private:
    sockaddr_storage addr;
    socklen_t len;
};

struct AddressInfo {
    int family;
    int socktype;
    int protocol;
    SocketAddress addr;
};

inline std::expected<int, SystemError> open(char const *file, int flags, mode_t mode) {
    if (auto const fd = ::open(file, flags, mode); fd != -1) return fd;
    return sysError();
}

inline std::expected<int, SystemError> open(int dirfd, char const *file, int flags, mode_t mode) {
    if (auto const fd = ::openat(dirfd, file, flags, mode); fd != -1) return fd;
    return sysError();
}

inline std::expected<void, SystemError> fcntl(int fd, int op, int flags) {
    if (::fcntl(fd, op, flags) == 0) return {};
    return sysError();
}

inline std::expected<void, SystemError> close(int fd) {
    if (::close(fd) == 0) return {};
    return sysError();
}

inline std::expected<IPv4Addr, SystemError> ipv4(char const *addr) {
    IPv4Addr dst;
    if (::inet_pton(AF_INET, addr, &dst) == 1) return dst;
    return sysError();
}

inline std::expected<IPv6Addr, SystemError> ipv6(char const *addr) {
    IPv6Addr dst;
    if (::inet_pton(AF_INET6, addr, &dst) == 1) return dst;
    return sysError();
}

inline std::expected<void, SystemError> toString(IPv4Addr addr, std::string &str) {
    str.resize_and_overwrite(INET_ADDRSTRLEN, [&](char *ptr, size_t) {
        return ::inet_ntop(AF_INET, &addr, ptr, INET_ADDRSTRLEN) ? std::strlen(ptr) : 0;
    });
    if (str.size()) return {};
    return sysError();
}

inline std::expected<void, SystemError> toString(IPv6Addr addr, std::string &str) {
    str.resize_and_overwrite(INET6_ADDRSTRLEN, [&](char *ptr, size_t) {
        return ::inet_ntop(AF_INET6, &addr, ptr, INET6_ADDRSTRLEN) ? std::strlen(ptr) : 0;
    });
    if (str.size()) return {};
    return sysError();
}

inline std::expected<void, GAIError> toString(SocketAddress const &sa, std::string &host, std::string &serv,
                                              int flags = NI_NUMERICHOST | NI_NUMERICSERV) {
    int status;
    host.resize_and_overwrite(NI_MAXHOST, [&](char *hostPtr, size_t) {
        serv.resize_and_overwrite(NI_MAXSERV, [&](char *servPtr, size_t) {
            return (status = ::getnameinfo(sa.address(), sa.length(), hostPtr, NI_MAXHOST, servPtr, NI_MAXSERV,
                                           flags)) == 0
                           ? std::strlen(servPtr)
                           : 0;
        });
        return status == 0 ? std::strlen(hostPtr) : 0;
    });
    if (status == 0) return {};
    return gaiError(status);
}

inline std::expected<void, GAIError> getAddressInfo(char const *node, char const *service,
                                                    std::vector<AddressInfo> &ainfos, int family = AF_UNSPEC,
                                                    int socktype = 0, int protocol = 0,
                                                    int flags = AI_V4MAPPED | AI_ADDRCONFIG) {
    addrinfo hints{};
    hints.ai_flags = flags;
    hints.ai_family = family;
    hints.ai_socktype = socktype;
    hints.ai_protocol = protocol;
    addrinfo *res;
    auto const status = ::getaddrinfo(node, service, &hints, &res);
    if (status != 0) return gaiError(status);
    ainfos.clear();
    for (auto info = res; info; info = info->ai_next)
        ainfos.emplace_back(info->ai_family, info->ai_socktype, info->ai_protocol,
                            SocketAddress{info->ai_addr, info->ai_addrlen});
    freeaddrinfo(res);
    return {};
}

inline std::expected<SocketAddress, SystemError> socketName(int fd) {
    if (SocketAddress sa; ::getsockname(fd, sa.addressPtr(), sa.lengthPtr()) == 0) return sa;
    return sysError();
}

inline std::expected<SocketAddress, SystemError> peerName(int fd) {
    if (SocketAddress sa; ::getpeername(fd, sa.addressPtr(), sa.lengthPtr()) == 0) return sa;
    return sysError();
}

inline std::expected<void, SystemError> getHostName(std::string &str) {
    str.resize_and_overwrite(HOST_NAME_MAX, [](char *ptr, size_t) {
        return ::gethostname(ptr, HOST_NAME_MAX) == 0 ? std::strlen(ptr) : 0;
    });
    if (str.size()) return {};
    return sysError();
}

inline std::expected<void, SystemError> setHostName(std::string_view name) {
    if (::sethostname(name.data(), name.size()) == 0) return {};
    return sysError();
}

inline std::expected<int, SystemError> socket(int domain, int type, int protocol) {
    if (auto const fd = ::socket(domain, type, protocol); fd != -1) return fd;
    return sysError();
}

inline auto socket(AddressInfo const &ai) { return socket(ai.family, ai.socktype, ai.protocol); }

inline std::expected<void, SystemError> bind(int fd, SocketAddress const &sa) {
    if (::bind(fd, sa.address(), sa.length()) == 0) return {};
    return sysError();
}

inline std::expected<void, SystemError> connect(int fd, SocketAddress const &sa) {
    if (::connect(fd, sa.address(), sa.length()) == 0) return {};
    return sysError();
}

inline std::expected<void, SystemError> listen(int fd, int backlog) {
    if (::listen(fd, backlog) == 0) return {};
    return sysError();
}

inline std::expected<int, SystemError> accept(int fd, SocketAddress &sa) {
    if (auto const clientFd = ::accept(fd, sa.addressPtr(), sa.lengthPtr()); clientFd != -1) return clientFd;
    return sysError();
}

inline std::expected<int, SystemError> accept(int fd) {
    if (auto const clientFd = ::accept(fd, nullptr, nullptr); clientFd != -1) return clientFd;
    return sysError();
}

inline std::expected<size_t, SystemError> recv(int fd, void *buf, size_t n, int flags) {
    if (auto const nread = ::recv(fd, buf, n, flags); nread != -1) return nread;
    return sysError();
}

inline std::expected<size_t, SystemError> recv(int fd, void *buf, size_t n, int flags, SocketAddress &sa) {
    if (auto const nread = ::recvfrom(fd, buf, n, flags, sa.addressPtr(), sa.lengthPtr()); nread != -1) return nread;
    return sysError();
}

inline std::expected<size_t, SystemError> send(int fd, void const *buf, size_t n, int flags) {
    if (auto const nsent = ::send(fd, buf, n, flags); nsent != -1) return nsent;
    return sysError();
}

inline std::expected<size_t, SystemError> send(int fd, void const *buf, size_t n, int flags, SocketAddress const &sa) {
    if (auto const nsent = ::sendto(fd, buf, n, flags, sa.address(), sa.length()); nsent != -1) return nsent;
    return sysError();
}

inline std::expected<void, SystemError> shutdown(int fd, int how) {
    if (::shutdown(fd, how) == 0) return {};
    return sysError();
}

inline std::expected<int, SystemError> poll(std::span<pollfd> fds,
                                            std::optional<std::chrono::milliseconds> timeout = {}) {
    if (auto const nready = ::poll(fds.data(), fds.size(), timeout ? timeout->count() : -1); nready != -1)
        return nready;
    return sysError();
}

inline std::expected<struct stat, SystemError> fstat(int fd) {
    if (struct stat st; fstat(fd, &st) == 0) return st;
    return sysError();
}

inline std::expected<void, SystemError> ftruncate(int fd, size_t length) {
    if (::ftruncate(fd, length) != -1) return {};
    return sysError();
}

inline std::expected<size_t, SystemError> read(int fd, char *buf, size_t n) {
    if (auto const nread = ::read(fd, buf, n); nread != -1) return nread;
    return sysError();
}

inline std::expected<size_t, SystemError> readN(int fd, char *buf, size_t n) {
    for (size_t nleft = n; nleft != 0;) {
        auto const nread = ::read(fd, buf, nleft);
        if (nread == -1)
            if (errno == EINTR) continue;
            else return sysError();
        else if (nread == 0) return n - nleft;
        nleft -= nread;
        buf += nread;
    }
    return n;
}

inline std::expected<size_t, SystemError> readNAt(int fd, char *buf, size_t n, off_t pos) {
    for (size_t nleft = n; nleft != 0;) {
        auto const nread = ::pread(fd, buf, nleft, pos);
        if (nread == -1)
            if (errno == EINTR) continue;
            else return sysError();
        else if (nread == 0) return n - nleft;
        nleft -= nread;
        buf += nread;
        pos += nread;
    }
    return n;
}

inline std::expected<size_t, SystemError> writeN(int fd, char const *buf, size_t n) {
    for (size_t nleft = n; nleft != 0;) {
        auto const nwritten = ::write(fd, buf, nleft);
        if (nwritten == -1) {
            if (errno == EINTR) continue;
            else return sysError();
        }
        nleft -= nwritten;
        buf += nwritten;
    }
    return n;
}

inline std::expected<size_t, SystemError> writeNAt(int fd, char const *buf, size_t n, off_t pos) {
    for (size_t nleft = n; nleft != 0;) {
        auto const nwritten = ::pwrite(fd, buf, nleft, pos);
        if (nwritten == -1) {
            if (errno == EINTR) continue;
            else return sysError();
        }
        nleft -= nwritten;
        buf += nwritten;
        pos += nwritten;
    }
    return n;
}

inline std::expected<size_t, SystemError> readTo(std::string &str, int fd, size_t n) {
    std::expected<size_t, SystemError> res;
    str.resize_and_overwrite(n, [&](char *ptr, size_t) {
        res = readN(fd, ptr, n);
        return res ? *res : 0;
    });
    return res;
}

inline std::expected<size_t, SystemError> readAppend(std::string &str, int fd, size_t n) {
    auto const prev_size = str.size();
    std::expected<size_t, SystemError> res;
    str.resize_and_overwrite(prev_size + n, [&](char *ptr, size_t) {
        res = readN(fd, ptr + prev_size, n);
        return prev_size + (res ? *res : 0);
    });
    return res;
}

inline std::expected<size_t, SystemError> readTo(std::string &str, int fd, size_t n, off_t pos) {
    std::expected<size_t, SystemError> res;
    str.resize_and_overwrite(n, [&](char *ptr, size_t) {
        res = readNAt(fd, ptr, n, pos);
        return res ? *res : 0;
    });
    return res;
}

inline std::expected<size_t, SystemError> readAppend(std::string &str, int fd, size_t n, off_t pos) {
    auto const prev_size = str.size();
    std::expected<size_t, SystemError> res;
    str.resize_and_overwrite(prev_size + n, [&](char *ptr, size_t) {
        res = readNAt(fd, ptr + prev_size, n, pos);
        return prev_size + (res ? *res : 0);
    });
    return res;
}

inline std::expected<int, std::variant<GAIError, SystemError>> tcpListener(char const *port, int family = AF_UNSPEC,
                                                                           int backlog = SOMAXCONN) {
    std::vector<AddressInfo> ainfos;
    auto const res =
            getAddressInfo(nullptr, port, ainfos, family, SOCK_STREAM, 0, AI_PASSIVE | AI_ADDRCONFIG | AI_NUMERICSERV);
    if (not res) return res.error().unexpect();
    for (auto const &addrInfo : ainfos) {
        auto const soc = socket(addrInfo);
        if (not soc) continue;
        if (int optVal; ::setsockopt(*soc, SOL_SOCKET, SO_REUSEADDR, &optVal, sizeof optVal) != 0) return sysError();
        if (bind(*soc, addrInfo.addr) and listen(*soc, backlog)) return *soc;
        ::close(*soc);
    }
    return sysError();
}

inline std::expected<int, std::variant<GAIError, SystemError>> tcpConnect(char const *host, char const *port,
                                                                          int family = AF_UNSPEC) {
    std::vector<AddressInfo> ainfos;
    auto const res = getAddressInfo(host, port, ainfos, family, SOCK_STREAM, 0, AI_ADDRCONFIG | AI_NUMERICSERV);
    if (not res) return res.error().unexpect();
    for (auto const &addrInfo : ainfos) {
        auto const soc = socket(addrInfo);
        if (not soc) continue;
        if (connect(*soc, addrInfo.addr)) return *soc;
        ::close(*soc);
    }
    return sysError();
}

inline std::expected<void, SystemError> setNonBlock(int fd) { return fcntl(fd, F_SETFL, O_NONBLOCK); }

inline std::optional<std::string> fileData(char const *fileName) {
    auto const fd = io::open(fileName, O_RDONLY, 0);
    if (not fd) return {};
    defer[&] { ::close(*fd); };
    auto const stat = io::fstat(*fd);
    if (not stat or not(stat->st_mode & S_IRUSR)) return {};
    size_t const fileSize = stat->st_size;
    if (fileSize == 0) return "";
    if (std::string str; io::readTo(str, *fd, fileSize) and str.size() == fileSize) return str;
    return {};
}

inline bool isEAgain(auto error) { return error == EAGAIN or error == EWOULDBLOCK; }

inline bool writeToSocket(int fd, std::span<char const> buffer) {
    return static_cast<bool>(io::writeN(fd, buffer.data(), buffer.size()));
}

inline bool readFromSocket(int fd, std::string &str) {
    std::array<char, 1024> buffer;
    while (true) {
        auto const nr = io::recv(fd, buffer.data(), buffer.size(), MSG_DONTWAIT);
        if (nr and *nr) str += std::string_view{buffer.data(), *nr};
        else return not nr and isEAgain(nr.error());
    }
}
}// namespace io

#endif
