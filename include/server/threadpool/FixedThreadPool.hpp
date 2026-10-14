// 用来把任务投递到线程池中，线程池中的线程会从任务队列中取出任务并执行
#include "syncQueue_2.hpp"
#include <functional>
#include <thread>
#include <list>
#include <atomic>
#include <future>
#include <memory>
#include <iostream>
#include "TaskExecutor.hpp"
using namespace std;

#ifndef FIXED_THREAD_POOL
#define FIXED_THREAD_POOL

namespace pool
{
    /*
    把各种各样要执行的逻辑，统一包装成同一种类型
    可以把任务存进容器（vector<TaskType>、队列）
    线程从队列里取出来，直接 task() 执行，不用关心任务原本是什么
    */
    using TaskType = std::function<void(void)>; // 包装器，把要执行的任务，包装成无参无返回值的可调用对象
    class FixedThreadPool : public TaskExecutor
    {
    private:
        pool::SyncQueue<TaskType> m_queue; // 任务队列
        // std::list<std::thread>m_threadgroup; // 线程组
        std::list<std::shared_ptr<std::thread>> m_threadgroup; // 线程组
        std::atomic_bool m_running;                            // 是否开始
        std::once_flag m_flag;                                 // 保证线程池只能被启动一次
        void Start(int numThreads)                             // 启动线程池,参数为线程池中线程的数量
        {
            m_running = true;                    // 标记线程池开始运行
            for (int i = 0; i < numThreads; ++i) // 循环创建线程，线程函数为ScheduleThreadPool::RunThread
            {
                // 就是创建一个管理std::thread对象的shared_ptr共享智能指针，指向新启动的线程，绑定执行成员函数RunThread，再把这个智能指针存入线程组容器统一管理
                m_threadgroup.push_back(std::make_shared<std::thread>(&FixedThreadPool::RunThread, this));

                // std::thread tha(&ScheduleThreadPool::RunThread, this);//直接创建线程对象
                // std::shared_ptr<std::thread> ptha = std::make_shared<std::thread>(new thread(&ScheduleThreadPool::RunThread, this));//创建线程对象由智能指针来管理,将线程对象和this分开创建
                // std::shared_ptr<std::thread> pthb = std::make_shared<std::thread>(&ScheduleThreadPool::RunThread, this);//这个是直接创建线程对象的简化版本，直接在make_shared里创建线程对象
                // m_threadgroup.push_back(pthb);//把线程对象放进线程组
            }
        }
        void RunThread() // 线程函数
        {
            while (m_running) // 线程池正在运行
            {
                TaskType task;       // 定义一个任务对象
                m_queue.Task(&task); // 从任务队列中取出一个任务
                if (task)            // 如果取出的任务不为空，就执行这个任务
                {
                    task(); // 执行任务
                    /*
                    std::function 是 C++ 标准库写好的类,它内部已经重载了 operator(),所以它的对象可以直接 () 调用
                    TaskType 就是 std::function，所以 task() 能执行,执行的就是你之前包装进去的函数 /lambda
                    */
                }
            }
        }
        void StopThreadGroup() // 停止线程池
        {
            //m_queue.Stop();    // 停止任务队列，唤醒所有等待的线程，让它们知道线程池要停止了
            m_queue.WaitQueueEmptyStop(); // 等待队列清空，停止任务队列，唤醒所有等待的线程，让它们知道线程池要停止了
            m_running = false; // 标记线程池停止运行

            for (auto &tha : m_threadgroup) // 遍历线程组中的每个线程
            {
                if (tha->joinable()) // 如果线程可连接（即还没有被 join 或 detach）
                {
                    tha->join(); // 等待线程执行完毕
                }
            }
            m_threadgroup.clear(); // 清空线程组容器，释放资源
        }

    public:
        FixedThreadPool(const size_t taskquesize = 2000, int numthreads = std::thread::hardware_concurrency()) // 获取系统逻辑内核个数
            : m_queue(taskquesize), m_running(false)
        {
            Start(numthreads);
        }
        ~FixedThreadPool()
        {
            if (m_running)
            {
                Stop();
            }
        }

        // 线程池的启动/停止，只能执行一次！
        void Stop() // 多线程环境下，Stop函数可能被调用多次，所以定义一个标志，第一次标志是未执行，执行一次过后标志设为已执行，然后后面每一次看到标志直接保证线程池只能被停止一次
        {
            std::call_once(m_flag, [this]() -> void
                           { StopThreadGroup(); });
        }
        void AddTask(const TaskType &task)
        {
            // 队列满时在提交边界做背压，不能静默丢弃业务消息，也不能回退到 IO 线程执行。
            while (m_running && m_queue.Put(task) != 0)
            {
                std::this_thread::yield();
            }
        }
        void AddTask(TaskType &&task)
        {
            while (m_running && m_queue.Put(task) != 0)
            {
                std::this_thread::yield();
            }
        }

        size_t queueSize() const { return m_queue.Size(); }
        size_t queueCapacity() const { return m_queue.MaxSize(); }

        bool submitTask(TaskType task) override
        {
            while (m_running && m_queue.Put(std::move(task)) != 0)
                std::this_thread::yield();
            return m_running;
        }

        template <class Func, class... Args>
        auto submit(Func &&func, Args &&...args)
        {
            // 任务提交接口，传入一个可调用对象和它的参数，返回一个future对象，调用者可以通过这个future对象获取任务的返回值
            using RetType = decltype(func(args...)); // 定义一个变量来存储任务的返回值
            auto task = std::make_shared<std::packaged_task<RetType()>>(
                std::bind(std::forward<Func>(func), std::forward<Args>(args)...));
            std::future<RetType> result = task->get_future(); // 获取这个任务对象的future对象，调用者可以通过这个future对象获取任务的返回值
            TaskType queueTask = [task]() mutable
                            { (*task)(); };
            submitTask(std::move(queueTask));
            return result;
        }
    };

}
#endif
