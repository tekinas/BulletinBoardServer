#include "bullentin_board.hpp"
#include "concurrent_queue.hpp"
#include "config.hpp"
#include "daemon.hpp"
#include "io.hpp"
#include "string.hpp"

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <format>
#include <iterator>
#include <memory>
#include <mutex>
#include <print>
#include <shared_mutex>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <variant>

inline constexpr std::string_view usageGuide = R"(Commands:
HELP : Display available commands.
USER <name> : change the user name to <name>.
SIZE : query total number of messages in the Bulletin Board.
READ <msg-number> : query the message with number <msg-number>.
LIST <first> <last> : List all messages in the range [first, last].
WRITE <message> : write the message to the Bulletin Board and get its message number.
REPLACE <msg-number>/<message> : replace the message with msg-number with the given message.
QUIT : quit the current session.

Notes:
* User name should consist of alphanumeric characters.
* Messages should consist of printable characters.
* Any whitespaces in the front or the back of the message string is removed.
* Messages are assigned numbers in sequence they are written, starting with 0. So if the total number of messages written are N then the last message written will have number N-1.
)";

enum class FdState { IO, PROCESS, CLOSE };

using namespace std::string_view_literals;

namespace chr = std::chrono;

struct ClientContext {
    std::string name;
    std::string readBuffer;
    std::string writeBuffer;
};

struct SyncContext {
    std::string buffer;
};

template<typename Context>
struct InitEvent {
    Context *cxt;
};

template<typename Context>
struct SessionEvent {
    Context *cxt;
};

using IOContext = std::variant<ClientContext, SyncContext>;
using Event = std::variant<InitEvent<ClientContext>, InitEvent<SyncContext>, SessionEvent<ClientContext>,
                           SessionEvent<SyncContext>>;

struct IOEvent {
    int fd;
    Event event;
    std::atomic<FdState> *fdState;
};

inline void print(std::string_view str, std::variant<io::GAIError, io::SystemError> const &error) {
    std::visit([&str](auto &error) { std::print("error: {} : {}\n", str, error.str()); }, error);
}

inline bool printPid(char const *pidFile) {
    if (auto const pdf = std::fopen(pidFile, "w")) {
        std::print(pdf, "{}\n", getpid());
        std::fclose(pdf);
        return true;
    }
    std::print("error: failed to write pid {} to pid file: {}\n", getpid(), pidFile);
    return false;
}

inline volatile std::sig_atomic_t sig;

inline bool setSignalHandlers() {
    auto signalHandler = +[](int s) { sig = s; };
    if (signal(SIGINT, signalHandler) != SIG_ERR and signal(SIGQUIT, signalHandler) != SIG_ERR and
        signal(SIGHUP, signalHandler) != SIG_ERR)
        return true;
    std::print("error: failed to set signal handlers\n");
    return false;
}

template<typename Dur>
class Timer {
public:
    using clock = chr::steady_clock;

    Timer() : tp{clock::now()} {}

    auto get() {
        auto const now = clock::now();
        return chr::duration_cast<Dur>(now - std::exchange(tp, now));
    }

private:
    clock::time_point tp;
};

template<typename... Callable>
struct Visitor : Callable... {
    using Callable::operator()...;
};

struct WriteSynRequest {
    std::string_view poster;
    std::string_view message;
};

struct ReplaceSynRequest {
    size_t number;
    std::string_view poster;
    std::string_view message;
};

using SyncRequest = std::variant<std::monostate, WriteSynRequest, ReplaceSynRequest>;

inline SyncRequest waitForCommit(int fd, std::string &buffer, chr::milliseconds timeout) {
    buffer.clear();
    Timer<chr::milliseconds> timer;
    pollfd pfd{.fd = fd, .events = POLL_IN, .revents{}};
    size_t ln = 0;
    while (true) {
        auto const n = poll(&pfd, 1, timeout.count());
        if (n <= 0) return {};
        if (not io::readFromSocket(fd, buffer)) return {};
        if (ln == 0 and buffer[0] != 'W' and buffer[0] != 'R') return {};
        if (auto const pos = buffer.find('\n', ln); pos != buffer.npos) {
            ln = pos;
            break;
        } else ln = buffer.size();
        timeout -= timer.get();
        if (timeout <= chr::milliseconds{}) return {};
    }
    if (std::string_view line{buffer.data(), ln}; line[0] == 'W') {
        line.remove_prefix(1);
        auto const slash = line.find('/');
        if (slash == line.npos or slash == 0) return {};
        auto const poster = line.substr(0, slash);
        auto const message = line.substr(slash + 1);
        if (not isValidName(poster) or not isValidMessage(message)) return {};
        return WriteSynRequest{.poster = poster, .message = message};
    } else if (line[0] == 'R') {
        line.remove_prefix(1);
        auto const number = getNumber(line);
        if (not number) return {};
        if (line.empty() or line[0] != ' ') return {};
        line.remove_prefix(1);
        auto const slash = line.find('/');
        if (slash == line.npos or slash == 0) return {};
        auto const poster = line.substr(0, slash);
        auto const message = line.substr(slash + 1);
        if (not isValidName(poster) or not isValidMessage(message)) return {};
        return ReplaceSynRequest{.number = *number, .poster = poster, .message = message};
    }
    return {};
}

inline bool waitForAck(std::span<pollfd> peers, chr::milliseconds timeout) {
    Timer<chr::milliseconds> timer;
    for (size_t p = peers.size();;) {
        auto const n = poll(peers.data(), peers.size(), timeout.count());
        if (n <= 0) return false;
        for (char ch; auto &peer : peers)
            if (peer.revents & POLL_IN) {
                if (auto const nr = io::read(peer.fd, &ch, 1); not nr or *nr != 1 or ch != 'S') return false;
                else {
                    peer.fd = ~peer.fd;
                    --p;
                }
            }
        if (p == 0) break;
        timeout -= timer.get();
        if (timeout <= chr::milliseconds{}) return false;
    }
    for (auto &peer : peers) peer.fd = ~peer.fd;
    return true;
}

bool syncWrite(std::span<Peer const> syncPeersAddrs, std::string_view commitMsg, auto operation,
               chr::milliseconds timeout) {
    std::vector<pollfd> syncPeers;
    auto const sendAck = [&](bool status) {
        for (auto &peer : syncPeers) io::writeToSocket(peer.fd, status ? "S"sv : "U"sv);
    };
    defer[&] {
        for (auto &peer : syncPeers) close(peer.fd);
    };
    for (auto &peer : syncPeersAddrs)
        if (auto const socket = io::tcpConnect(peer.host.data(), peer.port.data()))
            syncPeers.push_back({.fd = *socket, .events = POLL_IN, .revents{}});
        else return sendAck(false), false;
    if (not waitForAck(syncPeers, timeout)) return sendAck(false), false;
    for (auto &peer : syncPeers)
        if (not io::writeToSocket(peer.fd, commitMsg)) return sendAck(false), false;
    if (not waitForAck(syncPeers, timeout)) return sendAck(false), false;
    auto const res = operation();
    return sendAck(res), res;
}

class Server {
public:
    Server(char const *configFile) : m_ConfigFile{configFile} {}

    ~Server() {
        joinAllThreads();
        closeAllFds();
    }

    void start() {
        umask(0);
        if (not(loadConfigFile() and (not m_Config.daemon or daemon(logFile)) and setSignalHandlers() and
                printPid(pidFile) and loadBullentinBoard()))
            return;
        while (true) {
            launchIOThreads();
            if (not openServerSockets()) return;
            while (true) {
                if (auto const pollRes = io::poll(m_Sockets); pollRes and sig == 0) {
                    pollServerSockets();
                    pollClientSockets();
                } else if (sig != 0 or pollRes.error() == EINTR) {
                    if (sig == SIGINT or sig == SIGQUIT) return;
                    break;
                } else return std::print("error: poll failed: {}\n", pollRes.error().str());
            }
            reset();
            if (not loadConfigFile()) return;
            if (not loadBullentinBoard()) return;
        }
    }

private:
    bool processEvent(int fd, SessionEvent<ClientContext> cs) {
        ClientContext &ci = *cs.cxt;
        if (io::readFromSocket(fd, ci.readBuffer)) {
            std::string_view buffer = ci.readBuffer;
            while (auto const line = getLine(buffer)) {
                if (not line->empty()) {
                    ci.writeBuffer.clear();
                    bool keepAlive = true;
                    auto response = [out = std::back_inserter(ci.writeBuffer)]<typename... Args>(
                                            std::format_string<Args...> fmt, Args &&...args) {
                        std::format_to(out, fmt, std::forward<Args>(args)...);
                    };
                    processRequest(ci, *line, response, keepAlive);
                    if (not ci.writeBuffer.empty())
                        if (not io::writeToSocket(fd, ci.writeBuffer)) return false;
                    if (not keepAlive) return false;
                }
            }
            ci.readBuffer.erase(0, ci.readBuffer.size() - buffer.size());
            return true;
        }
        return false;
    }

    bool processEvent(int, SessionEvent<SyncContext>) { return false; }

    bool processEvent(int fd, InitEvent<ClientContext> ie) {
        ie.cxt->name = defaultUserName;
        auto out = std::back_inserter(ie.cxt->writeBuffer);
        std::format_to(out,
                       "Greetings,\n"
                       "{}\n"
                       "Your user name is \"{}\". To change it use the USER command.\n\n",
                       usageGuide, defaultUserName);
        return io::writeToSocket(fd, ie.cxt->writeBuffer);
    }

    bool processEvent(int fd, InitEvent<SyncContext> ie) {
        processSyncPeer(fd, *ie.cxt);
        return false;
    }

    void pollServerSockets() {
        if (m_Sockets[0].revents & POLL_IN)
            while (auto const client = io::accept(m_Sockets[0].fd)) addClient(*client);
        if (m_Sockets[1].revents & POLL_IN)
            while (auto const syncClient = io::accept(m_Sockets[1].fd)) addSyncPeer(*syncClient);
    }

    void pollClientSockets() {
        for (size_t i = 2; i != m_Sockets.size();) {
            auto const fd = m_Sockets[i].fd;
            auto const fdsPtr = m_FDStates.find(fd)->second.get();
            if (auto const fds = fdsPtr->load(std::memory_order::acquire);
                fds == FdState::IO and m_Sockets[i].revents & POLL_IN) {
                fdsPtr->store(FdState::PROCESS, std::memory_order::relaxed);
                std::visit(
                        [&](auto &cxt) {
                            m_EventQueue.push({.fd = fd, .event = SessionEvent{&cxt}, .fdState = fdsPtr});
                        },
                        *m_IOContexts.find(fd)->second.get());
            } else if (fds == FdState::CLOSE) {
                m_FDStates.erase(fd);
                m_IOContexts.erase(fd);
                m_Sockets[i] = m_Sockets.back();
                m_Sockets.pop_back();
                close(fd);
                continue;
            }
            ++i;
        }
    }

    void addClient(int fd) {
        m_Sockets.push_back({.fd = fd, .events = POLL_IN, .revents{}});
        auto [fds, _] = m_FDStates.emplace(fd, std::make_unique<FDStateFlag>(FdState::PROCESS));
        auto [ioc, _] = m_IOContexts.emplace(fd, std::make_unique<IOContext>(ClientContext{}));
        m_EventQueue.push({.fd = fd,
                           .event = InitEvent{std::get_if<ClientContext>(ioc->second.get())},
                           .fdState = fds->second.get()});
    }

    void addSyncPeer(int fd) {
        m_Sockets.push_back({.fd = fd, .events = POLL_IN, .revents{}});
        auto [fds, _] = m_FDStates.emplace(fd, std::make_unique<FDStateFlag>(FdState::PROCESS));
        auto [ioc, _] = m_IOContexts.emplace(fd, std::make_unique<IOContext>(SyncContext{}));
        m_EventQueue.push({.fd = fd,
                           .event = InitEvent{std::get_if<SyncContext>(ioc->second.get())},
                           .fdState = fds->second.get()});
    }

    bool loadConfigFile() {
        if (auto const data = io::fileData(m_ConfigFile))
            if (auto const config = parse<Config>(*data)) {
                m_Config = *config;
                return true;
            }
        std::print("error: couldn't load configuration\n");
        return false;
    }

    bool loadBullentinBoard() { return m_BullentinBoard.init(m_Config.bbFile.data()); }

    void launchIOThreads() {
        sigset_t mask;
        sigfillset(&mask);
        pthread_sigmask(SIG_SETMASK, &mask, nullptr);
        for (size_t i = 0; i != m_Config.thMax; ++i) m_IOThreads.emplace_back([&] { processIOEvents(); });
        sigemptyset(&mask);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGQUIT);
        sigaddset(&mask, SIGHUP);
        pthread_sigmask(SIG_UNBLOCK, &mask, nullptr);
    }

    void processIOEvents() {
        for (IOEvent ioEvent; m_EventQueue.pop(ioEvent);)
            std::visit(
                    [&](auto event) {
                        auto const status = processEvent(ioEvent.fd, event);
                        if (not status) shutdown(ioEvent.fd, SHUT_RDWR);
                        ioEvent.fdState->store(status ? FdState::IO : FdState::CLOSE, std::memory_order::release);
                    },
                    ioEvent.event);
    }

    bool openServerSockets() {
        auto const bbServer = io::tcpListener(m_Config.bbPort.data());
        auto const syncServer = io::tcpListener(m_Config.syncPort.data());
        if (not bbServer) {
            print("failed to open bullentin board server", bbServer.error());
            return false;
        }
        if (not syncServer) {
            print("failed to open sync server", syncServer.error());
            return false;
        }
        if (not(io::setNonBlock(*bbServer) and io::setNonBlock(*syncServer))) {
            std::print("failed to set listening sockets to non-blocking mode\n");
            return false;
        }
        m_Sockets.push_back({.fd = *bbServer, .events = POLL_IN, .revents{}});
        m_Sockets.push_back({.fd = *syncServer, .events = POLL_IN, .revents{}});
        return true;
    }

    void joinAllThreads() {
        m_EventQueue.quit();
        for (auto &t : m_IOThreads) t.join();
    }

    void closeAllFds() {
        for (auto pfd : m_Sockets)
            if (not io::close(pfd.fd)) std::print("error: couldn't close file: {}\n", pfd.fd);
    }

    void reset() {
        joinAllThreads();
        closeAllFds();
        m_SyncCxt = {};
        m_BullentinBoard.clear();
        m_IOThreads.clear();
        m_EventQueue.clear();
        m_Sockets.clear();
        m_FDStates.clear();
        m_IOContexts.clear();
        sig = 0;
    }

    void processRequest(ClientContext &ci, std::string_view line, auto response, bool &keepAlive) {
        auto const command = getWord(line);
        if (not command) return;
        if (*command == "QUIT") {
            keepAlive = false;
            return response("BYE {}.\n", ci.name);
        } else if (*command == "HELP") {
            return response("{}\n", usageGuide);
        } else if (*command == "USER") {
            auto const name = getWord(line);
            if (not name) return response("ERROR USER. Name is empty. Please, provide a name.\n");
            if (not isValidName(*name))
                return response("ERROR USER. Invalid name. Valid user name consists of alphanumeric characters.\n");
            ci.name = *name;
            return response("HELLO {}.\n", *name);
        } else if (*command == "READ") {
            auto const number = getNumber(line);
            if (not number) return response("UNKNOWN. Provided input is not a number.\n");
            std::shared_lock lk{m_BBMutex};
            if (*number >= m_BullentinBoard.size())
                return response("UNKNOWN {}. Message number is out of range.\n", *number);
            if (std::string poster, message; m_BullentinBoard.read(*number, poster, message))
                return response("MESSAGE {} {}/{}\n", *number, poster, message);
            return response("ERROR READ. Couldn't read message {}\n", *number);
        } else if (*command == "WRITE") {
            auto const msg = message(line);
            if (not isValidMessage(msg))
                return response("ERROR WRITE. Invalid message. Message should consist of printable characters.\n");
            if (m_Config.syncPeers.empty()) {
                std::scoped_lock lk{m_BBMutex};
                if (auto const msgNum = m_BullentinBoard.add(ci.name, msg)) return response("WROTE {}.\n", *msgNum);
                return response("ERROR WRITE. Couldn't write message.\n");
            } else {
                if (m_WriteFlag.test_and_set()) return response("ERROR WRITE. Server is busy.\n");
                defer[&] { m_WriteFlag.clear(); };
                m_SyncCxt.buffer.clear();
                std::format_to(std::back_inserter(m_SyncCxt.buffer), "W{}/{}\n", ci.name, msg);
                if (size_t msgNum; syncWrite(
                            m_Config.syncPeers, m_SyncCxt.buffer,
                            [&] {
                                std::scoped_lock lk{m_BBMutex};
                                auto const num = m_BullentinBoard.add(ci.name, msg);
                                if (num) msgNum = *num;
                                return num.has_value();
                            },
                            syncTimeOut))
                    return response("WROTE {}.\n", msgNum);
                return response("ERROR WRITE. Couldn't write message.\n");
            }
        } else if (*command == "REPLACE") {
            auto const number = getNumber(line);
            if (not number) return response("UNKNOWN. Provided input is not a number.\n");
            if (line.empty() or line[0] != '/') return response("ERROR REPLACE. Invalid format.\n");
            auto const msg = message(line.substr(1));
            if (not isValidMessage(msg))
                return response("ERROR REPLACE. "
                                "Invalid message. "
                                "Message should consist of printable characters.\n");
            if (m_Config.syncPeers.empty()) {
                std::scoped_lock lk{m_BBMutex};
                if (*number >= m_BullentinBoard.size())
                    return response("UNKNOWN {}. Message number is out of range.\n", *number);
                if (m_BullentinBoard.replace(*number, ci.name, msg)) return response("WROTE {}.\n", *number);
                return response("ERROR WRITE. Couldn't write message.\n");
            } else {
                if (m_WriteFlag.test_and_set()) return response("ERROR WRITE. Server is busy.\n");
                defer[&] { m_WriteFlag.clear(); };
                m_SyncCxt.buffer.clear();
                if (std::scoped_lock lk{m_BBMutex}; *number >= m_BullentinBoard.size())
                    return response("UNKNOWN {}. Message number is out of range.\n", *number);
                std::format_to(std::back_inserter(m_SyncCxt.buffer), "R{} {}/{}\n", *number, ci.name, msg);
                if (syncWrite(
                            m_Config.syncPeers, m_SyncCxt.buffer,
                            [&] {
                                std::scoped_lock lk{m_BBMutex};
                                return m_BullentinBoard.replace(*number, ci.name, msg);
                            },
                            syncTimeOut))
                    return response("WROTE {}.\n", *number);
                return response("ERROR WRITE. Couldn't write message.\n");
            }
        } else if (*command == "SIZE") {
            std::shared_lock lk{m_BBMutex};
            return response("{} MESSAGES.\n", m_BullentinBoard.size());
        } else if (*command == "LIST") {
            auto const first = getNumber(line);
            auto const last = getNumber(line);
            if (not(first and last and *last >= *first)) return response("ERROR LIST. Invalid input.\n");
            std::shared_lock lk{m_BBMutex};
            if (*last >= m_BullentinBoard.size()) return response("ERROR LIST. Numbers out of range.\n");
            std::string poster, message;
            for (size_t i = *first; i <= *last; ++i)
                if (m_BullentinBoard.read(i, poster, message)) response("MESSAGE {} {}/{}\n", i, poster, message);
                else return response("ERROR READ Couldn't read message {}.\n", i);
        } else return response("ERROR UNKNOWN COMMAND: {}\n", *command);
    }

    void processSyncPeer(int fd, SyncContext &si) {
        if (m_WriteFlag.test_and_set()) {
            io::writeToSocket(fd, "U"sv);
            return;
        }
        defer[&] { m_WriteFlag.clear(); };
        if (not io::writeToSocket(fd, "S"sv)) return;
        auto const commit = waitForCommit(fd, si.buffer, syncTimeOut);
        auto const res = std::visit(Visitor{[](std::monostate) { return false; },
                                            [&](WriteSynRequest wr) {
                                                std::scoped_lock lk{m_BBMutex};
                                                return m_BullentinBoard.add(wr.poster, wr.message).has_value();
                                            },
                                            [&](ReplaceSynRequest rr) {
                                                std::scoped_lock lk{m_BBMutex};
                                                return rr.number < m_BullentinBoard.size() and
                                                       m_BullentinBoard.replace(rr.number, rr.poster, rr.message);
                                            }},
                                    commit);
        if (not io::writeToSocket(fd, res ? "S"sv : "U"sv) or not res) return;
        if (pollfd pdf{.fd = fd, .events = POLL_IN, .revents{}};
            not waitForAck(std::span<pollfd>{&pdf, 1}, syncTimeOut)) {
            std::scoped_lock lk{m_BBMutex};
            m_BullentinBoard.rollback();
        }
    }


    static constexpr std::string_view defaultUserName = "nobody";
    static constexpr char const *pidFile = "bbserv.pid";
    static constexpr char const *logFile = "bbserv.log";
    static constexpr std::chrono::milliseconds syncTimeOut{500};
    using FDStateFlag = std::atomic<FdState>;
    char const *m_ConfigFile;
    Config m_Config;
    SyncContext m_SyncCxt;
    BulletinBoard m_BullentinBoard;
    std::shared_mutex m_BBMutex;
    std::vector<std::jthread> m_IOThreads;
    ConcurrentQueue<IOEvent> m_EventQueue;
    std::atomic_flag m_WriteFlag;
    std::vector<pollfd> m_Sockets;
    std::unordered_map<int, std::unique_ptr<FDStateFlag>> m_FDStates;
    std::unordered_map<int, std::unique_ptr<IOContext>> m_IOContexts;
};
