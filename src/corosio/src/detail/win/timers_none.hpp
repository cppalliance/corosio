//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_WIN_TIMERS_NONE_HPP
#define BOOST_COROSIO_DETAIL_WIN_TIMERS_NONE_HPP

#include "src/detail/win/timers.hpp"

namespace boost {
namespace corosio {
namespace detail {

// No-op timer wakeup for debugging/disabling timer support.
// Not automatically selected by make_win_timers.
class win_timers_none final : public win_timers
{
public:
    win_timers_none() = default;

    void start() override {}
    void stop() override {}
    void update_timeout(time_point) override {}
};

} // namespace detail
} // namespace corosio
} // namespace boost

#endif
