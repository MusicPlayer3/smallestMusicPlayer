#ifndef SIMPLETHREADPOOL_HPP
#define SIMPLETHREADPOOL_HPP

#include "PCH.h"
#include <functional> // 必须包含，用于 std::bind

// 引入 BS::thread_pool 库
// 请确保 BS_thread_pool.hpp 文件在您的包含路径下
#include "BS_thread_pool.hpp"

class SimpleThreadPool
{
public:
    // 单例访问点
    static SimpleThreadPool &instance()
    {
        // 传入 hardware_concurrency，具体数值会在构造函数中被最小限制逻辑调整
        static SimpleThreadPool pool(std::thread::hardware_concurrency());
        return pool;
    }

    // 构造函数
    SimpleThreadPool(size_t threads) :
        // 限制最小线程数为 2，与您原有的逻辑保持一致
        // 使用 BS::thread_pool<> 显式实例化模板
        pool_(threads < 2 ? 2 : threads)
    {
    }

    // 禁止拷贝和移动
    SimpleThreadPool(const SimpleThreadPool &) = delete;
    SimpleThreadPool &operator=(const SimpleThreadPool &) = delete;

    /**
     * @brief 提交任务到线程池
     *
     * BS::thread_pool v5.0.0+ 的 submit_task 要求传入无参的可调用对象。
     * 使用 std::bind 将用户的函数和参数绑定为一个无参对象。
     *
     * @return std::future<返回值类型>
     */
    template <class F, class... Args>
    auto enqueue(F &&f, Args &&...args)
    {
        // submit_task 返回 std::future<ResultType>，这与您原本的接口兼容
        return pool_.submit_task(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
    }

    // 手动关闭函数
    void shutdown()
    {
        // 等待所有任务完成
        pool_.wait();
    }

    // 析构函数
    ~SimpleThreadPool()
    {
        // 析构时会自动等待所有任务完成
        shutdown();
    }

    // [修复] 返回类型必须包含模板参数 <>
    // 获取底层 BS::thread_pool 对象引用
    BS::thread_pool<> &get_native_pool()
    {
        return pool_;
    }

private:
    // [修复] 成员变量类型必须包含模板参数 <>，使用默认参数
    BS::thread_pool<> pool_;
};

#endif