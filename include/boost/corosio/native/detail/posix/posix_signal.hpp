//
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_POSIX_POSIX_SIGNAL_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_POSIX_POSIX_SIGNAL_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/signal_set.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/detail/scheduler_op.hpp>
#include <boost/capy/continuation.hpp>
#include <boost/capy/ex/executor_ref.hpp>

#include <coroutine>
#include <cstddef>
#include <optional>
#include <stop_token>
#include <system_error>

namespace boost::corosio {

namespace detail {

// Forward declarations
class posix_signal_service;

// Maximum signal number supported (NSIG is typically 64 on Linux)
enum
{
    max_signal_number = 64
};

// signal_op - pending wait operation

struct signal_op : scheduler_op
{
    std::coroutine_handle<> h;
    capy::continuation cont;
    capy::executor_ref d;
    std::error_code* ec_out   = nullptr;
    int* signal_out           = nullptr;
    int signal_number         = 0;
    posix_signal_service* svc = nullptr; // For work_finished callback

    void operator()() override;
    void destroy() override;
};

// signal_registration - per-signal registration tracking

struct signal_registration
{
    int signal_number                  = 0;
    signal_set::flags_t flags          = signal_set::none;
    signal_set::implementation* owner  = nullptr;
    std::size_t undelivered            = 0;
    signal_registration* next_in_table = nullptr;
    signal_registration* prev_in_table = nullptr;
    signal_registration* next_in_set   = nullptr;
};

// posix_signal - per-signal_set implementation

class posix_signal final
    : public signal_set::implementation
    , public intrusive_list<posix_signal>::node
{
    friend class posix_signal_service;

    posix_signal_service& svc_;
    signal_registration* signals_ = nullptr;
    signal_op pending_op_;
    bool waiting_   = false;
    bool cancelled_ = false;

    /** Routes a stop request into the service's per-operation cancel.

        Deliberately NOT `cancel_wait`: that sets the sticky `cancelled_`
        latch, which belongs to `cancel()` and scopes to the whole set. A
        stop token scopes to one wait, so a late fire must be a no-op
        rather than poisoning the next wait.
    */
    struct token_canceller
    {
        posix_signal* self;
        void operator()() const noexcept;
    };

    /** Armed for the duration of one wait; see wait().

        Never reset while `posix_signal_service::mutex_` is held:
        `~stop_callback` blocks until a concurrently running callback
        returns, and that callback takes the same mutex.
    */
    std::optional<std::stop_callback<token_canceller>> stop_cb_;

    /** Set when a stop request arrives for the current wait.

        Closes a lost-wakeup race. The callback is armed before
        `start_wait` takes the lock, so a request landing in that window
        finds `waiting_ == false` and would otherwise return having done
        nothing, leaving `start_wait` to park the wait forever. Every
        other op survives this because `coro_op::on_cancel()` defaults to
        `request_cancel()`, which sets a persistent flag the completion
        decode reads later; this is that flag for the signal path.

        Distinct from `cancelled_` on purpose: `cancelled_` is the sticky
        per-set latch `cancel()` owns, this is per-operation.
    */
    bool token_cancelled_ = false;

public:
    explicit posix_signal(posix_signal_service& svc) noexcept;

    std::coroutine_handle<> wait(
        std::coroutine_handle<>,
        capy::executor_ref,
        std::stop_token,
        std::error_code*,
        int*) override;

    std::error_code add(int signal_number, signal_set::flags_t flags) override;
    std::error_code remove(int signal_number) override;
    std::error_code clear() override;
    void cancel() noexcept override;

    /// Disarm the wait's stop callback. Called before teardown.
    void disarm_stop() noexcept
    {
        stop_cb_.reset();
    }
};

} // namespace detail

} // namespace boost::corosio

#endif // BOOST_COROSIO_POSIX

#endif // BOOST_COROSIO_NATIVE_DETAIL_POSIX_POSIX_SIGNAL_HPP
