#ifndef CHAT_TASK_EXECUTOR_HPP
#define CHAT_TASK_EXECUTOR_HPP
#include <functional>
#include <cstddef>
namespace pool {
using TaskType = std::function<void()>;
class TaskExecutor {
public:
    virtual ~TaskExecutor() = default;
    virtual bool submitTask(TaskType task) = 0;
    virtual std::size_t queueSize() const = 0;
    virtual std::size_t queueCapacity() const = 0;
    virtual void Stop() = 0;
};
}
#endif
