//
// Copyright (c) 2026 Steve Gerbino
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
#include <boost/capy/ex/executor_ref.hpp>

#include <coroutine>
#include <cstddef>
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
    bool waiting_ = false;

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
    void cancel() override;
};

} // namespace detail

} // namespace boost::corosio

#endif // BOOST_COROSIO_POSIX

#endif // BOOST_COROSIO_NATIVE_DETAIL_POSIX_POSIX_SIGNAL_HPP
