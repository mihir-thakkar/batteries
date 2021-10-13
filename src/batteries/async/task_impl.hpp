// Copyright 2021 Tony Astolfi
//
#include <batteries/async/task.hpp>
//

#include <batteries/config.hpp>
#include <batteries/stream_util.hpp>

namespace batt {

BATT_INLINE_IMPL i32 next_thread_id()
{
    static std::atomic<i32> id_{1000};
    return id_.fetch_add(1);
}

BATT_INLINE_IMPL i32& this_thread_id()
{
    thread_local i32 id_ = next_thread_id();
    return id_;
}

//=#=#==#==#===============+=+=+=+=++=++++++++++++++-++-+--+-+----+---------------
// Task static methods.

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
BATT_INLINE_IMPL usize& Task::nesting_depth()
{
    thread_local usize depth_ = 0;
    return depth_;
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
BATT_INLINE_IMPL i32 Task::next_id()
{
    static std::atomic<i32> id_{1};
    return id_.fetch_add(1);
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
BATT_INLINE_IMPL Task*& Task::current_ptr()
{
    thread_local Task* ptr_ = nullptr;
    return ptr_;
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
BATT_INLINE_IMPL Task& Task::current()
{
    return *current_ptr();
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
BATT_INLINE_IMPL std::mutex& Task::global_mutex()
{
    static std::mutex m_;
    return m_;
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
BATT_INLINE_IMPL auto Task::all_tasks() -> AllTaskList&
{
    static AllTaskList l_;
    return l_;
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
BATT_INLINE_IMPL void Task::yield()
{
    Task* current_task = Task::current_ptr();
    if (current_task) {
        current_task->yield_impl();
        return;
    }

    std::this_thread::yield();
}

//=#=#==#==#===============+=+=+=+=++=++++++++++++++-++-+--+-+----+---------------
// Task instance methods.

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
BATT_INLINE_IMPL Task::~Task() noexcept
{
    BATT_CHECK(!this->parent_);
    BATT_CHECK(!this->self_);
    BATT_CHECK(is_terminal_state(this->state_.load())) << "state=" << std::bitset<9>{this->state_.load()};
    {
        std::unique_lock<std::mutex> lock{global_mutex()};
        this->unlink();
    }
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
BATT_INLINE_IMPL void Task::pre_entry(Continuation&& parent) noexcept
{
#ifdef BATT_GLOG_AVAILABLE
    VLOG(1) << "Task{.name=" << this->name_ << ",} created on thread " << this_thread_id();
#endif

    volatile u8 base = 0;

    this->stack_base_ = &base;
    this->parent_ = parent.resume();

#ifdef BATT_GLOG_AVAILABLE
    VLOG(1) << "Task{.name=" << this->name_ << ",} started on thread " << this_thread_id();
#endif
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
BATT_INLINE_IMPL Continuation Task::post_exit() noexcept
{
    HandlerList<> local_handlers = [&] {
        SpinLockGuard lock{this, kCompletionHandlersLock};
        return std::move(this->completion_handlers_);
    }();

    Continuation parent = std::move(this->parent_);

    this->handle_event(kTerminated);

    invoke_all_handlers(&local_handlers);

    return parent;
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
BATT_INLINE_IMPL void Task::yield_impl()
{
    BATT_CHECK(this->parent_) << std::bitset<9>{this->state_.load()};

    for (;;) {
        this->parent_ = this->parent_.resume();

        // If a stack trace has been requested, print it and suspend.
        //
        if (this->state_ & kStackTrace) {
            this->stack_trace_.emplace();
            continue;
        }
        break;
    }

    BATT_CHECK_EQ(Task::current_ptr(), this);
    BATT_CHECK(this->parent_) << std::bitset<9>{this->state_.load()};
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
BATT_INLINE_IMPL ErrorCode Task::sleep_impl(const boost::posix_time::time_duration& duration)
{
    SpinLockGuard lock{this, kSleepTimerLock};

    if (!this->sleep_timer_) {
        this->sleep_timer_.emplace(this->ex_);
    }

    this->sleep_timer_->expires_from_now(duration);

    return await_impl<ErrorCode>([&](auto&& handler) {
        this->sleep_timer_->async_wait(BATT_FORWARD(handler));
    });
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
BATT_INLINE_IMPL void Task::join()
{
    await(get_future(this->promise_));
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
BATT_INLINE_IMPL bool Task::wake()
{
    SpinLockGuard lock{this, kSleepTimerLock};

    if (this->sleep_timer_) {
        ErrorCode ec;
        this->sleep_timer_->cancel(ec);
        if (!ec) {
            return true;
        }
    }
    return false;
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
BATT_INLINE_IMPL void Task::handle_event(u32 event_mask)
{
    const u32 new_state = this->state_.fetch_or(event_mask) | event_mask;

    if (is_ready_state(new_state)) {
        this->schedule_to_run(new_state);
    } else if (is_terminal_state(new_state)) {
#ifdef BATT_GLOG_AVAILABLE
        VLOG(1) << "[Task] " << this->name_ << " exiting";
#endif
        this->promise_.set_value(make_copy(None));
        //
        // There must be nothing after this point!
    }
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
BATT_INLINE_IMPL void Task::schedule_to_run(u32 observed_state, bool force_post)
{
    for (;;) {
        if (!is_ready_state(observed_state)) {
            return;
        }
        const u32 target_state = observed_state & ~(kSuspended | kNeedSignal | kHaveSignal);
        if (this->state_.compare_exchange_weak(observed_state, target_state)) {
            break;
        }
    }

    // If we dispatch, it may preempt the currently running task; if we post, the current task stays
    // running.  There are a couple of things to do here:
    //
    // - TODO [tastolfi 2020-12-12] If running preempts an existing task, reschedule it on another thread if
    //   it is ready to run?  This requires a bit of thought; it's non-obvious how to schedule the current
    //   task elsewhere...
    // - TODO [tastolfi 2020-12-12] If `this` ran for a very short time last time it ran -OR- `current` has
    //   been running for a long time, then `dispatch`, otherwise `post`.
    //
    auto activation = make_custom_alloc_handler(this->activate_memory_, [this] {
        this->run();
    });

    if (Task::nesting_depth() < kMaxNestingDepth && !force_post) {
        ++Task::nesting_depth();
        auto on_scope_exit = batt::finally([] {
            --Task::nesting_depth();
        });

        boost::asio::dispatch(this->ex_, std::move(activation));
    } else {
        boost::asio::post(this->ex_, std::move(activation));
    }
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
BATT_INLINE_IMPL void Task::run()
{
    // If the sleep timer lock was held *the last time* we yielded control, then re-acquire it now.
    //
    u32 observed_state = this->state_.load();
    if (observed_state & kSleepTimerLockSuspend) {
        for (;;) {
            if (observed_state & kSleepTimerLock) {
                observed_state = this->state_.load();
                continue;
            }
            const u32 target_state = (observed_state & ~kSleepTimerLockSuspend) | kSleepTimerLock;
            if (this->state_.compare_exchange_weak(observed_state, target_state)) {
                break;
            }
        }
    }

    this->resume_impl();

    // If the sleep timer lock was held *this time* when we yielded, then atomically release it and set the
    // kSleepTimerLockSuspend bit so we re-acquire it next time.
    //
    observed_state = this->state_.load();
    if (observed_state & kSleepTimerLock) {
        for (;;) {
            const u32 target_state = (observed_state & ~kSleepTimerLock) | kSleepTimerLockSuspend;
            if (this->state_.compare_exchange_weak(observed_state, target_state)) {
                break;
            }
        }
    }

    this->handle_event(kSuspended);
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
BATT_INLINE_IMPL void Task::resume_impl()
{
    Task* const saved_task = Task::current_ptr();
    Task::current_ptr() = this;
    auto on_scope_exit = batt::finally([saved_task] {
        Task::current_ptr() = saved_task;
    });

    BATT_CHECK(this->self_);
    this->self_ = this->self_.resume();
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
BATT_INLINE_IMPL u32 Task::spin_lock(u32 lock_mask)
{
    u32 prior_state = 0;

    if (!this->try_spin_lock(lock_mask, prior_state)) {
        for (;;) {
            std::this_thread::yield();
            if (this->try_spin_lock(lock_mask, prior_state)) {
                break;
            }
        }
    }

    return prior_state;
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
BATT_INLINE_IMPL bool Task::try_spin_lock(u32 lock_mask, u32& prior_state)
{
    prior_state = this->state_.fetch_or(lock_mask);
    return (prior_state & lock_mask) == 0;
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
BATT_INLINE_IMPL void Task::spin_unlock(u32 lock_mask)
{
    this->state_.fetch_and(~lock_mask);
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
BATT_INLINE_IMPL i32 Task::backtrace_all()
{
    i32 i = 0;
    std::unique_lock<std::mutex> lock{global_mutex()};
    std::cerr << std::endl;
    for (auto& t : all_tasks()) {
        std::cerr << "-- Task{id=" << t.id() << ", name=" << t.name_ << "} -------------" << std::endl;
        if (!t.try_dump_stack_trace()) {
            std::cerr << "(running)" << std::endl;
        }
        std::cerr << std::endl;
        ++i;
    }
    std::cerr << i << " Tasks are active" << std::endl;

    print_all_threads_debug_info(std::cerr);

    return i;
}

//==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -
//
BATT_INLINE_IMPL bool Task::try_dump_stack_trace()
{
    u32 observed_state = this->state_.load();
    for (;;) {
        if (is_running_state(observed_state) || is_ready_state(observed_state) ||
            is_terminal_state(observed_state) || (observed_state & kStackTrace)) {
            return false;
        }
        const u32 target_state = observed_state | kStackTrace;
        if (this->state_.compare_exchange_weak(observed_state, target_state)) {
            break;
        }
    }

    std::cerr << "(suspended) state=" << std::bitset<9>{this->state_}
              << " hdlr,timr,lock,dump,term,susp,have,need (0=running)" << std::endl;

    if (this->debug_info) {
        std::cerr << "DEBUG:" << std::endl;
        print_debug_info(this->debug_info, std::cerr);
    }

    this->resume_impl();

    BATT_CHECK(this->stack_trace_);

    std::cerr << *this->stack_trace_ << std::endl;
    this->stack_trace_ = None;

    const u32 after_state = this->state_.fetch_and(~kStackTrace) & ~kStackTrace;
    this->schedule_to_run(after_state, /*force_post=*/true);

    return true;
}

}  // namespace batt