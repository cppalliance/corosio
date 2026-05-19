//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_STREAM_OPS_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_STREAM_OPS_HPP

#include <boost/corosio/native/detail/reactor/reactor_op.hpp>
#include <boost/corosio/native/detail/reactor/reactor_op_complete.hpp>

namespace boost::corosio::detail {

/* Parameterized stream op types for reactor backends.

   Given a Traits type (providing write_policy, accept_policy) and
   forward-declared Socket/Acceptor types, generates all the concrete
   op types needed for a stream socket. The cancel() and operator()()
   bodies reference Socket/Acceptor but are only instantiated when the
   vtable of a derived type is emitted -- at which point those types
   are complete.

   @tparam Traits     Backend traits (epoll_traits, kqueue_traits, etc.)
   @tparam Socket     The concrete stream socket type (forward-declared).
   @tparam Acceptor   The concrete stream acceptor type (forward-declared).
   @tparam Endpoint   The endpoint type (endpoint or local_endpoint).
*/

template<class Traits, class Socket, class Acceptor, class Endpoint>
struct reactor_stream_base_op
    : reactor_op<Socket, Acceptor>
{
    void operator()() override;
    void cancel() noexcept override;
};

template<class Traits, class Socket, class Acceptor, class Endpoint>
struct reactor_stream_connect_op final
    : reactor_connect_op<
          reactor_stream_base_op<Traits, Socket, Acceptor, Endpoint>,
          Endpoint>
{
    void operator()() override;
};

template<class Traits, class Socket, class Acceptor, class Endpoint>
struct reactor_stream_read_op final
    : reactor_read_op<
          reactor_stream_base_op<Traits, Socket, Acceptor, Endpoint>>
{
};

template<class Traits, class Socket, class Acceptor, class Endpoint>
struct reactor_stream_write_op final
    : reactor_write_op<
          reactor_stream_base_op<Traits, Socket, Acceptor, Endpoint>,
          typename Traits::write_policy>
{
};

template<class Traits, class Socket, class Acceptor, class Endpoint>
struct reactor_stream_accept_op final
    : reactor_accept_op<
          reactor_stream_base_op<Traits, Socket, Acceptor, Endpoint>,
          typename Traits::accept_policy>
{
    void operator()() override;
};

template<class Traits, class Socket, class Acceptor, class Endpoint>
struct reactor_stream_wait_op final
    : reactor_wait_op<
          reactor_stream_base_op<Traits, Socket, Acceptor, Endpoint>>
{
    void operator()() override;
};

// --- Deferred implementations (instantiated when Socket/Acceptor are complete) ---

template<class Traits, class Socket, class Acceptor, class Endpoint>
void
reactor_stream_base_op<Traits, Socket, Acceptor, Endpoint>::operator()()
{
    complete_io_op(*this);
}

template<class Traits, class Socket, class Acceptor, class Endpoint>
void
reactor_stream_base_op<Traits, Socket, Acceptor, Endpoint>::cancel() noexcept
{
    if (this->socket_impl_)
        this->socket_impl_->cancel_single_op(*this);
    else if (this->acceptor_impl_)
        this->acceptor_impl_->cancel_single_op(*this);
    else
        this->request_cancel();
}

template<class Traits, class Socket, class Acceptor, class Endpoint>
void
reactor_stream_connect_op<Traits, Socket, Acceptor, Endpoint>::operator()()
{
    complete_connect_op(*this);
}

template<class Traits, class Socket, class Acceptor, class Endpoint>
void
reactor_stream_accept_op<Traits, Socket, Acceptor, Endpoint>::operator()()
{
    complete_accept_op<Socket>(*this);
}

template<class Traits, class Socket, class Acceptor, class Endpoint>
void
reactor_stream_wait_op<Traits, Socket, Acceptor, Endpoint>::operator()()
{
    complete_wait_op(*this);
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_STREAM_OPS_HPP
