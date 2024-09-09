#ifndef BULLETIN_BOARD
#define BULLETIN_BOARD

#include "io.hpp"
#include "parse.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <optional>
#include <print>
#include <string_view>
#include <utility>
#include <vector>

class MsgIndex {
public:
    MsgIndex(size_t off, size_t ns, size_t ps, size_t ms) : offset{off}, nSize{ns}, pnSize{ps}, msgSize{ms} {}
    void shift(ssize_t n) { offset += n; }
    size_t lineOffset() const { return offset; }
    size_t lineSize() const { return nSize + pnSize + msgSize + 3; }
    size_t numberOffset() const { return 0; }
    size_t numberSize() const { return nSize; }
    size_t posterNameOffset() const { return nSize + 1; }
    size_t posterNameSize() const { return pnSize; }
    size_t messageOffset() const { return nSize + pnSize + 2; }
    size_t messageSize() const { return msgSize; }

private:
    size_t offset;
    size_t nSize;
    size_t pnSize;
    size_t msgSize;
};

inline bool isPrint(char ch) { return std::isprint(static_cast<unsigned char>(ch)); }

inline bool isAlphaNum(char ch) { return std::isalnum(static_cast<unsigned char>(ch)); }

inline bool isValidMessage(std::string_view str) { return std::ranges::all_of(str, isPrint); }

inline bool isValidName(std::string_view str) { return std::ranges::all_of(str, isAlphaNum); }

inline std::optional<size_t> makeIndex(int fd, std::vector<MsgIndex> &index, size_t blockSize) {
    constexpr size_t npos = std::string_view::npos;
    index.clear();
    std::string buffer;
    size_t offset = 0;
    while (true) {
        auto const nread = io::readAppend(buffer, fd, blockSize);
        if (not nread or *nread == 0) break;
        size_t ncousumed = 0;
        for (std::string_view str = buffer; not str.empty();) {
            auto const ln = str.find_first_of('\n');
            if (ln == npos) break;
            auto const sl1 = str.find_first_of('/');
            if (sl1 == npos or sl1 > ln) return {};
            auto const sl2 = str.find_first_of('/', sl1 + 1);
            if (sl2 == npos or sl2 > ln) return {};
            std::string_view const nStr = str.substr(0, sl1), pStr = str.substr(sl1 + 1, sl2 - sl1 - 1),
                                   mStr = str.substr(sl2 + 1, ln - sl2 - 1);
            if (auto const num = parse<size_t>(nStr); not num or *num != index.size()) return {};
            if (pStr.empty() or not isValidName(pStr)) return {};
            if (not isValidMessage(mStr)) return {};
            index.push_back(MsgIndex{offset, nStr.size(), pStr.size(), mStr.size()});
            offset += ln + 1;
            ncousumed += ln + 1;
            str = str.substr(ln + 1);
        }
        buffer.erase(0, ncousumed);
    }
    if (buffer.empty()) return offset;
    return {};
}

inline void toString(size_t number, std::string &str) {
    str.resize_and_overwrite(128, [&](char *ptr, size_t n) {
        auto [p, _] = std::to_chars(ptr, ptr + n, number);
        return p - ptr;
    });
}

template<size_t blockSize>
bool shiftForward(int fd, size_t pos, size_t n, size_t fs) {
    std::array<char, blockSize> buffer;
    for (size_t rp = pos; rp != fs;) {
        auto const nr = io::readNAt(fd, buffer.data(), buffer.size(), rp);
        if (not nr) return false;
        if (not io::writeNAt(fd, buffer.data(), *nr, rp - n)) return false;
        rp += *nr;
    }
    return io::ftruncate(fd, fs - n).has_value();
}

template<size_t blockSize>
bool shiftBackward(int fd, size_t pos, size_t n, size_t fs) {
    std::array<char, blockSize> buffer;
    for (size_t re = fs; re != pos;) {
        auto const rs = std::min(blockSize, re - pos);
        auto const rp = re - rs;
        auto const nr = io::readNAt(fd, buffer.data(), rs, rp);
        if (not nr or *nr != rs) return false;
        if (not io::writeNAt(fd, buffer.data(), *nr, rp + n)) return false;
        re = rp;
    }
    return true;
}

inline size_t messageLine(std::string &buffer, size_t i, std::string_view poster, std::string_view message) {
    toString(i, buffer);
    auto const numSize = buffer.size();
    buffer += '/';
    buffer += poster;
    buffer += '/';
    buffer += message;
    buffer += '\n';
    return numSize;
}

class BulletinBoard {
public:
    BulletinBoard() = default;

    ~BulletinBoard() { close(m_Fd); }

    size_t size() const { return m_Index.size(); }

    bool read(size_t i, std::string &poster, std::string &message) const {
        auto const msgi = m_Index[i];
        std::string buffer;
        auto const nread = io::readTo(buffer, m_Fd, msgi.lineSize(), msgi.lineOffset());
        if (not nread or *nread != msgi.lineSize()) return false;
        std::string_view const msgline = buffer;
        poster = msgline.substr(msgi.posterNameOffset(), msgi.posterNameSize());
        message = msgline.substr(msgi.messageOffset(), msgi.messageSize());
        return true;
    }

    std::optional<size_t> add(std::string_view poster, std::string_view message) {
        auto const msgNum = m_Index.size();
        auto const numSize = messageLine(m_WriteBuffer, msgNum, poster, message);
        if (not io::writeNAt(m_Fd, m_WriteBuffer.data(), m_WriteBuffer.size(), m_AppendOffset)) return {};
        m_RevertData = RevertAppendData{};
        m_Index.push_back(MsgIndex{m_AppendOffset, numSize, poster.size(), message.size()});
        m_AppendOffset += m_WriteBuffer.size();
        return msgNum;
    }

    bool replace(size_t i, std::string_view poster, std::string_view message) {
        if (RevertReplaceData rdata{i, {}, {}}; not read(i, rdata.poster, rdata.message)) return false;
        else m_RevertData = std::move(rdata);
        return replaceImpl(i, poster, message);
    }

    void rollback() {
        std::visit([&](auto &rbd) { revert(rbd); }, m_RevertData);
        m_RevertData = {};
    }

    bool init(char const *file) {
        auto const fd = io::open(file, O_RDWR | O_CREAT, 0644);
        if (not fd) return std::print("error: couldn't open bulletin board file: {}\n", file), false;
        auto const offset = makeIndex(*fd, m_Index, blockSize);
        if (not offset) return std::print("error: bulletin board file {} have invalid data\n", file), false;
        m_Fd = *fd;
        m_AppendOffset = *offset;
        return true;
    }

    void clear() {
        m_Index.clear();
        close(std::exchange(m_Fd, -1));
        m_AppendOffset = 0;
    }

private:
    struct RevertAppendData {};

    struct RevertReplaceData {
        size_t i;
        std::string poster;
        std::string message;
    };

    void revert(RevertAppendData) {
        auto const msgi = m_Index.back();
        m_Index.pop_back();
        if (not io::ftruncate(m_Fd, msgi.lineOffset())) std::print("error: couldn't truncate file to previous size");
        m_AppendOffset = msgi.lineOffset();
    }

    void revert(RevertReplaceData const &rad) {
        if (not replaceImpl(rad.i, rad.poster, rad.message)) std::print("error: couldn't revert replace operation");
        m_RevertData = {};
    }

    void revert(std::monostate) {}

    bool replaceImpl(size_t i, std::string_view poster, std::string_view message) {
        auto const numSize = messageLine(m_WriteBuffer, i, poster, message);
        auto const pmsgi = m_Index[i];
        ssize_t const diff = m_WriteBuffer.size() - pmsgi.lineSize();
        if (auto const pos = pmsgi.lineOffset() + pmsgi.lineSize(); diff > 0) {
            if (not shiftBackward<blockSize>(m_Fd, pos, diff, m_AppendOffset)) return false;
        } else if (diff < 0)
            if (not shiftForward<blockSize>(m_Fd, pos, -diff, m_AppendOffset)) return false;
        if (not io::writeNAt(m_Fd, m_WriteBuffer.data(), m_WriteBuffer.size(), pmsgi.lineOffset())) return false;
        m_Index[i] = MsgIndex{pmsgi.lineOffset(), numSize, poster.size(), message.size()};
        if (diff != 0)
            for (size_t j = i + 1; j != m_Index.size(); ++j) m_Index[j].shift(diff);
        m_AppendOffset += diff;
        return true;
    }

    static constexpr size_t blockSize = 4 * 1024;
    std::vector<MsgIndex> m_Index;
    int m_Fd = -1;
    size_t m_AppendOffset = 0;
    std::string m_WriteBuffer;
    std::variant<std::monostate, RevertAppendData, RevertReplaceData> m_RevertData;
};

#endif
