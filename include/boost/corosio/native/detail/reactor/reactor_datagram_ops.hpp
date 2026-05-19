//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_DATAGRAM_OPS_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_DATAGRAM_OPS_HPP

#include <boost/corosio/native/detail/reactor/reactor_op.hpp>
#include <boost/corosio/native/detail/reactor/reactor_op_complete.hpp>

namespace boost::corosio::detail {

/* Parameterized datagram op types for reactor backends.

   @tparam Traits     Backend traits (epoll_traits, kqueue_traits, etc.)
   @tparam Socket     The concrete datagram socket type (forward-declared).
   @tparam DummyAcc   Acceptor type placeholder (datagrams have no acceptor).
   @tparam Endpoint   The endpoint type (endpoint or local_endpoint).
*/

template<class Traits, class Socket, class DummyAcc, class Endpoint>
struct reactor_dgram_base_op
    : reactor_op<Socket, DummyAcc>
{
    void operator()() override;
    void cancel() noexcept override;
};

template<class Traits, class Socket, class DummyAcc, class Endpoint>
struct reactor_dgram_connect_op final
    : reactor_connect_op<
          reactor_dgram_base_op<Traits, Socket, DummyAcc, Endpoint>,
          Endpoint>
{
    void operator()() override;
};

template<class Traits, class Socket, class DummyAcc, class Endpoint>
struct reactor_dgram_send_to_op final
    : reactor_send_to_op<
          reactor_dgram_base_op<Traits, Socket, DummyAcc, Endpoint>>
{
};

template<class Traits, class Socket, class DummyAcc, class Endpoint>
struct reactor_dgram_recv_from_op final
    : reactor_recv_from_op<
          reactor_dgram_base_op<Traits, Socket, DummyAcc, Endpoint>,
          Endpoint>
{
    void operator()() override;
};

template<class Traits, class Socket, class DummyAcc, class Endpoint>
struct reactor_dgram_send_op final
    : reactor_send_op<
          reactor_dgram_base_op<Traits, Socket, DummyAcc, Endpoint>>
{
};

template<class Traits, class Socket, class DummyAcc, class Endpoint>
struct reactor_dgram_recv_op final
    : reactor_recv_op<
          reactor_dgram_base_op<Traits, Socket, DummyAcc, Endpoint>>
{
    void operator()() override;
};

template<class Traits, class Socket, class DummyAcc, class Endpoint>
struct reactor_dgram_wait_op final
    : reactor_wait_op<
          reactor_dgram_base_op<Traits, Socket, DummyAcc, Endpoint>>
{
    void operator()() override;
};

// --- Deferred implementations ---

template<class Traits, class Socket, class DummyAcc, class Endpoint>
void
reactor_dgram_base_op<Traits, Socket, DummyAcc, Endpoint>::operator()()
{
    complete_io_op(*this);
}

template<class Traits, class Socket, class DummyAcc, class Endpoint>
void
reactor_dgram_base_op<Traits, Socket, DummyAcc, Endpoint>::cancel() noexcept
{
    if (this->socket_impl_)
        this->socket_impl_->cancel_single_op(*this);
    else
        this->request_cancel();
}

template<class Traits, class Socket, class DummyAcc, class Endpoint>
void
reactor_dgram_connect_op<Traits, Socket, DummyAcc, Endpoint>::operator()()
{
    complete_connect_op(*this);
}

template<class Traits, class Socket, class DummyAcc, class Endpoint>
void
reactor_dgram_recv_from_op<Traits, Socket, DummyAcc, Endpoint>::operator()()
{
    complete_datagram_op(*this, this->source_out);
}

template<class Traits, class Socket, class DummyAcc, class Endpoint>
void
reactor_dgram_recv_op<Traits, Socket, DummyAcc, Endpoint>::operator()()
{
    // Datagram completion: zero-length datagrams are valid, not EOF.
    complete_datagram_op(*this);
}

template<class Traits, class Socket, class DummyAcc, class Endpoint>
void
reactor_dgram_wait_op<Traits, Socket, DummyAcc, Endpoint>::operator()()
{
    complete_wait_op(*this);
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_NATIVE_DETAIL_REACTOR_REACTOR_DATAGRAM_OPS_HPP
