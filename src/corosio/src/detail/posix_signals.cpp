//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef _WIN32

#include <boost/corosio/signal_set.hpp>
#include <boost/corosio/detail/except.hpp>

namespace boost {
namespace corosio {

signal_set::
~signal_set()
{
    if (impl_)
        impl_->release();
}

signal_set::
signal_set(capy::execution_context& ctx)
    : io_object(ctx)
{
    // Stub: signal_set not supported on this platform
    impl_ = nullptr;
}

signal_set::
signal_set(capy::execution_context& ctx, int)
    : io_object(ctx)
{
    impl_ = nullptr;
    detail::throw_system_error(
        make_error_code(system::errc::function_not_supported),
        "signal_set: not supported on this platform");
}

signal_set::
signal_set(
    capy::execution_context& ctx,
    int,
    int)
    : io_object(ctx)
{
    impl_ = nullptr;
    detail::throw_system_error(
        make_error_code(system::errc::function_not_supported),
        "signal_set: not supported on this platform");
}

signal_set::
signal_set(
    capy::execution_context& ctx,
    int,
    int,
    int)
    : io_object(ctx)
{
    impl_ = nullptr;
    detail::throw_system_error(
        make_error_code(system::errc::function_not_supported),
        "signal_set: not supported on this platform");
}

signal_set::
signal_set(signal_set&& other) noexcept
    : io_object(other.context())
{
    impl_ = other.impl_;
    other.impl_ = nullptr;
}

signal_set&
signal_set::
operator=(signal_set&& other)
{
    if (this != &other)
    {
        if (ctx_ != other.ctx_)
            detail::throw_logic_error(
                "signal_set::operator=: context mismatch");

        if (impl_)
            impl_->release();

        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

void
signal_set::
add(int signal_number)
{
    system::error_code ec;
    add(signal_number, ec);
    if (ec)
        detail::throw_system_error(ec, "signal_set::add");
}

void
signal_set::
add(int, system::error_code& ec)
{
    ec = make_error_code(system::errc::function_not_supported);
}

void
signal_set::
remove(int signal_number)
{
    system::error_code ec;
    remove(signal_number, ec);
    if (ec)
        detail::throw_system_error(ec, "signal_set::remove");
}

void
signal_set::
remove(int, system::error_code& ec)
{
    ec = make_error_code(system::errc::function_not_supported);
}

void
signal_set::
clear()
{
    system::error_code ec;
    clear(ec);
    if (ec)
        detail::throw_system_error(ec, "signal_set::clear");
}

void
signal_set::
clear(system::error_code& ec)
{
    ec = make_error_code(system::errc::function_not_supported);
}

void
signal_set::
cancel()
{
    // No-op: nothing to cancel on stub implementation
}

} // namespace corosio
} // namespace boost

#endif // !_WIN32
