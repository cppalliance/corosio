//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_POSIX_SIGNALS_HPP
#define BOOST_COROSIO_DETAIL_POSIX_SIGNALS_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/signal_set.hpp>
#include <boost/capy/ex/any_executor_ref.hpp>
#include <boost/capy/ex/execution_context.hpp>
#include <boost/capy/core/intrusive_list.hpp>
#include <boost/system/error_code.hpp>

#include "src/detail/posix_op.hpp"
#include "src/detail/scheduler_op.hpp"

#include <coroutine>
#include <cstddef>
#include <mutex>
#include <stop_token>

#include <signal.h>

namespace boost {
namespace corosio {
namespace detail {

class posix_scheduler;
class posix_signals;
class posix_signal_impl;

// Maximum signal number supported (NSIG is typically 64 on Linux)
enum { max_signal_number = 64 };

//------------------------------------------------------------------------------

/** Signal wait operation state. */
struct signal_op : scheduler_op
{
    std::coroutine_handle<> h;
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
    posix_signal_impl* owner = nullptr;
    std::size_t undelivered = 0;
    signal_registration* next_in_table = nullptr;
    signal_registration* prev_in_table = nullptr;
    signal_registration* next_in_set = nullptr;
};

//------------------------------------------------------------------------------

/** Signal set implementation for POSIX using signalfd.

    This class contains the state for a single signal_set, including
    registered signals and pending wait operation.

    @note Internal implementation detail. Users interact with signal_set class.
*/
class posix_signal_impl
    : public signal_set::signal_set_impl
    , public capy::intrusive_list<posix_signal_impl>::node
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

    system::error_code add(int signal_number) override;
    system::error_code remove(int signal_number) override;
    system::error_code clear() override;
    void cancel() override;
};

//------------------------------------------------------------------------------

/** POSIX signal management service using signalfd.

    This service owns all signal set implementations and coordinates
    their lifecycle. It provides:

    - Signal implementation allocation and deallocation
    - Signal registration via signalfd
    - Global signal state management
    - Graceful shutdown - destroys all implementations when io_context stops

    @par Thread Safety
    All public member functions are thread-safe.

    @note Only available on POSIX platforms with signalfd support.
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
        @return Error code, or success.
    */
    system::error_code add_signal(
        posix_signal_impl& impl,
        int signal_number);

    /** Remove a signal from a signal set.

        @param impl The signal implementation to modify.
        @param signal_number The signal to unregister.
        @return Error code, or success.
    */
    system::error_code remove_signal(
        posix_signal_impl& impl,
        int signal_number);

    /** Remove all signals from a signal set.

        @param impl The signal implementation to clear.
        @return Error code, or success.
    */
    system::error_code clear_signals(posix_signal_impl& impl);

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

    posix_scheduler& sched_;
    std::mutex mutex_;
    capy::intrusive_list<posix_signal_impl> impl_list_;

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

#endif
