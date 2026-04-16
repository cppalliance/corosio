//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_BACKEND_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_BACKEND_HPP

/* Reactor backend: acceptor accept() implementation.

   Contains the accept() method body for reactor_acceptor_impl,
   which needs all socket/service types to be complete. Included
   by per-backend type files (epoll_types.hpp, etc.) after all
   named types are defined.
*/

#include <boost/corosio/native/detail/reactor/reactor_service_finals.hpp>
#include <boost/corosio/native/detail/reactor/reactor_op_complete.hpp>
#include <boost/corosio/native/detail/endpoint_convert.hpp>
#include <boost/corosio/detail/dispatch_coro.hpp>

#include <mutex>

namespace boost::corosio::detail {

// ============================================================
// Acceptor accept() implementation
// ============================================================

template<class Derived, class Traits, class Service,
         class SocketFinal, class AccImplBase, class Endpoint>
std::coroutine_handle<>
reactor_acceptor_impl<Derived, Traits, Service, SocketFinal, AccImplBase, Endpoint>::accept(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    std::stop_token token,
    std::error_code* ec,
    io_object::implementation** impl_out)
{
    auto& op = this->acc_;
    op.reset();
    op.h        = h;
    op.ex       = ex;
    op.ec_out   = ec;
    op.impl_out = impl_out;
    op.fd       = this->fd_;
    op.start(token, static_cast<Derived*>(this));

    sockaddr_storage peer_storage{};
    socklen_t peer_addrlen = 0;

    int accepted = Traits::accept_policy::do_accept(
        this->fd_, peer_storage, peer_addrlen);

    if (accepted >= 0)
    {
        {
            std::lock_guard lock(this->desc_state_.mutex);
            this->desc_state_.read_ready = false;
        }

        if (this->svc_.scheduler().try_consume_inline_budget())
        {
            auto* socket_svc = this->svc_.stream_service();
            if (socket_svc)
            {
                auto& impl =
                    static_cast<SocketFinal&>(*socket_svc->construct());
                impl.set_socket(accepted);

                impl.desc_state_.fd = accepted;
                {
                    std::lock_guard lock(impl.desc_state_.mutex);
                    impl.desc_state_.read_op    = nullptr;
                    impl.desc_state_.write_op   = nullptr;
                    impl.desc_state_.connect_op = nullptr;
                }
                socket_svc->scheduler().register_descriptor(
                    accepted, &impl.desc_state_);

                impl.set_endpoints(
                    this->local_endpoint_,
                    from_sockaddr_as(
                        peer_storage, peer_addrlen, Endpoint{}));

                *ec = {};
                if (impl_out)
                    *impl_out = &impl;
            }
            else
            {
                ::close(accepted);
                *ec = make_err(ENOENT);
                if (impl_out)
                    *impl_out = nullptr;
            }
            op.cont_op.cont.h = h;
            return dispatch_coro(ex, op.cont_op.cont);
        }

        op.accepted_fd  = accepted;
        op.peer_storage = peer_storage;
        op.peer_addrlen = peer_addrlen;
        op.complete(0, 0);
        op.impl_ptr = this->shared_from_this();
        this->svc_.post(&op);
        return std::noop_coroutine();
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK)
    {
        op.impl_ptr = this->shared_from_this();
        this->svc_.work_started();

        std::lock_guard lock(this->desc_state_.mutex);
        bool io_done = false;
        if (this->desc_state_.read_ready)
        {
            this->desc_state_.read_ready = false;
            op.perform_io();
            io_done = (op.errn != EAGAIN && op.errn != EWOULDBLOCK);
            if (!io_done)
                op.errn = 0;
        }

        if (io_done || op.cancelled.load(std::memory_order_acquire))
        {
            this->svc_.post(&op);
            this->svc_.work_finished();
        }
        else
        {
            this->desc_state_.read_op = &op;
        }
        return std::noop_coroutine();
    }

    op.complete(errno, 0);
    op.impl_ptr = this->shared_from_this();
    this->svc_.post(&op);
    return std::noop_coroutine();
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_BACKEND_HPP
