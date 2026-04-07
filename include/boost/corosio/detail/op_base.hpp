//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_OP_BASE_HPP
#define BOOST_COROSIO_DETAIL_OP_BASE_HPP

#include <boost/capy/io_result.hpp>
#include <boost/capy/ex/executor_ref.hpp>
#include <boost/capy/ex/io_env.hpp>

#include <coroutine>
#include <cstddef>
#include <stop_token>
#include <system_error>

namespace boost::corosio::detail {

/* CRTP base for awaitables that return io_result<std::size_t>.

   Derived classes must provide:

     std::coroutine_handle<> dispatch(
         std::coroutine_handle<> h,
         capy::executor_ref ex) const;

   which forwards to the backend implementation method, passing
   token_, &ec_, and &bytes_ as the cancellation/output parameters.
*/
template<class Derived>
class bytes_op_base
{
    friend Derived;
    bytes_op_base() = default;

public:
    std::stop_token token_;
    mutable std::error_code ec_;
    mutable std::size_t bytes_ = 0;

    bool await_ready() const noexcept
    {
        return token_.stop_requested();
    }

    capy::io_result<std::size_t> await_resume() const noexcept
    {
        if (token_.stop_requested())
            return {make_error_code(std::errc::operation_canceled), 0};
        return {ec_, bytes_};
    }

    auto await_suspend(std::coroutine_handle<> h, capy::io_env const* env)
        -> std::coroutine_handle<>
    {
        token_ = env->stop_token;
        return static_cast<Derived const*>(this)->dispatch(
            h, env->executor);
    }
};

/* CRTP base for awaitables that return io_result<>.

   Derived classes must provide:

     std::coroutine_handle<> dispatch(
         std::coroutine_handle<> h,
         capy::executor_ref ex) const;

   which forwards to the backend implementation method, passing
   token_ and &ec_ as the cancellation/output parameters.
*/
template<class Derived>
class void_op_base
{
    friend Derived;
    void_op_base() = default;

public:
    std::stop_token token_;
    mutable std::error_code ec_;

    bool await_ready() const noexcept
    {
        return token_.stop_requested();
    }

    capy::io_result<> await_resume() const noexcept
    {
        if (token_.stop_requested())
            return {make_error_code(std::errc::operation_canceled)};
        return {ec_};
    }

    auto await_suspend(std::coroutine_handle<> h, capy::io_env const* env)
        -> std::coroutine_handle<>
    {
        token_ = env->stop_token;
        return static_cast<Derived const*>(this)->dispatch(
            h, env->executor);
    }
};

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_DETAIL_OP_BASE_HPP
