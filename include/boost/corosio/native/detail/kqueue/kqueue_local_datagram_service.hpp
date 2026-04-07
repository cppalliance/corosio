//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_LOCAL_DATAGRAM_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_LOCAL_DATAGRAM_SERVICE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_KQUEUE

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/local_datagram_service.hpp>

#include <boost/corosio/native/detail/kqueue/kqueue_local_datagram_socket.hpp>
#include <boost/corosio/native/detail/kqueue/kqueue_scheduler.hpp>
#include <boost/corosio/native/detail/reactor/reactor_socket_service.hpp>

#include <boost/corosio/native/detail/reactor/reactor_op_complete.hpp>

#include <coroutine>

#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace boost::corosio::detail {

class BOOST_COROSIO_DECL kqueue_local_datagram_service final
    : public reactor_socket_service<
          kqueue_local_datagram_service,
          local_datagram_service,
          kqueue_scheduler,
          kqueue_local_datagram_socket>
{
public:
    explicit kqueue_local_datagram_service(capy::execution_context& ctx)
        : reactor_socket_service(ctx)
    {
    }

    std::error_code open_socket(
        local_datagram_socket::implementation& impl,
        int family,
        int type,
        int protocol) override;

    std::error_code assign_socket(
        local_datagram_socket::implementation& impl,
        native_handle_type fd) override;

    std::error_code bind_socket(
        local_datagram_socket::implementation& impl,
        corosio::local_endpoint ep) override;
};

// Cancellation for connectionless ops

inline void
kqueue_local_send_to_op::cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

inline void
kqueue_local_recv_from_op::cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

// Cancellation for connected-mode ops

inline void
kqueue_local_dgram_connect_op::cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

inline void
kqueue_local_dgram_send_op::cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

inline void
kqueue_local_dgram_recv_op::cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

// Completion handlers

inline void
kqueue_local_datagram_op::operator()()
{
    complete_io_op(*this);
}

inline void
kqueue_local_recv_from_op::operator()()
{
    complete_datagram_op(*this, this->source_out);
}

inline void
kqueue_local_dgram_connect_op::operator()()
{
    complete_connect_op(*this);
}

inline void
kqueue_local_dgram_recv_op::operator()()
{
    // Datagram completion: do not map 0 bytes to EOF
    complete_dgram_recv_op(*this);
}

// Socket construction/destruction

inline kqueue_local_datagram_socket::kqueue_local_datagram_socket(
    kqueue_local_datagram_service& svc) noexcept
    : reactor_datagram_socket(svc)
{
}

inline kqueue_local_datagram_socket::~kqueue_local_datagram_socket() = default;

// Connectionless I/O

inline std::coroutine_handle<>
kqueue_local_datagram_socket::send_to(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    buffer_param buf,
    corosio::local_endpoint dest,
    int flags,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    return do_send_to(h, ex, buf, dest, flags, token, ec, bytes_out);
}

inline std::coroutine_handle<>
kqueue_local_datagram_socket::recv_from(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    buffer_param buf,
    corosio::local_endpoint* source,
    int flags,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    return do_recv_from(h, ex, buf, source, flags, token, ec, bytes_out);
}

// Connected-mode I/O

inline std::coroutine_handle<>
kqueue_local_datagram_socket::connect(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    corosio::local_endpoint ep,
    std::stop_token token,
    std::error_code* ec)
{
    return do_connect(h, ex, ep, token, ec);
}

inline std::coroutine_handle<>
kqueue_local_datagram_socket::send(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    buffer_param buf,
    int flags,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    return do_send(h, ex, buf, flags, token, ec, bytes_out);
}

inline std::coroutine_handle<>
kqueue_local_datagram_socket::recv(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    buffer_param buf,
    int flags,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    return do_recv(h, ex, buf, flags, token, ec, bytes_out);
}

inline void
kqueue_local_datagram_socket::cancel() noexcept
{
    do_cancel();
}

inline void
kqueue_local_datagram_socket::close_socket() noexcept
{
    do_close_socket();
}

inline native_handle_type
kqueue_local_datagram_socket::release_socket() noexcept
{
    return this->do_release_socket();
}

// Service implementations

inline std::error_code
kqueue_local_datagram_service::open_socket(
    local_datagram_socket::implementation& impl,
    int family,
    int type,
    int protocol)
{
    auto* kq_impl = static_cast<kqueue_local_datagram_socket*>(&impl);
    kq_impl->close_socket();

    int fd = ::socket(family, type, protocol);
    if (fd < 0)
        return make_err(errno);

    // Set non-blocking
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags == -1)
    {
        int errn = errno;
        ::close(fd);
        return make_err(errn);
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
    {
        int errn = errno;
        ::close(fd);
        return make_err(errn);
    }

    // Set close-on-exec
    if (::fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
    {
        int errn = errno;
        ::close(fd);
        return make_err(errn);
    }

    // Suppress SIGPIPE on this socket; sendmsg() has no MSG_NOSIGNAL
    // equivalent on macOS/FreeBSD.
#ifdef SO_NOSIGPIPE
    int one = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) != 0)
    {
        int errn = errno;
        ::close(fd);
        return make_err(errn);
    }
#endif

    kq_impl->fd_ = fd;

    kq_impl->desc_state_.fd = fd;
    {
        std::lock_guard lock(kq_impl->desc_state_.mutex);
        kq_impl->desc_state_.read_op    = nullptr;
        kq_impl->desc_state_.write_op   = nullptr;
        kq_impl->desc_state_.connect_op = nullptr;
    }
    scheduler().register_descriptor(fd, &kq_impl->desc_state_);

    return {};
}

inline std::error_code
kqueue_local_datagram_service::assign_socket(
    local_datagram_socket::implementation& impl,
    native_handle_type fd)
{
    if (fd < 0)
        return make_err(EBADF);

    auto* kq_impl = static_cast<kqueue_local_datagram_socket*>(&impl);
    kq_impl->close_socket();

    kq_impl->fd_ = fd;

    kq_impl->desc_state_.fd = fd;
    {
        std::lock_guard lock(kq_impl->desc_state_.mutex);
        kq_impl->desc_state_.read_op    = nullptr;
        kq_impl->desc_state_.write_op   = nullptr;
        kq_impl->desc_state_.connect_op = nullptr;
    }
    scheduler().register_descriptor(fd, &kq_impl->desc_state_);

    return {};
}

inline std::error_code
kqueue_local_datagram_service::bind_socket(
    local_datagram_socket::implementation& impl,
    corosio::local_endpoint ep)
{
    return static_cast<kqueue_local_datagram_socket*>(&impl)->do_bind(ep);
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_KQUEUE

#endif // BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_LOCAL_DATAGRAM_SERVICE_HPP
