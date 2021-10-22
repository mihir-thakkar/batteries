// Copyright 2021 Tony Astolfi
//
#pragma once
#ifndef BATTERIES_ASYNC_TASK_HPP
#define BATTERIES_ASYNC_TASK_HPP

#include <batteries/assert.hpp>
#include <batteries/async/continuation.hpp>
#include <batteries/async/debug_info_decl.hpp>
#include <batteries/async/future.hpp>
#include <batteries/async/handler.hpp>
#include <batteries/async/io_result.hpp>
#include <batteries/case_of.hpp>
#include <batteries/config.hpp>
#include <batteries/finally.hpp>
#include <batteries/int_types.hpp>
#include <batteries/optional.hpp>
#include <batteries/segv.hpp>
#include <batteries/utility.hpp>

#ifdef BATT_GLOG_AVAILABLE
#include <glog/logging.h>
#endif  // BATT_GLOG_AVAILABLE

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wswitch-enum"
#pragma clang diagnostic ignored "-Wunused-variable"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
#endif  // __clang__

#include <boost/asio/deadline_timer.hpp>
#include <boost/asio/defer.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/executor.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>
#include <boost/exception/diagnostic_information.hpp>
#include <boost/preprocessor/cat.hpp>

#ifdef __clang__
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif  // __clang__

#include <atomic>
#include <bitset>
#include <functional>
#include <future>
#include <utility>

namespace batt {

// Returns the lowest unused global thread id number; repeated calls to `next_thread_id()` will return
// monotonically increasing values.
//
i32 next_thread_id();

// Returns a reference to the thread-local id for the current thread.
//
i32& this_thread_id();

// A user-space cooperatively scheduled thread of control.
//
class Task
    : public boost::intrusive::list_base_hook<boost::intrusive::link_mode<boost::intrusive::auto_unlink>>
{
    friend class DebugInfoFrame;

    friend void print_debug_info(DebugInfoFrame* p, std::ostream& out);

   public:
    using state_type = u32;

    BATT_STRONG_TYPEDEF_WITH_DEFAULT(i32, Priority, 0);

    using executor_type = boost::asio::any_io_executor;

    using AllTaskList = boost::intrusive::list<Task, boost::intrusive::constant_time_size<false>>;

    //==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -

    // Thread-local counter that limits stack growth while running Tasks via `dispatch`.
    //
    static usize& nesting_depth();

    // The upper bound on `nesting_depth()`.  When scheduling a task to run via `dispatch` would increase the
    // nesting depth on the current thread to greater than `kMaxNestingDepth`, `post` is used instead.
    //
    static constexpr usize kMaxNestingDepth = 8;

    // The number of bytes to statically allocate for handler memory buffers.
    //
    static constexpr usize kHandlerMemoryBytes = 128;

    //==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -

    // Set when code within the task requests a signal, because it is awaiting some external async event.
    //
    static constexpr state_type kNeedSignal = state_type{1} << 0;

    // Set when the continuation generated by an `await` is invoked.
    //
    static constexpr state_type kHaveSignal = state_type{1} << 1;

    // Set when the task is not currently running.
    //
    static constexpr state_type kSuspended = state_type{1} << 2;

    // Indicates the task has finished execution.
    //
    static constexpr state_type kTerminated = state_type{1} << 3;

    // Set to request that the task collect a stack trace the next time it resumes.
    //
    static constexpr state_type kStackTrace = state_type{1} << 4;

    // Spin-lock bit to serialize access to the sleep timer member of the Task.
    //
    static constexpr state_type kSleepTimerLock = state_type{1} << 5;

    // Spin-lock bit to serialize access to the completions handlers list.
    //
    static constexpr state_type kCompletionHandlersLock = state_type{1} << 6;

    // Used to save the value of the `kSleepTimerLock` bit when the Task is suspended (e.g., in `await` or
    // `yield`).  The Task should not hold any spinlocks while it is suspended, so we don't deadlock.  Rather,
    // the sleep timer lock is temporarily released while suspended and then re-acquired when the task is
    // resumed.
    //
    static constexpr state_type kSleepTimerLockSuspend = state_type{1} << 7;

    // The number of state flags defined above.
    //
    static constexpr usize kNumStateFlags = 8;

    // The bitset type for a state.
    //
    using StateBitset = std::bitset<kNumStateFlags>;

    // Returns true iff the given state is *not* a suspended state.
    //
    static constexpr bool is_running_state(state_type state)
    {
        return (state & kSuspended) == 0;
    }

    // Returns true iff the task is not currently running, but is ready to be resumed.
    //
    static constexpr bool is_ready_state(state_type state)
    {
        return
            // The task must be suspended, but not terminated.
            //
            ((state & (kSuspended | kTerminated)) == kSuspended) &&

            (  // *Either* task is not waiting for a signal...
               //
                (state & (kNeedSignal | kHaveSignal)) == 0 ||

                // ...*Or* task was waiting for a signal, and it received one.
                //
                (state & (kNeedSignal | kHaveSignal)) == (kNeedSignal | kHaveSignal)) &&

            // The stack trace flag is not set.
            //
            ((state & kStackTrace) == 0);
    }

    // Returns true if the passed state represents a fully terminated task.
    //
    static constexpr bool is_terminal_state(state_type state)
    {
        return (state & (kSuspended | kTerminated)) == (kSuspended | kTerminated);
    }

    //==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -

    struct DebugTrace {
        boost::stacktrace::stacktrace stack_trace;
        std::string debug_info;
        StateBitset state_bits;
        isize stack_growth_bytes;
    };

    //==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -

    // Returns a reference to the global mutex that protects the global task list.
    //
    static std::mutex& global_mutex();

    // Returns a reference to the global task list.  Must only be accessed while holding a lock on
    // `Task::global_mutex()`.
    //
    static AllTaskList& all_tasks();

    // Returns a reference to the currently running Task, if there is one.
    //
    static Task& current();

    // Dumps stack traces and debug info from all Tasks and threads to stderr.
    //
    static i32 backtrace_all();

    // Yield control from the current Task/thread, allowing other tasks to run.
    //
    static void yield();

    // Put the current Task/thread to sleep for the specified duration.
    //
    template <typename Duration>
    static ErrorCode sleep(const Duration& duration)
    {
        Task* current_task = Task::current_ptr();
        if (current_task) {
            return current_task->sleep_impl(duration);
        }

        std::this_thread::sleep_for(std::chrono::nanoseconds(duration.total_nanoseconds()));
        return ErrorCode{};
    }

    // Suspend the current thread/Task until an asynchronous event occurs.
    //
    // The param `fn` is passed a continuation handler that will cause this Task to wake up, causing `await`
    // to return an instance of type `R` constructed from the arguments passed to the handler.  For example,
    // `await` can be used to turn an async socket read into a synchronous call:
    //
    // ```
    // boost::asio::ip::tcp::socket s;
    //
    // using ReadResult = std::pair<boost::system::error_code, std::size_t>;

    // ReadResult r = Task::await<ReadResult>([&](auto&& handler) {
    //     s.async_read_some(buffers, BATT_FORWARD(handler));
    //   });
    //
    // if (r.first) {
    //   std::cout << "Error! ec=" << r.first;
    // } else {
    //   std::cout << r.second << " bytes were read.";
    // }
    // ```
    //
    template <typename R, typename Fn>
    static R await(Fn&& fn)
    {
        // If there is a Task active on the current thread, use the Task impl of await.
        //
        Task* current_task = Task::current_ptr();
        if (current_task) {
            return current_task->template await_impl<R>(BATT_FORWARD(fn));
        }

        //---------------
        // This is the generic thread (non-Task) implementation:
        //
        HandlerMemory<kHandlerMemoryBytes> handler_memory;
        std::promise<R> prom;
        std::atomic<bool> ok_to_exit{false};

        BATT_FORWARD(fn)
        (make_custom_alloc_handler(handler_memory, [&prom, &ok_to_exit](auto&&... args) {
            prom.set_value(R{BATT_FORWARD(args)...});
            ok_to_exit.store(true);
        }));

        auto wait_for_promise = batt::finally([&] {
            while (!ok_to_exit.load()) {
                std::this_thread::yield();
            }
        });

        // TODO [tastolfi 2020-12-01] - detect deadlock here

        return prom.get_future().get();
    }

    template <typename R, typename Fn>
    static R await(batt::StaticType<R>, Fn&& fn)
    {
        return Task::await<R>(BATT_FORWARD(fn));
    }

    template <typename T>
    static T await(const Future<T>& future_result)
    {
        return Task::await<T>([&](auto&& handler) {
            future_result.async_wait(BATT_FORWARD(handler));
        });
    }

    static std::string default_name()
    {
        return "(anonymous)";
    }

    static Priority current_priority()
    {
        Task* current_task = Task::current_ptr();
        if (current_task == nullptr) {
            return Priority{0};
        }
        return current_task->get_priority();
    }

    //==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    template <typename BodyFn>
    explicit Task(const boost::asio::any_io_executor& ex, StackSize stack_size, BodyFn&& body_fn) noexcept
        : Task{ex, BATT_FORWARD(body_fn), /*name=*/default_name(), stack_size}
    {
    }

    template <typename BodyFn>
    explicit Task(const boost::asio::any_io_executor& ex, BodyFn&& body_fn,
                  std::string&& name = default_name(), StackSize stack_size = StackSize{16 * 1024},
                  StackType stack_type = StackType::kFixedSize, Optional<Priority> priority = None) noexcept
        : name_(std::move(name))
        , ex_(ex)
        , priority_{priority.value_or(Task::current_priority() + 100)}
    {
        this->self_ = callcc(  //
            stack_size, stack_type,
            [body_fn = ::batt::make_optional(BATT_FORWARD(body_fn)), this](Continuation&& parent) mutable {
                auto work_guard = boost::asio::make_work_guard(this->ex_);

                this->pre_entry(std::move(parent));

                try {
                    (*body_fn)();
                } catch (...) {
#ifdef BATT_GLOG_AVAILABLE
                    LOG(WARNING)
#else
                    std::cerr
#endif  // BATT_GLOG_AVAILABLE
                        << "task fn exited via unhandled exception [task='" << this->name_
                        << "']: " << boost::current_exception_diagnostic_information();
                }
                body_fn = None;

                return this->post_exit();
            });

        {
            std::unique_lock<std::mutex> lock{global_mutex()};
            all_tasks().push_back(*this);
        }

        this->handle_event(kSuspended);
    }

    ~Task() noexcept;

    i32 id() const
    {
        return this->id_;
    }

    std::string_view name() const
    {
        return this->name_;
    }

    Priority get_priority() const
    {
        return Priority{this->priority_.load()};
    }

    void set_priority(Priority new_priority)
    {
        this->priority_.store(new_priority);
    }

    usize stack_pos() const
    {
        volatile u8 pos = 0;

        if (&pos < this->stack_base_) {
            return this->stack_base_ - &pos;
        } else {
            return &pos - this->stack_base_;
        }
    }

    void join();

    bool wake();

    executor_type get_executor() const
    {
        return this->ex_;
    }

    template <typename F>
    void call_when_done(F&& handler)
    {
        if (this->state_.load() & kTerminated) {
            BATT_FORWARD(handler)();
            return;
        }

        SpinLockGuard lock{this, kCompletionHandlersLock};
        push_handler(&this->completion_handlers_, BATT_FORWARD(handler));
    }

    // =#=#==#==#===============+=+=+=+=++=++++++++++++++-++-+--+-+----+---------------

    DebugInfoFrame* debug_info = nullptr;

    // =#=#==#==#===============+=+=+=+=++=++++++++++++++-++-+--+-+----+---------------

   private:
    //=#=#==#==#===============+=+=+=+=++=++++++++++++++-++-+--+-+----+---------------
    //
    class SpinLockGuard
    {
       public:
        explicit SpinLockGuard(Task* task, state_type mask) noexcept : task_{task}, mask_{mask}
        {
            task_->spin_lock(mask);
        }

        SpinLockGuard(const SpinLockGuard&) = delete;
        SpinLockGuard& operator=(const SpinLockGuard&) = delete;

        ~SpinLockGuard() noexcept
        {
            task_->spin_unlock(mask_);
        }

       private:
        Task* const task_;
        const state_type mask_;
    };

    //==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -

    static i32 next_id();

    static Task*& current_ptr();

    //==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -

    // Invoked in the task's context prior to entering the task function; yields control back to the parent
    // context, ensuring that the task function is invoked via the executor.
    //
    void pre_entry(Continuation&& parent) noexcept;

    // Invoked in the task's context after the task function returns.
    //
    Continuation post_exit() noexcept;

    // Suspend the task, resuming the parent context.
    //
    void yield_impl();

    // Set the timer to expire after the given duration, suspending the task in a manner identical to
    // `await_impl`.
    //
    ErrorCode sleep_impl(const boost::posix_time::time_duration& duration);

    // Clears state flags kSuspended|kNeedSignal|kHaveSignal and resumes the task via its executor.  If
    // `force_post` is true, the resume is always scheduled via boost::asio::post.  Otherwise, if
    // Task::nesting_depth() is below the limit, boost::asio::dispatch is used instead.  `observed_state` is
    // the last observed value of `Task::state_`.
    //
    void schedule_to_run(state_type observed_state, bool force_post = false);

    // Resumes execution of the task on the current thread; this is the normal code path, when the task
    // receives a signal or is ready to run.  Stack traces collected on the task do not use this method;
    // rather they directly call resume_impl after atomically setting the kStackTrace bit (conditional on the
    // thread *not* being in a running, ready-to-run, or terminal state).
    //
    void run();

    // Switch the current thread context to the task and resume execution.
    //
    void resume_impl();

    // `fn` is passed a callable acting as the continutation of the suspended Task.  This continuation may
    // receive any set of arguments from which the await operation's result type `R` can be constructed.
    //
    template <typename R, typename Fn>
    R await_impl(Fn&& fn)
    {
        Optional<R> result;

        HandlerMemory<kHandlerMemoryBytes> handler_memory;

        const state_type prior_state = this->state_.fetch_or(kNeedSignal);
        BATT_CHECK_NE((prior_state & kHaveSignal), kHaveSignal) << "prior_state=" << StateBitset{prior_state};

        BATT_FORWARD(fn)
        (/*callback handler=*/make_custom_alloc_handler(handler_memory, [this, &result](auto&&... args) {
            result.emplace(BATT_FORWARD(args)...);

            this->handle_event(kHaveSignal);
        }));

        // Suspend this Task.  It will not be in a ready state until the kHaveSignal event has been handled.
        //
        this->yield_impl();

        return std::move(*result);
    }

    // Tells the task to handle events which may affect its running/suspended state.  This function is safe to
    // invoke inside the task or outside.  `event_mask` *must* be one of:
    //
    // - kHaveSignal
    // - kSuspended
    // - kTerminated
    //
    void handle_event(state_type event_mask);

    // Acquire a spin lock on the given state bit mask.  `lock_mask` must be one of:
    //
    // - kSleepTimerLock
    // - kCompletionHandlersLock
    //
    // Locks acquired via this function are not recursive.
    //
    state_type spin_lock(state_type lock_mask);

    // Same as `spin_lock`, except only try once to acquire the lock.  Returns `true` iff the lock was
    // acquired. Sets `prior_state` equal to the last observed value of `state_`.
    //
    bool try_spin_lock(state_type lock_mask, state_type& prior_state);

    // Release the given spin lock bit.  `lock_mask` must be a legal value passed to
    // `spin_lock`/`try_spin_lock`, and the calling thread must currently hold a lock on the given bit
    // (acquired via `spin_lock`/`try_spin_lock`).
    //
    void spin_unlock(state_type lock_mask);

    // Attempt to collect a stack trace from the task, dumping it to stderr if successful.  This will fail if
    // the task is running, ready-to-run, or terminated.  Returns true iff successful.
    //
    bool try_dump_stack_trace();

    //==#==========+==+=+=++=+++++++++++-+-+--+----- --- -- -  -  -   -

    const i32 id_ = next_id();
    const std::string name_;
    executor_type ex_;
    Continuation scheduler_;
    Continuation self_;
    std::atomic<state_type> state_{kSuspended};
    std::atomic<Priority::value_type> priority_;
    Promise<NoneType> promise_;
    Optional<boost::asio::deadline_timer> sleep_timer_;
    Optional<boost::stacktrace::stacktrace> stack_trace_;
    HandlerList<> completion_handlers_;
    HandlerMemory<kHandlerMemoryBytes> activate_memory_;
    const volatile u8* stack_base_ = nullptr;
    // TODO [tastolfi 2021-10-18]
    //    std::atomic<DebugTrace*> debug_trace_{nullptr};
};

}  // namespace batt

#endif  // BATTERIES_ASYNC_TASK_HPP

#if BATT_HEADER_ONLY
#include "task_impl.hpp"
#endif
#if BATT_HEADER_ONLY
#include "debug_info_impl.hpp"
#endif
