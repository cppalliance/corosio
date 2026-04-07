//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_LOCAL_STREAM_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_LOCAL_STREAM_SERVICE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_EPOLL

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/local_stream_service.hpp>

#include <boost/corosio/native/detail/epoll/epoll_local_stream_socket.hpp>
#include <boost/corosio/native/detail/epoll/epoll_scheduler.hpp>
#include <boost/corosio/native/detail/reactor/reactor_socket_service.hpp>

#include <boost/corosio/native/detail/reactor/reactor_op_complete.hpp>

#include <coroutine>

#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace boost::corosio::detail {

class BOOST_COROSIO_DECL epoll_local_stream_service final
    : public reactor_socket_service<
          epoll_local_stream_service,
          local_stream_service,
          epoll_scheduler,
          epoll_local_stream_socket>
{
public:
    explicit epoll_local_stream_service(capy::execution_context& ctx)
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
epoll_local_connect_op::cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

inline void
epoll_local_read_op::cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

inline void
epoll_local_write_op::cancel() noexcept
{
    if (socket_impl_)
        socket_impl_->cancel_single_op(*this);
    else
        request_cancel();
}

inline void
epoll_local_stream_op::operator()()
{
    complete_io_op(*this);
}

inline void
epoll_local_connect_op::operator()()
{
    complete_connect_op(*this);
}

// Socket implementations

inline epoll_local_stream_socket::epoll_local_stream_socket(
    epoll_local_stream_service& svc) noexcept
    : reactor_stream_socket(svc)
{
}

inline epoll_local_stream_socket::~epoll_local_stream_socket() = default;

inline std::coroutine_handle<>
epoll_local_stream_socket::connect(
    std::coroutine_handle<> h,
    capy::executor_ref ex,
    corosio::local_endpoint ep,
    std::stop_token token,
    std::error_code* ec)
{
    return do_connect(h, ex, ep, token, ec);
}

inline std::coroutine_handle<>
epoll_local_stream_socket::read_some(
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
epoll_local_stream_socket::write_some(
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
epoll_local_stream_socket::cancel() noexcept
{
    do_cancel();
}

inline void
epoll_local_stream_socket::close_socket() noexcept
{
    do_close_socket();
}

inline native_handle_type
epoll_local_stream_socket::release_socket() noexcept
{
    return this->do_release_socket();
}

// Service implementations

inline std::error_code
epoll_local_stream_service::open_socket(
    local_stream_socket::implementation& impl,
    int family,
    int type,
    int protocol)
{
    auto* epoll_impl = static_cast<epoll_local_stream_socket*>(&impl);
    epoll_impl->close_socket();

    int fd = ::socket(family, type | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
    if (fd < 0)
        return make_err(errno);

    epoll_impl->fd_ = fd;

    epoll_impl->desc_state_.fd = fd;
    {
        std::lock_guard lock(epoll_impl->desc_state_.mutex);
        epoll_impl->desc_state_.read_op    = nullptr;
        epoll_impl->desc_state_.write_op   = nullptr;
        epoll_impl->desc_state_.connect_op = nullptr;
    }
    scheduler().register_descriptor(fd, &epoll_impl->desc_state_);

    return {};
}

inline std::error_code
epoll_local_stream_service::assign_socket(
    local_stream_socket::implementation& impl,
    native_handle_type fd)
{
    if (fd < 0)
        return make_err(EBADF);

    auto* epoll_impl = static_cast<epoll_local_stream_socket*>(&impl);
    epoll_impl->close_socket();

    epoll_impl->fd_ = fd;

    epoll_impl->desc_state_.fd = fd;
    {
        std::lock_guard lock(epoll_impl->desc_state_.mutex);
        epoll_impl->desc_state_.read_op    = nullptr;
        epoll_impl->desc_state_.write_op   = nullptr;
        epoll_impl->desc_state_.connect_op = nullptr;
    }
    scheduler().register_descriptor(fd, &epoll_impl->desc_state_);

    return {};
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_EPOLL

#endif // BOOST_COROSIO_NATIVE_DETAIL_EPOLL_EPOLL_LOCAL_STREAM_SERVICE_HPP
