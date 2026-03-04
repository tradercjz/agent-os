#include <agentos/scheduler/scheduler.hpp>

namespace agentos::scheduler {

std::atomic<TaskId> Scheduler::next_task_id_{1000};

} // namespace agentos::scheduler
