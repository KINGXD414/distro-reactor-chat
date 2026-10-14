#include <deque>
#include <mutex>
#include <assert.h>
#include <utility>
#include <condition_variable>
using namespace std;
#ifndef SYNC_QUEUE_2_HPP
#define SYNC_QUEUE_2_HPP

namespace pool
{
    const size_t MaxTaskCount = 500; // 最大任务数
    template <class T>               // 模板类T，在初始化同步队列时就已经给定
    class SyncQueue                  // 以下都为模板T的模板方法
    {
    private:
        std::deque<T> m_queue;              // 双端队列存放任务信息
        mutable std::mutex m_mutex;         // 去常性互斥锁，mutable修饰的成员变量可以在const成员函数中被修改
        std::condition_variable m_notEmpty; // 条件：不空就可以取任务，消费者
        std::condition_variable m_notFull;  // 条件：不满就可以放任务，生产者
        std::condition_variable m_waitStop;
        int m_maxsize; // 上限
        int m_waitTime;
        bool m_needStop; // 为真，同步队列停止工作
        int m_timeout;   // 超时时间，单位毫秒

        // 内部私有接口
        bool IsFull() const // 判满
        {
            bool full = m_queue.size() >= m_maxsize; // 当此时任务数大于最大值
            return full;
        }
        bool IsEmpty() const // 判空
        {
            bool empty = m_queue.empty();
            return empty;
        }
        template <class F> // 模板F，F与T要进行兼容性检查
        int Add(F &&task)  // 添加任务，这个&&代表的是引用型别未定义
        {
            // 加锁
            std::unique_lock<std::mutex> locker(m_mutex);
             // 添加任务时：等待的条件——不停止或者不满的时候等待下一次往里加
             // lambda表达式为真不等待，为假处于等待，要用对象的成员方法时需要捕获this指针
            #if 0
            m_notFull.wait(locker, [this]() -> bool
                         {
                          return m_needStop || !IsFull(); // 不停止或者不满的时候等待
                       });                                 // 弃锁，阻塞，等待被唤醒，重新加锁，继续往下走
            // return -1;//等待了时间超时了，自己唤醒返回-1;
            #endif
            // 线程池没有停止并且队列已经满了，就等待一段时间，如果等待了时间超时了，自己唤醒返回-1;如果线程池停止了，就不添加了，直接返回-2;如果添加成功，返回0
            // 写法1：使用wait_for和cv_status结合，等待的条件是线程池没有停止并且队列已经满了
            // wait_for(锁，时间)，两参时，返回值只有两种可能：
            // std::cv_status::timeout → 超时了
            // std::cv_status::no_timeout → 被唤醒了
            #if 0
            while (!m_needStop && IsFull())
            {
                std::cv_status tag = m_notFull.wait_for(locker, std::chrono::milliseconds(m_timeout));

                if (tag == std::cv_status::timeout) // 如果等待了时间超时了，自己唤醒返回-1;
                {
                    return -1;
                }
            }
            #endif

            // 写法2：使用wait_for和lambda表达式结合，lambda为假，才等待，等待的条件是线程池没有停止并且队列已经满了
            // wait_for(锁，时间，条件（假的，等够时间退出/真的，直接退出）)，三参时，返回值只有两种可能：
            // true → 条件为真，直接退出
            // false → 条件为假，等待了时间超时了，自己唤醒返回-1;
            if (m_notFull.wait_for(locker, std::chrono::milliseconds(m_timeout), [this]() -> bool
                                   {
                                       return m_needStop || !IsFull(); // 不停止或者不满的时候等待
                                   }) == false)                        // 如果wait_for返回false，等待了时间超时了，没有人来唤醒，自己唤醒返回-1;
            {
                return -1;
            }
            if (m_needStop)
                return -2; // 如果停止了，就不添加了，直接返回-2
            m_queue.push_back(std::forward<F>(task));
            m_notEmpty.notify_all();
            return 0; // 添加成功，返回0
        }

    public:
        SyncQueue(int maxsize = MaxTaskCount, int timeout = 1)
            : m_maxsize(maxsize),
              m_timeout(timeout),
              m_waitTime(timeout),
              m_needStop(false)//runing
        {
        }
        ~SyncQueue() // 析构
        {
            if (!m_needStop) // 如果没有停止，就停止
            {
                Stop();
            }
        }

        int Put(const T &task) // 万能引用
        {
            return Add(task); // 传入一个const T类型的任务对象，调用Add方法添加任务
        }
        int Put(T &&task) // 万能引用
        {
            return Add(std::forward<T>(task)); // 传入一个const T类型的任务对象，调用Add方法添加任务
        }

        void Task(std::deque<T> *pdeq) // 写数据、投任务、批量塞入
        {
            assert(pdeq != nullptr);
            std::unique_lock<std::mutex> locker(m_mutex);
            // 不为空
            m_notEmpty.wait(locker, [this]() -> bool
                            {
                                return m_needStop || !IsEmpty(); // 当停止了或者不空的时候就不等待了，继续往下走
                            });                                  // 弃锁，阻塞，等待被唤醒，重新加锁，继续往下走
            if (m_needStop)
                return;                 // 如果停止了，就不取了，直接返回
            *pdeq = std::move(m_queue); // 将任务一次取
            m_notFull.notify_all();
            m_waitStop.notify_all();
        }
        // void Task(const std::deque<T>&que);//读数据、取任务、遍历消费
        void Task(T *ps) // 传入一个指向单个T对象的指针
        {
            assert(ps != nullptr); // Debug版本有用
            // 加锁
            std::unique_lock<std::mutex> locker(m_mutex);
            // 不为空
            m_notEmpty.wait(locker, [this]() -> bool
                            {
                //不停，不为空，还有东西可以取，可以消费，阻塞
                return m_needStop||!IsEmpty(); });
            if (m_needStop)
                return;
            *ps = m_queue.front(); // 一次取一个
            m_queue.pop_front();   // 取完了就弹出
            m_notFull.notify_all();
            m_waitStop.notify_all();
        }

        // 多个线程访问同一个变量，一定要加锁！
        void Stop() // 停止工作
        {
            // 加锁不是为了只停一次，是为了安全修改多线程共享的停止标记
            {
                std::unique_lock<std::mutex> locker(m_mutex); // 加锁
                m_needStop = true;                            // 标记停止
            }
            m_notEmpty.notify_all();
            m_notFull.notify_all();
        }
        void WaitQueueEmptyStop() // 等待队列清空
        {
            std::unique_lock<std::mutex> locker(m_mutex);
            while (!IsEmpty())
            {
                m_waitStop.wait_for(locker, std::chrono::milliseconds(m_waitTime));// 等待的条件是队列不空，等待了时间超时了，自己唤醒继续判断，如果队列空了，就退出循环
            }
            m_needStop = true;
            m_notEmpty.notify_all();
            m_notFull.notify_all();
        }
        // 外部判空判满接口
        bool Empty() const // 判空
        {
            std::unique_lock<std::mutex> locker(m_mutex);
            return m_queue.size() == 0;
        }
        bool Full() const // 判满
        {
            std::unique_lock<std::mutex> locker(m_mutex);
            return m_queue.size() >= m_maxsize;
        }

        size_t Size() const // 返回当前任务数
        {
            std::unique_lock<std::mutex> locker(m_mutex); // 加锁要被修改，常方法不能被修改
            return m_queue.size();
        }

        size_t MaxSize() const
        {
            std::unique_lock<std::mutex> locker(m_mutex);
            return m_maxsize;
        }
    };
}

#endif
