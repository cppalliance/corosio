//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/timer.hpp>
#include <boost/corosio/detail/timer_service.hpp>

namespace boost::corosio {

timer::~timer() = default;

timer::timer(capy::execution_context& ctx)
    : io_timer(create_handle<detail::timer_service>(ctx))
{
}

timer::timer(capy::execution_context& ctx, time_point t) : timer(ctx)
{
    expires_at(t);
}

timer::timer(timer&& other) noexcept : io_timer(std::move(other)) {}

timer&
timer::operator=(timer&& other) noexcept
{
    if (this != &other)
        h_ = std::move(other.h_);
    return *this;
}

std::size_t
timer::do_cancel()
{
    return detail::timer_service_cancel(get());
}

std::size_t
timer::do_cancel_one()
{
    return detail::timer_service_cancel_one(get());
}

std::size_t
timer::do_update_expiry()
{
    return detail::timer_service_update_expiry(get());
}

} // namespace boost::corosio
