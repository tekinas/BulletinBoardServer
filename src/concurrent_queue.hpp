#ifndef CONCURRENT_QUEUE
#define CONCURRENT_QUEUE

#include <condition_variable>
#include <deque>
#include <mutex>

template<typename T>
class ConcurrentQueue {
public:
    ConcurrentQueue() = default;

    ConcurrentQueue(ConcurrentQueue const &) = delete;

    ConcurrentQueue &operator=(ConcurrentQueue const &) = delete;

    void push(T value) {
        std::lock_guard lk{m_Mutex};
        m_Queue.push_back(std::move(value));
        m_CondVar.notify_one();
    }

    void quit() {
        std::lock_guard lk{m_Mutex};
        m_Quit = true;
        m_CondVar.notify_all();
    }

    bool pop(T &value) {
        std::unique_lock lk{m_Mutex};
        m_CondVar.wait(lk, [&] { return (not m_Queue.empty()) or m_Quit; });
        if (m_Quit) return false;
        value = std::move(m_Queue.front());
        m_Queue.pop_front();
        return true;
    }

    void clear() {
        std::lock_guard lk{m_Mutex};
        m_Quit = false;
        m_Queue.clear();
    }

    bool empty() const {
        std::lock_guard lk{m_Mutex};
        return m_Queue.empty();
    }

private:
    std::deque<T> m_Queue;
    mutable std::mutex m_Mutex;
    std::condition_variable m_CondVar;
    bool m_Quit = false;
};

#endif
