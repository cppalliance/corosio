//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_STREAM_HANDLE_SERVICE_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_STREAM_HANDLE_SERVICE_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/object_pool.hpp>
#include <boost/corosio/detail/object_ref.hpp>
#include <boost/corosio/detail/win_handle_service.hpp>
#include <boost/corosio/native/detail/iocp/win_overlapped_handle.hpp>

namespace boost::corosio::detail {

class win_stream_handle_service;

/** IOCP implementation of @ref win_stream_handle: the slot model at offset 0. */
class win_stream_handle_impl final
    : public win_stream_handle::implementation
    , public intrusive_list<win_stream_handle_impl>::node
{
    win_stream_handle_service& svc_;
    win_slot_handle internal_;

public:
    win_stream_handle_impl(
        win_stream_handle_service& svc, win_scheduler& sched) noexcept
        : svc_(svc)
        , internal_(sched, *this, /*track_offset=*/false)
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

    win_slot_handle* get_internal() noexcept
    {
        return &internal_;
    }

    std::coroutine_handle<> read_some(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        buffer_param buf,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes) override
    {
        return internal_.read_some(h, ex, buf, std::move(token), ec, bytes);
    }

    std::coroutine_handle<> write_some(
        std::coroutine_handle<> h,
        capy::executor_ref ex,
        buffer_param buf,
        std::stop_token token,
        std::error_code* ec,
        std::size_t* bytes) override
    {
        return internal_.write_some(h, ex, buf, std::move(token), ec, bytes);
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

/** IOCP service that owns the @ref win_stream_handle implementations. */
class BOOST_COROSIO_DECL win_stream_handle_service final
    : public stream_handle_service
{
    friend class win_stream_handle_impl;

public:
    explicit win_stream_handle_service(capy::execution_context& ctx)
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
        auto* impl = static_cast<win_stream_handle_impl*>(p);
        impl->get_internal()->close_handle();
        release(impl);
    }

    void close(io_object::handle& h) override
    {
        static_cast<win_stream_handle_impl&>(*h.get())
            .get_internal()
            ->close_handle();
    }

    void shutdown() override
    {
        pool_.shutdown(
            [](win_stream_handle_impl* impl) { impl->get_internal()->close_handle(); });
    }

    std::error_code assign_stream_handle(
        win_stream_handle::implementation& impl,
        native_handle_type h) override
    {
        return static_cast<win_stream_handle_impl&>(impl)
            .get_internal()
            ->assign(h, handle_kind::stream_handle);
    }

private:
    win_scheduler& sched_;
    BOOST_COROSIO_MSVC_WARNING_PUSH
    BOOST_COROSIO_MSVC_WARNING_DISABLE(4251) // detail:: members, dll-interface
    object_pool<win_stream_handle_impl> pool_;
    BOOST_COROSIO_MSVC_WARNING_POP
};

inline void
win_stream_handle_impl::retire() noexcept
{
    svc_.pool_.recycle(this);
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif
