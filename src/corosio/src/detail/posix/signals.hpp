//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_POSIX_SIGNALS_HPP
#define BOOST_COROSIO_DETAIL_POSIX_SIGNALS_HPP

#include "src/detail/config_backend.hpp"

#if defined(BOOST_COROSIO_SIGNAL_POSIX)

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/signal_set.hpp>
#include <boost/capy/ex/any_coro.hpp>
#include <boost/capy/ex/any_executor_ref.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include "src/detail/intrusive.hpp"
#include <boost/system/error_code.hpp>
#include <boost/system/result.hpp>

#include "src/detail/scheduler_op.hpp"

#include <coroutine>
#include <cstddef>
#include <mutex>
#include <stop_token>

#include <signal.h>

/*
    POSIX Signal Implementation - Header
    =====================================

    This header declares the internal types for POSIX signal handling.
    See signals.cpp for the full implementation overview.

    Data Structure Summary:

    signal_op           - Pending async_wait operation, posted to scheduler on signal
    signal_registration - Links a signal to its owning signal_set; tracks queued signals
    posix_signal_impl   - Per-signal_set state (derives from signal_set::signal_set_impl)
    posix_signals       - Per-execution_context service managing all signal_sets

    Pointer Relationships:

    signal_registration has two linked list memberships:
      - next_in_set: Singly-linked list of all signals in one signal_set (sorted)
      - prev/next_in_table: Doubly-linked list of all registrations for one signal
        number across all signal_sets in one execution_context

    This dual-linking allows efficient:
      - Per-set iteration (add/remove/clear operations)
      - Per-signal iteration (signal delivery to all waiting sets)
*/

namespace boost {
namespace corosio {
namespace detail {

class epoll_scheduler;
class posix_signals;
class posix_signal_impl;

// Maximum signal number supported (NSIG is typically 64 on Linux)
enum { max_signal_number = 64 };

//------------------------------------------------------------------------------

/** Signal wait operation state. */
struct signal_op : scheduler_op
{
    capy::any_coro h;
    capy::any_executor_ref d;
    system::error_code* ec_out = nullptr;
    int* signal_out = nullptr;
    int signal_number = 0;
    posix_signals* svc = nullptr;  // For work_finished callback

    void operator()() override;
    void destroy() override;
};

//------------------------------------------------------------------------------

/** Per-signal registration tracking. */
struct signal_registration
{
    int signal_number = 0;
    signal_set::flags_t flags = signal_set::none;
    posix_signal_impl* owner = nullptr;
    std::size_t undelivered = 0;
    signal_registration* next_in_table = nullptr;
    signal_registration* prev_in_table = nullptr;
    signal_registration* next_in_set = nullptr;
};

//------------------------------------------------------------------------------

/** Signal set implementation for POSIX.

    This class contains the state for a single signal_set, including
    registered signals and pending wait operation.

    @note Internal implementation detail. Users interact with signal_set class.
*/
class posix_signal_impl
    : public signal_set::signal_set_impl
    , public intrusive_list<posix_signal_impl>::node
{
    friend class posix_signals;

    posix_signals& svc_;
    signal_registration* signals_ = nullptr;
    signal_op pending_op_;
    bool waiting_ = false;

public:
    explicit posix_signal_impl(posix_signals& svc) noexcept;

    void release() override;

    void wait(
        std::coroutine_handle<>,
        capy::any_executor_ref,
        std::stop_token,
        system::error_code*,
        int*) override;

    system::result<void> add(int signal_number, signal_set::flags_t flags) override;
    system::result<void> remove(int signal_number) override;
    system::result<void> clear() override;
    void cancel() override;
};

//------------------------------------------------------------------------------

/** POSIX signal management service.

    This service owns all signal set implementations and coordinates
    their lifecycle using C signal handlers. It provides:

    - Signal implementation allocation and deallocation
    - Signal registration via C signal()
    - Global signal state management
    - Graceful shutdown - destroys all implementations when io_context stops

    @par Thread Safety
    All public member functions are thread-safe.
*/
class posix_signals : public capy::execution_context::service
{
public:
    using key_type = posix_signals;

    /** Construct the signal service.

        @param ctx Reference to the owning execution_context.
    */
    explicit posix_signals(capy::execution_context& ctx);

    /** Destroy the signal service. */
    ~posix_signals();

    posix_signals(posix_signals const&) = delete;
    posix_signals& operator=(posix_signals const&) = delete;

    /** Shut down the service. */
    void shutdown() override;

    /** Create a new signal implementation. */
    posix_signal_impl& create_impl();

    /** Destroy a signal implementation. */
    void destroy_impl(posix_signal_impl& impl);

    /** Add a signal to a signal set.

        @param impl The signal implementation to modify.
        @param signal_number The signal to register.
        @param flags The flags to apply when registering the signal.
        @return Success, or an error.
    */
    system::result<void> add_signal(
        posix_signal_impl& impl,
        int signal_number,
        signal_set::flags_t flags);

    /** Remove a signal from a signal set.

        @param impl The signal implementation to modify.
        @param signal_number The signal to unregister.
        @return Success, or an error.
    */
    system::result<void> remove_signal(
        posix_signal_impl& impl,
        int signal_number);

    /** Remove all signals from a signal set.

        @param impl The signal implementation to clear.
        @return Success, or an error.
    */
    system::result<void> clear_signals(posix_signal_impl& impl);

    /** Cancel pending wait operations.

        @param impl The signal implementation to cancel.
    */
    void cancel_wait(posix_signal_impl& impl);

    /** Start a wait operation.

        @param impl The signal implementation.
        @param op The operation to start.
    */
    void start_wait(posix_signal_impl& impl, signal_op* op);

    /** Deliver a signal to all registered services.

        Called from the C signal handler.

        @param signal_number The signal that occurred.
    */
    static void deliver_signal(int signal_number);

    /** Notify scheduler of pending work. */
    void work_started() noexcept;

    /** Notify scheduler that work completed. */
    void work_finished() noexcept;

    /** Post an operation for completion. */
    void post(signal_op* op);

private:
    static void add_service(posix_signals* service);
    static void remove_service(posix_signals* service);

    epoll_scheduler& sched_;
    std::mutex mutex_;
    intrusive_list<posix_signal_impl> impl_list_;

    // Per-signal registration table
    signal_registration* registrations_[max_signal_number];

    // Registration counts for each signal
    std::size_t registration_count_[max_signal_number];

    // Linked list of all posix_signals services for signal delivery
    posix_signals* next_ = nullptr;
    posix_signals* prev_ = nullptr;
};

} // namespace detail
} // namespace corosio
} // namespace boost

#endif // BOOST_COROSIO_SIGNAL_POSIX

#endif // BOOST_COROSIO_DETAIL_POSIX_SIGNALS_HPP
