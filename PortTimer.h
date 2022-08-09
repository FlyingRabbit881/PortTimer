#ifndef PORT_TIMER_H
#define PORT_TIMER_H

/** Examples
(1) one shot timer
fly::TimerManager::Instance().Init(1);
fly::Timer t1;
t1.ExpiresAfter(10000, [](){
    printf("timer1\n");
});

(2) repeated timer
void Print(fly::Timer* timer)
{
    printf("timer2\n");
    timer->ExpiresAt(timer->Expiry() + std::chrono::seconds(3), std::bind(&Print, timer));
}
fly::TimerManager::Instance().Init(3);
fly::Timer t2;
t2.ExpiresAfter(3000, std::bind(&Print, &t2));

*/



#include <cstring>
#include <chrono>
#include <thread>
#include <functional>
#include <memory>
#include <vector>
#include <atomic>
#include <mutex>
#include <map>
#include <algorithm>
#include <iterator>

namespace fly
{


    template <class T>
    inline void boostHashCombine(std::size_t& seed, const T& v)
    {
        seed ^= std::hash<T>{}(v)+0x9e3779b9 + (seed << 6) + (seed >> 2);
    }

    using handler_t = std::function<void()>;
    using clock = std::chrono::steady_clock;
    using timestamp = std::chrono::time_point<clock>;

    class Timer
    {
    public:
        friend class TimerManager;
        static uint64_t GetId()
        {
            static std::atomic<uint64_t> sequence{ 0 };
            return sequence.fetch_add(1);
        }
        struct TimerInfo
        {
            timestamp expiry;
            uint64_t id;
            bool valid{ false };
            handler_t handler{ nullptr };
            std::recursive_mutex timer_mutex;
        };
        Timer() : m_info(std::make_shared<TimerInfo>())
        {
            m_info->id = GetId();
        }
        ~Timer()
        {
            Cancel();
        }
        Timer(const Timer&) = delete;
        Timer& operator=(const Timer&) = delete;

        void Cancel();
        const timestamp& Expiry() const { return m_info->expiry; }
        template <class Rep, class Period>
        inline bool ExpiresAfter(const std::chrono::duration<Rep, Period>& delay, handler_t handler)
        {
            return ExpiresAt(clock::now() + std::chrono::duration_cast<std::chrono::microseconds>(delay), std::move(handler));
        }
        bool ExpiresAfter(uint32_t millisec, handler_t handler)
        {
            return ExpiresAfter(std::chrono::milliseconds(millisec), std::move(handler));
        }
        bool ExpiresAt(const timestamp& when, handler_t handler);

        std::shared_ptr<TimerInfo> m_info;
        int m_worker{ -1 };
    };

    class TimerManager
    {
    public:
        class Worker
        {
        public:
            using Ptr = std::unique_ptr<Worker>;
            using TimerKey = std::pair<timestamp, uint64_t>;
            using TimerMap = std::map<TimerKey, std::weak_ptr<Timer::TimerInfo>>;
            Worker()
            {
                m_thread = std::thread([this]() { Run(); });
            }
            ~Worker()
            {
                m_quit = true;
                m_condition.notify_one();
                if (m_thread.joinable())
                {
                    m_thread.join();
                }
            }
            bool Add(const std::shared_ptr<Timer::TimerInfo>& info)
            {
                TimerKey key{ info->expiry, info->id };
                {
                    std::lock_guard<std::mutex> lk(m_mutex);
                    bool earliestChanged = false;
                    auto it = m_timers.begin();
                    if (it == m_timers.end() || info->expiry < it->first.first)
                    {
                        earliestChanged = true;
                    }

                    auto result = m_timers.emplace(key, std::weak_ptr<Timer::TimerInfo>(info));
                    if (!result.second)
                    {
                        return false;
                    }
                    if (earliestChanged)
                    {
                        m_condition.notify_one();
                    }
                }
                return true;
            }
            void Remove(const std::shared_ptr<Timer::TimerInfo>& info)
            {
                TimerKey key{ info->expiry, info->id };
                {
                    std::lock_guard<std::mutex> lk(m_mutex);
                    m_timers.erase(key);
                }
            }
            void Run()
            {
                while (!m_quit)
                {
                    // wait for wakeup
                    std::unique_lock<std::mutex> lk(m_mutex);
                    while (!m_quit)
                    {
                        if (m_timers.empty())
                        {
                            m_condition.wait(lk);
                        }
                        else
                        {
                            auto when = m_timers.begin()->first.first;
                            if (std::chrono::steady_clock::now() < when)
                            {
                                m_condition.wait_until(lk, when);
                            }
                            else
                            {
                                break;
                            }
                        }
                    }

                    if (m_quit)
                    {
                        break;
                    }
                    
                    // handle expired timers
                    auto expired = GetExpired(clock::now());
                    lk.unlock();

                    for (const auto& weakInfo : expired)
                    {
                        auto info = weakInfo.second.lock();
                        if (!info)
                            continue;
                        auto expiry = weakInfo.first.first;
                        std::lock_guard<decltype(info->timer_mutex)> lk(info->timer_mutex);
                        if (!info->valid || info->expiry != expiry)   // timer is canceled or timer is reset for new expiry
                            continue;
                        handler_t handler = std::move(info->handler);
                        info->handler = nullptr;
                        if (handler)
                        {
                            handler();
                        }
                        if (info->handler == nullptr)  // meaning timer is not reset in the callback
                        {
                            info->valid = false;
                        }
                    }
                  
                    
                }
            }
        private:
            std::vector<TimerMap::value_type> GetExpired(const timestamp& now)
            {
                std::vector<TimerMap::value_type> expired;
                TimerKey key{ now, UINT64_MAX };
                auto end = m_timers.upper_bound(key);
                std::move(m_timers.begin(), end, std::back_inserter(expired));
                m_timers.erase(m_timers.begin(), end);
                return expired;
            }
           

      
            std::thread m_thread;
            std::atomic<bool> m_quit{ false };
            std::mutex m_mutex;
            std::condition_variable m_condition;
            TimerMap m_timers;
        };
        static TimerManager& Instance()
        {
            static TimerManager mgr;
            return mgr;
        }

        bool Init(int threadCount)
        {
            m_workers.reserve(threadCount);
            for (int i = 0; i < threadCount; ++i)
            {
                m_workers.push_back(std::unique_ptr<Worker>(new Worker()));
            }
            return true;
        }
        void UnInit()
        {
            m_workers.clear();
        }
        bool AddTimer(Timer* timer)
        {
            if (m_workers.empty())
            {
                return false;
            }
            if (timer->m_worker < 0)
            {
                size_t seed{ 0 };   // use hash to get a random number
                boostHashCombine(seed, timer->m_info->id);
                boostHashCombine(seed, timer);
                timer->m_worker = static_cast<int>(seed % m_workers.size());
            }

            return m_workers[timer->m_worker]->Add(timer->m_info);
        }
        void RemoveTimer(Timer* timer)
        {
            if (timer->m_worker < 0)
                return;
            m_workers[timer->m_worker]->Remove(timer->m_info);
        }

    private:
        TimerManager() = default;
        TimerManager(const TimerManager&) = delete;
        TimerManager& operator=(const TimerManager&) = delete;

        std::vector<Worker::Ptr> m_workers;

    };

    inline bool Timer::ExpiresAt(const timestamp& when, handler_t handler)
    {
        std::lock_guard<decltype(m_info->timer_mutex)> lk(m_info->timer_mutex);
        // cancel first
        if (m_info->valid)
        {
            if (m_info->expiry == when)     // same expiry has been scheduled, just replace the handler
            {
                m_info->handler = std::move(handler);
                return true;
            }
            m_info->valid = false;
            TimerManager::Instance().RemoveTimer(this);
        }
        // add timer
        m_info->expiry = when;
        m_info->handler = std::move(handler);
        m_info->valid = true;
        return TimerManager::Instance().AddTimer(this);
    }

    inline void Timer::Cancel()
    {
        std::lock_guard<decltype(m_info->timer_mutex)> lk(m_info->timer_mutex);
        if (!m_info->valid)
            return;
        m_info->valid = false;
        TimerManager::Instance().RemoveTimer(this);
    }

}  // namespace fly




#endif   // PORT_TIMER_H

