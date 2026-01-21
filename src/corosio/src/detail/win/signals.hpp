//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_WIN_SIGNALS_HPP
#define BOOST_COROSIO_DETAIL_WIN_SIGNALS_HPP

#include "src/detail/config_backend.hpp"

#if defined(BOOST_COROSIO_SIGNAL_WIN)

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/signal_set.hpp>
#include <boost/capy/ex/any_executor_ref.hpp>
#include <boost/capy/concept/io_awaitable.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/capy/detail/intrusive.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/result.hpp>

#include "src/detail/iocp/mutex.hpp"
#include "src/detail/scheduler_op.hpp"

#include <coroutine>
#include <cstddef>
#include <stop_token>

#include <signal.h>

namespace boost {
namespace corosio {
namespace detail {

class win_scheduler;
class win_signals;
class win_signal_impl;

// Maximum signal number supported
enum { max_signal_number = 32 };

//------------------------------------------------------------------------------

/** Signal wait operation state. */
struct signal_op : scheduler_op
{
    capy::any_coro h;
    capy::any_executor_ref d;
    system::error_code* ec_out = nullptr;
    int* signal_out = nullptr;
    int signal_number = 0;
    signal_op* next_in_queue = nullptr;
    win_signals* svc = nullptr;  // For work_finished callback

    void operator()() override;
    void destroy() override;
};

//------------------------------------------------------------------------------

/** Per-signal registration tracking. */
struct signal_registration
{
    int signal_number = 0;
    win_signal_impl* owner = nullptr;
    std::size_t undelivered = 0;
    signal_registration* next_in_table = nullptr;
    signal_registration* prev_in_table = nullptr;
    signal_registration* next_in_set = nullptr;
};

//------------------------------------------------------------------------------

/** Signal set implementation for Windows.

    This class contains the state for a single signal_set, including
    registered signals and pending wait operation.

    @note Internal implementation detail. Users interact with signal_set class.
*/
class win_signal_impl
    : public signal_set::signal_set_impl
    , public capy::detail::intrusive_list<win_signal_impl>::node
{
    friend class win_signals;

    win_signals& svc_;
    signal_registration* signals_ = nullptr;
    signal_op pending_op_;
    bool waiting_ = false;

public:
    explicit win_signal_impl(win_signals& svc) noexcept;

    void release() override;

    void wait(
        std::coroutine_handle<>,
        capy::any_executor_ref,
        std::stop_token,
        system::error_code*,
        int*) override;

    system::result<void> add(int signal_number) override;
    system::result<void> remove(int signal_number) override;
    system::result<void> clear() override;
    void cancel() override;
};

//------------------------------------------------------------------------------

/** Windows signal management service.

    This service owns all signal set implementations and coordinates
    their lifecycle. It provides:

    - Signal implementation allocation and deallocation
    - Signal registration via the C runtime signal() function
    - Global signal state management
    - Graceful shutdown - destroys all implementations when io_context stops

    @par Thread Safety
    All public member functions are thread-safe.

    @note Only available on Windows platforms.
*/
class win_signals : public capy::execution_context::service
{
public:
    using key_type = win_signals;

    /** Construct the signal service.

        @param ctx Reference to the owning execution_context.
    */
    explicit win_signals(capy::execution_context& ctx);

    /** Destroy the signal service. */
    ~win_signals();

    win_signals(win_signals const&) = delete;
    win_signals& operator=(win_signals const&) = delete;

    /** Shut down the service. */
    void shutdown() override;

    /** Create a new signal implementation. */
    win_signal_impl& create_impl();

    /** Destroy a signal implementation. */
    void destroy_impl(win_signal_impl& impl);

    /** Add a signal to a signal set.

        @param impl The signal implementation to modify.
        @param signal_number The signal to register.
        @return Success, or an error.
    */
    system::result<void> add_signal(
        win_signal_impl& impl,
        int signal_number);

    /** Remove a signal from a signal set.

        @param impl The signal implementation to modify.
        @param signal_number The signal to unregister.
        @return Success, or an error.
    */
    system::result<void> remove_signal(
        win_signal_impl& impl,
        int signal_number);

    /** Remove all signals from a signal set.

        @param impl The signal implementation to clear.
        @return Success, or an error.
    */
    system::result<void> clear_signals(win_signal_impl& impl);

    /** Cancel pending wait operations.

        @param impl The signal implementation to cancel.
    */
    void cancel_wait(win_signal_impl& impl);

    /** Start a wait operation.

        @param impl The signal implementation.
        @param op The operation to start.
    */
    void start_wait(win_signal_impl& impl, signal_op* op);

    /** Deliver a signal to all registered handlers.

        Called from the signal handler.

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
    static void add_service(win_signals* service);
    static void remove_service(win_signals* service);

    win_scheduler& sched_;
    win_mutex mutex_;
    capy::detail::intrusive_list<win_signal_impl> impl_list_;

    // Per-signal registration table for this service
    signal_registration* registrations_[max_signal_number];

    // Linked list of services for global signal delivery
    win_signals* next_ = nullptr;
    win_signals* prev_ = nullptr;
};

} // namespace detail
} // namespace corosio
} // namespace boost

#endif // BOOST_COROSIO_SIGNAL_WIN

#endif // BOOST_COROSIO_DETAIL_WIN_SIGNALS_HPP
