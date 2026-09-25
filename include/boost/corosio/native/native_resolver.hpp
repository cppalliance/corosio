//
// Copyright (c) 2026 Steve Gerbino
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_NATIVE_RESOLVER_HPP
#define BOOST_COROSIO_NATIVE_NATIVE_RESOLVER_HPP

#include <boost/corosio/resolver.hpp>
#include <boost/corosio/backend.hpp>
#include <boost/corosio/detail/op_base.hpp>

#ifndef BOOST_COROSIO_MRDOCS
#if BOOST_COROSIO_HAS_EPOLL || BOOST_COROSIO_HAS_SELECT || \
    BOOST_COROSIO_HAS_KQUEUE
#include <boost/corosio/native/detail/posix/posix_resolver_service.hpp>
#endif

#if BOOST_COROSIO_HAS_IOCP
#include <boost/corosio/native/detail/iocp/win_resolver_service.hpp>
#endif
#endif // !BOOST_COROSIO_MRDOCS

namespace boost::corosio {

/** Resolves host names to endpoints, calling the backend directly.

    This class template inherits from @ref resolver. It shadows the
    `resolve` operations with versions that call the backend
    implementation directly. The compiler can then inline through the
    entire call chain.

    Non-async operations (`cancel`) remain unchanged and dispatch
    through the compiled library.

    A `native_resolver` IS-A `resolver` and can be passed to any
    function expecting `resolver&`.

    @tparam Backend A backend tag value (e.g., `epoll`).

    @par Thread Safety
    Same as @ref resolver.

    @see resolver, epoll_t, iocp_t
*/
template<auto Backend>
class native_resolver : public resolver
{
    using backend_type = decltype(Backend);
    using impl_type    = typename backend_type::resolver_type;

    impl_type& get_impl() noexcept
    {
        return *static_cast<impl_type*>(h_.get());
    }

    struct native_resolve_awaitable
        : detail::value_op_base<native_resolve_awaitable, std::vector<endpoint>>
    {
        native_resolver& self_;
        std::string host_;
        std::string service_;
        resolve_flags flags_;

        native_resolve_awaitable(
            native_resolver& self,
            std::string_view host,
            std::string_view service,
            resolve_flags flags) noexcept
            : self_(self)
            , host_(host)
            , service_(service)
            , flags_(flags)
        {
        }

        std::coroutine_handle<>
        dispatch(std::coroutine_handle<> h, capy::executor_ref ex) const
        {
            return self_.get_impl().resolve(
                h, ex, host_, service_, flags_, this->token_, &this->ec_,
                &this->value_);
        }
    };

    struct native_reverse_awaitable
        : detail::value_op_base<native_reverse_awaitable, endpoint_name>
    {
        native_resolver& self_;
        endpoint ep_;
        reverse_flags flags_;

        native_reverse_awaitable(
            native_resolver& self,
            endpoint const& ep,
            reverse_flags flags) noexcept
            : self_(self)
            , ep_(ep)
            , flags_(flags)
        {
        }

        std::coroutine_handle<>
        dispatch(std::coroutine_handle<> h, capy::executor_ref ex) const
        {
            return self_.get_impl().reverse_resolve(
                h, ex, ep_, flags_, this->token_, &this->ec_, &this->value_);
        }
    };

public:
    /** Construct a native resolver from an execution context.

        @param ctx The execution context that owns this resolver.
    */
    explicit native_resolver(capy::execution_context& ctx) : resolver(ctx) {}

    /** Construct a native resolver from an executor.

        @param ex The executor whose context owns the resolver.
    */
    template<class Ex>
        requires(!std::same_as<std::remove_cvref_t<Ex>, native_resolver>) &&
        capy::Executor<Ex>
    explicit native_resolver(Ex const& ex) : native_resolver(ex.context())
    {
    }

    /** Move construct.

        @pre No awaitables returned by the source's `resolve` methods
            exist.
        @pre The execution context associated with the source must
            outlive this resolver.
    */
    native_resolver(native_resolver&&) noexcept = default;

    /** Move assign.

        @return Reference to this resolver.

        @pre No awaitables returned by either `*this` or the source's
            `resolve` methods exist.
        @pre The execution context associated with the source must
            outlive this resolver.
    */
    native_resolver& operator=(native_resolver&&) noexcept = default;

    /// Copy construction is disabled; the handle is uniquely owned.
    native_resolver(native_resolver const&) = delete;
    /// Copy assignment is disabled; the handle is uniquely owned.
    native_resolver& operator=(native_resolver const&) = delete;

    /** Asynchronously resolve a host and service to endpoints.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to @ref resolver::resolve.

        This resolver must outlive the returned awaitable.

        @param host The host name or address string.
        @param service The service name or port string.

        @return An awaitable yielding `io_result<std::vector<endpoint>>`.
    */
    [[nodiscard]] auto resolve(std::string_view host, std::string_view service)
    {
        return native_resolve_awaitable(
            *this, host, service, resolve_flags::none);
    }

    /** Asynchronously resolve a host and service with flags.

        This resolver must outlive the returned awaitable.

        @param host The host name or address string.
        @param service The service name or port string.
        @param flags Flags controlling resolution behavior.

        @return An awaitable yielding
            `io_result<std::vector<endpoint>>`.
    */
    [[nodiscard]] auto resolve(
        std::string_view host, std::string_view service, resolve_flags flags)
    {
        return native_resolve_awaitable(*this, host, service, flags);
    }

    /** Asynchronously reverse-resolve an endpoint.

        Calls the backend implementation directly, bypassing virtual
        dispatch. Otherwise identical to the endpoint overload of
        @ref resolver::resolve.

        This resolver must outlive the returned awaitable.

        @param ep The endpoint to resolve.

        @return An awaitable yielding
            `io_result<endpoint_name>`.
    */
    [[nodiscard]] auto resolve(endpoint const& ep)
    {
        return native_reverse_awaitable(*this, ep, reverse_flags::none);
    }

    /** Asynchronously reverse-resolve an endpoint with flags.

        This resolver must outlive the returned awaitable.

        @param ep The endpoint to resolve.
        @param flags Flags controlling resolution behavior.

        @return An awaitable yielding
            `io_result<endpoint_name>`.
    */
    [[nodiscard]] auto resolve(endpoint const& ep, reverse_flags flags)
    {
        return native_reverse_awaitable(*this, ep, flags);
    }
};

} // namespace boost::corosio

#endif
