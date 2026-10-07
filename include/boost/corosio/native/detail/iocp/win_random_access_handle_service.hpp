//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_RANDOM_ACCESS_HANDLE_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_RANDOM_ACCESS_HANDLE_SERVICE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/object_pool.hpp>
#include <boost/corosio/detail/object_ref.hpp>
#include <boost/corosio/detail/win_handle_service.hpp>
#include <boost/corosio/native/detail/iocp/win_overlapped_handle.hpp>

namespace boost::corosio::detail {

class win_random_access_handle_service;

/** IOCP implementation of @ref win_random_access_handle: any number of positional operations. */
class win_random_access_handle_impl final
    : public win_random_access_handle::implementation
    , public intrusive_list<win_random_access_handle_impl>::node
{
    win_random_access_handle_service& svc_;
    win_concurrent_handle internal_;

public:
    win_random_access_handle_impl(
        win_random_access_handle_service& svc, win_scheduler& sched) noexcept
        : svc_(svc)
        , internal_(sched, *this)
    {
    }

    /// Recycle into the owning service's pool. Defined out-of-line
    /// after the service is complete.
    void retire() noexcept override;

    /** Assert the closed state a recycled handle starts from.

        @pre refs_ == 0, handle closed, no op in flight.
    */
    void reuse() noexcept
    {
        BOOST_COROSIO_ASSERT(!internal_.is_open());
    }

    win_concurrent_handle* get_internal() noexcept
    {
        return &internal_;
    }

    std::coroutine_handle<> read_some_at(
        std::uint64_t offset,
        capy::continuation& cont,
        capy::executor_ref ex,
        buffer_param buf,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes) override
    {
        return internal_.read_some_at(
            offset, cont, ex, buf, std::move(token), ec, bytes);
    }

    std::coroutine_handle<> write_some_at(
        std::uint64_t offset,
        capy::continuation& cont,
        capy::executor_ref ex,
        buffer_param buf,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes) override
    {
        return internal_.write_some_at(
            offset, cont, ex, buf, std::move(token), ec, bytes);
    }

    native_handle_type native_handle() const noexcept override
    {
        return reinterpret_cast<native_handle_type>(internal_.native_handle());
    }

    native_handle_type release_handle() override
    {
        return internal_.release();
    }

    void cancel() noexcept override
    {
        internal_.cancel();
    }
};

/** IOCP service that owns the @ref win_random_access_handle implementations. */
class BOOST_COROSIO_DECL win_random_access_handle_service final
    : public random_access_handle_service
{
    friend class win_random_access_handle_impl;

public:
    explicit win_random_access_handle_service(capy::execution_context& ctx)
        : sched_(ctx.use_service<win_scheduler>())
    {
    }

    io_object::implementation* construct() override
    {
        return pool_.acquire(*this, sched_);
    }

    void destroy(io_object::implementation* p) override
    {
        if (!p)
            return;
        auto* impl = static_cast<win_random_access_handle_impl*>(p);
        impl->get_internal()->close_handle();
        release(impl);
    }

    void close(io_object::handle& h) override
    {
        static_cast<win_random_access_handle_impl&>(*h.get())
            .get_internal()
            ->close_handle();
    }

    void shutdown() override
    {
        pool_.shutdown(
            [](win_random_access_handle_impl* impl) { impl->get_internal()->close_handle(); });
    }

    std::error_code assign_random_access_handle(
        win_random_access_handle::implementation& impl,
        native_handle_type h) override
    {
        return static_cast<win_random_access_handle_impl&>(impl)
            .get_internal()
            ->assign(h, handle_kind::random_access_handle);
    }

private:
    win_scheduler& sched_;
    BOOST_COROSIO_MSVC_WARNING_PUSH
    BOOST_COROSIO_MSVC_WARNING_DISABLE(4251) // detail:: members, dll-interface
    object_pool<win_random_access_handle_impl> pool_;
    BOOST_COROSIO_MSVC_WARNING_POP
};

inline void
win_random_access_handle_impl::retire() noexcept
{
    svc_.pool_.recycle(this);
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif
