//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_IO_OBJECT_HPP
#define BOOST_COROSIO_IO_OBJECT_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/capy/ex/execution_context.hpp>

namespace boost::corosio {

/** Base class for I/O objects in the library hierarchy.

    This class provides a common base for all I/O object implementations
    in the library. It holds the implementation pointer (`impl_`) which
    provides a unified interface for all derived classes in the hierarchy.

    By using a single pointer to a polymorphic base (`impl_base`), all
    classes in the I/O object hierarchy can leverage type erasure to
    share common implementation patterns while maintaining type safety
    through the virtual interface.

    @par Semantics
    Derived classes wrap direct platform I/O: OS sockets, timers,
    signal handlers, acceptors. Operations dispatch through a
    platform-specific implementation vtable (epoll, IOCP, kqueue,
    io_uring).

    Test mocks, decorators, and stream adapters must not inherit
    from io_object. Use concepts or templates for generic I/O
    algorithms.

    @note This class is intended for use as a protected base class.
        The implementation pointer is accessible to derived classes
        through the protected member `impl_`.
*/
class BOOST_COROSIO_DECL io_object
{
public:
    struct implementation;

    class handle;

    struct io_service
    {
        virtual void open(handle&) = 0;
        virtual void close(handle&) = 0;
        virtual void destroy(implementation*) = 0;
        virtual implementation* construct() = 0;
    };

    class handle
    {
        capy::execution_context* ctx_ = nullptr;
        io_service* svc_ = nullptr;
        implementation* impl_ = nullptr;

    public:
        ~handle()
        {
            if(impl_)
                svc_->destroy(impl_);
        }
        handle() = default;
        handle(
            capy::execution_context& ctx,
            io_service& svc)
            : ctx_(&ctx)
            , svc_(&svc)
            , impl_(svc_->construct())
        {
        }
        handle(handle&& other)
            : ctx_(std::exchange(other.ctx_, nullptr))
            , svc_(std::exchange(other.svc_, nullptr))
            , impl_(std::exchange(other.impl_, nullptr))
        {
        }
        handle& operator=(handle&& other) noexcept
        {
            ctx_ = std::exchange(other.ctx_, nullptr);
            svc_ = std::exchange(other.svc_, nullptr);
            impl_ = std::exchange(other.impl_, nullptr);
            return *this;
        }
        capy::execution_context& context() const noexcept
        {
            return *ctx_;
        }
        io_service& service() const noexcept
        {
            return *svc_;
        }
        implementation& get() const noexcept
        {
            return *impl_;
        }
    };

    //------

    struct io_object_impl
    {
        virtual ~io_object_impl() = default;

        virtual void release() = 0;
    };

    /** Return the execution context.

        @return Reference to the execution context that owns this socket.
    */
    auto
    context() const noexcept ->
        capy::execution_context&
    {
        return *ctx_;
    }

protected:
    virtual ~io_object() = default;

    explicit
    io_object(
        capy::execution_context& ctx) noexcept
        : ctx_(&ctx)
    {
    }

    capy::execution_context* ctx_ = nullptr;
    io_object_impl* impl_ = nullptr;
};

} // namespace boost::corosio

#endif
