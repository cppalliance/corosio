//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/timer.hpp>

#include <boost/corosio/detail/except.hpp>

namespace boost::corosio {

namespace detail {

// Defined in timer_service.cpp
extern timer::timer_impl* timer_service_create(capy::execution_context&);
extern void timer_service_destroy(timer::timer_impl&) noexcept;
extern timer::time_point timer_service_expiry(timer::timer_impl&) noexcept;
extern void timer_service_expires_at(timer::timer_impl&, timer::time_point);
extern void timer_service_expires_after(timer::timer_impl&, timer::duration);
extern void timer_service_cancel(timer::timer_impl&) noexcept;

} // namespace detail

timer::
~timer()
{
    if (impl_)
        detail::timer_service_destroy(get());
}

timer::
timer(capy::execution_context& ctx)
    : io_object(ctx)
{
    impl_ = detail::timer_service_create(ctx);
}

timer::
timer(timer&& other) noexcept
    : io_object(other.context())
{
    impl_ = other.impl_;
    other.impl_ = nullptr;
}

timer&
timer::
operator=(timer&& other)
{
    if (this != &other)
    {
        if (ctx_ != other.ctx_)
            detail::throw_logic_error(
                "cannot move timer across execution contexts");
        if (impl_)
            detail::timer_service_destroy(get());
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

void
timer::
cancel()
{
    detail::timer_service_cancel(get());
}

timer::time_point
timer::
expiry() const
{
    return detail::timer_service_expiry(get());
}

void
timer::
expires_at(time_point t)
{
    detail::timer_service_expires_at(get(), t);
}

void
timer::
expires_after(duration d)
{
    detail::timer_service_expires_after(get(), d);
}

} // namespace boost::corosio
