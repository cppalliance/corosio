//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/select_context.hpp>


#if !defined(_WIN32)

#include "src/detail/select/scheduler.hpp"
#include "src/detail/select/sockets.hpp"

#include <thread>

namespace boost::corosio {

select_context::
select_context()
    : select_context(std::thread::hardware_concurrency())
{
}

select_context::
select_context(
    unsigned concurrency_hint)
{
    sched_ = &make_service<detail::select_scheduler>(
        static_cast<int>(concurrency_hint));

    // Install socket/acceptor services.
    // These use socket_service and acceptor_service as key_type,
    // enabling runtime polymorphism.
    make_service<detail::select_socket_service>();
    make_service<detail::select_acceptor_service>();
}

select_context::
~select_context()
{
    shutdown();
    destroy();
}

} // namespace boost::corosio

#endif // !defined(_WIN32)
