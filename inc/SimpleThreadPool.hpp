#ifndef SIMPLETHREADPOOL_HPP
#define SIMPLETHREADPOOL_HPP

#include "PCH.h"

class SimpleThreadPool
{
public:
    static SimpleThreadPool &instance()
    {
        static SimpleThreadPool pool(std::thread::hardware_concurrency());
        return pool;
    }

    SimpleThreadPool(size_t threads) :
        stop(false)
    {
        // 限制最小线程数，防止单核机器出问题
        if (threads < 2)
            threads = 2;
        for (size_t i = 0; i < threads; ++i)
            workers.emplace_back(
                [this]
                {
                    for (;;)
                    {
                        std::function<void()> task;
                        {
                            std::unique_lock<std::mutex> lock(this->queue_mutex);
                            this->condition.wait(lock, [this]
                                                 { return this->stop || !this->tasks.empty(); });
                            if (this->stop && this->tasks.empty())
                                return;
                            task = std::move(this->tasks.front());
                            this->tasks.pop();
                        }
                        task();
                    }
                });
    }

    template <class F, class... Args>
    auto enqueue(F &&f, Args &&...args)
        -> std::future<typename std::invoke_result<F, Args...>::type>
    {
        using return_type = typename std::invoke_result<F, Args...>::type;

        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));

        std::future<return_type> res = task->get_future();
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            if (stop)
                throw std::runtime_error("enqueue on stopped ThreadPool");
            tasks.emplace([task]()
                          { (*task)(); });
        }
        condition.notify_one();
        return res;
    }

    // [修改] 增加手动关闭函数
    void shutdown()
    {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            if (stop)
                return; // 避免重复关闭
            stop = true;
        }
        condition.notify_all();
        for (std::thread &worker : workers)
        {
            if (worker.joinable())
                worker.join();
        }
        workers.clear();
    }

    // [修改] 析构函数调用 shutdown
    ~SimpleThreadPool()
    {
        shutdown();
    }

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
};

#endif