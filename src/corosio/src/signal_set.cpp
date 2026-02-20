//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/signal_set.hpp>
#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP
#include <boost/corosio/native/detail/iocp/win_signals.hpp>
#elif BOOST_COROSIO_POSIX
#include <boost/corosio/native/detail/posix/posix_signal_service.hpp>
#endif

namespace boost::corosio {
namespace {

#if BOOST_COROSIO_HAS_IOCP
using signal_service = detail::win_signals;
#elif BOOST_COROSIO_POSIX
using signal_service = detail::posix_signal_service;
#endif

} // namespace

// Defined here (not inline) so shared-library builds have a single
// signal_state instance.  With -fvisibility-inlines-hidden the inline
// version would give each DSO its own static, causing use-after-free
// when constructor and destructor run in different DSOs.

#if BOOST_COROSIO_HAS_IOCP
namespace detail::signal_detail {

signal_state*
get_signal_state()
{
    static signal_state state;
    return &state;
}

} // namespace detail::signal_detail
#elif BOOST_COROSIO_POSIX
namespace detail::posix_signal_detail {

signal_state*
get_signal_state()
{
    static signal_state state;
    return &state;
}

} // namespace detail::posix_signal_detail
#endif

signal_set::~signal_set() = default;

signal_set::signal_set(capy::execution_context& ctx)
    : io_signal_set(create_handle<signal_service>(ctx))
{
}

signal_set::signal_set(signal_set&& other) noexcept
    : io_signal_set(std::move(other))
{
}

signal_set&
signal_set::operator=(signal_set&& other) noexcept
{
    if (this != &other)
        h_ = std::move(other.h_);
    return *this;
}

void
signal_set::do_cancel()
{
    get().cancel();
}

std::error_code
signal_set::add(int signal_number, flags_t flags)
{
    return get().add(signal_number, flags);
}

std::error_code
signal_set::remove(int signal_number)
{
    return get().remove(signal_number);
}

std::error_code
signal_set::clear()
{
    return get().clear();
}

} // namespace boost::corosio
