//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_SIGNAL_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_SIGNAL_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/signal_set.hpp>
#include <boost/corosio/detail/continuation_op.hpp>
#include <boost/corosio/detail/intrusive.hpp>
#include <boost/corosio/detail/scheduler_op.hpp>
#include <boost/capy/continuation.hpp>
#include <boost/capy/ex/executor_ref.hpp>

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <system_error>

namespace boost::corosio::detail {

// Forward declarations
class win_signals;
class win_signal;

// Maximum signal number supported
enum
{
    max_signal_number = 32
};

/** Signal wait operation state. */
struct signal_op : scheduler_op
{
    std::coroutine_handle<> h;
    detail::continuation_op cont_op;
    capy::executor_ref d;
    std::error_code* ec_out  = nullptr;
    int* signal_out          = nullptr;
    int signal_number        = 0;
    signal_op* next_in_queue = nullptr;
    win_signals* svc         = nullptr;

    static void do_complete(
        void* owner,
        scheduler_op* base,
        std::uint32_t bytes,
        std::uint32_t error);

    signal_op() noexcept;
};

/** Per-signal registration tracking. */
struct signal_registration
{
    int signal_number                  = 0;
    win_signal* owner                  = nullptr;
    std::size_t undelivered            = 0;
    signal_registration* next_in_table = nullptr;
    signal_registration* prev_in_table = nullptr;
    signal_registration* next_in_set   = nullptr;
};

/** Signal set implementation for Windows.

    This class contains the state for a single signal_set, including
    registered signals and pending wait operation.

    @note Internal implementation detail. Users interact with signal_set class.
*/
class win_signal final
    : public signal_set::implementation
    , public intrusive_list<win_signal>::node
{
    friend class win_signals;

    win_signals& svc_;
    signal_registration* signals_ = nullptr;
    signal_op pending_op_;
    bool waiting_   = false;
    bool cancelled_ = false;

public:
    explicit win_signal(win_signals& svc) noexcept;

    std::coroutine_handle<> wait(
        std::coroutine_handle<>,
        capy::executor_ref,
        std::stop_token,
        std::error_code*,
        int*) override;

    std::error_code add(int signal_number, signal_set::flags_t flags) override;
    std::error_code remove(int signal_number) override;
    std::error_code clear() override;
    void cancel() override;
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_SIGNAL_HPP
