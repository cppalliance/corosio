//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_LOCAL_STREAM_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_LOCAL_STREAM_SERVICE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_KQUEUE

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/local_stream_service.hpp>

#include <boost/corosio/native/detail/kqueue/kqueue_local_stream_socket.hpp>
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

class BOOST_COROSIO_DECL kqueue_local_stream_service final
    : public reactor_socket_service<
          kqueue_local_stream_service,
          local_stream_service,
          kqueue_scheduler,
          kqueue_local_stream_socket>
{
public:
    explicit kqueue_local_stream_service(capy::execution_context& ctx)
        : reactor_socket_service(ctx)
    {
    }

    std::error_code open_socket(
        local_stream_socket::implementation& impl,
        int family,
        int type,
        int protocol) override;

    std::error_code assign_socket(
        local_stream_socket::implementation& impl,
        native_handle_type fd) override;
};

// Op implementations

inline void
kqueue_local_connect_op::cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

inline void
kqueue_local_read_op::cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

inline void
kqueue_local_write_op::cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

inline void
kqueue_local_stream_op::operator()()
{
    complete_io_op(*this);
}

inline void
kqueue_local_connect_op::operator()()
{
    complete_connect_op(*this);
}

// Socket implementations

inline kqueue_local_stream_socket::kqueue_local_stream_socket(
    kqueue_local_stream_service& svc) noexcept
    : reactor_stream_socket(svc)
{
}

inline kqueue_local_stream_socket::~kqueue_local_stream_socket() = default;

inline std::coroutine_handle<>
kqueue_local_stream_socket::connect(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    corosio::local_endpoint ep,
    std::stop_token token,
    std::error_code* ec)
{
    return do_connect(h, ex, ep, token, ec);
}

inline std::coroutine_handle<>
kqueue_local_stream_socket::read_some(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    buffer_param param,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    return do_read_some(h, ex, param, token, ec, bytes_out);
}

inline std::coroutine_handle<>
kqueue_local_stream_socket::write_some(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    buffer_param param,
    std::stop_token token,
    std::error_code* ec,
    std::size_t* bytes_out)
{
    return do_write_some(h, ex, param, token, ec, bytes_out);
}

inline void
kqueue_local_stream_socket::cancel() noexcept
{
    do_cancel();
}

inline void
kqueue_local_stream_socket::close_socket() noexcept
{
    do_close_socket();
}

inline native_handle_type
kqueue_local_stream_socket::release_socket() noexcept
{
    return this->do_release_socket();
}

// Service implementations

inline std::error_code
kqueue_local_stream_service::open_socket(
    local_stream_socket::implementation& impl,
    int family,
    int type,
    int protocol)
{
    auto* kq_impl = static_cast<kqueue_local_stream_socket*>(&impl);
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

    // Suppress SIGPIPE on this socket; writev() has no MSG_NOSIGNAL
    // equivalent, so SO_NOSIGPIPE is required on macOS/FreeBSD.
    int one = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) != 0)
    {
        int errn = errno;
        ::close(fd);
        return make_err(errn);
    }

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
kqueue_local_stream_service::assign_socket(
    local_stream_socket::implementation& impl,
    native_handle_type fd)
{
    if (fd < 0)
        return make_err(EBADF);

    auto* kq_impl = static_cast<kqueue_local_stream_socket*>(&impl);
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

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_KQUEUE

#endif // BOOST_COROSIO_NATIVE_DETAIL_KQUEUE_KQUEUE_LOCAL_STREAM_SERVICE_HPP
