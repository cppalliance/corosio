//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/win_object_handle.hpp>

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/win_handle_service.hpp>
#include <boost/corosio/error.hpp>

#include "src/detail/use_backend_service.hpp"

namespace boost::corosio {

win_object_handle::~win_object_handle()
{
    close();
}

win_object_handle::win_object_handle(capy::execution_context& ctx)
    : io_object(handle(
          ctx,
          detail::use_backend_service<
              detail::object_handle_service_of,
              detail::object_handle_service>(ctx)))
{
}

std::error_code
win_object_handle::assign(native_handle_type h) noexcept
{
    if (is_open())
        return make_error_code(error::already_open);
    auto& svc = static_cast<detail::object_handle_service&>(h_.service());
    return svc.assign_object_handle(get(), h);
}

native_handle_type
win_object_handle::release()
{
    if (!is_open())
        detail::throw_system_error(
            make_error_code(std::errc::bad_file_descriptor),
            "win_object_handle::release");
    return get().release_handle();
}

void
win_object_handle::close() noexcept
{
    if (!is_open())
        return;
    h_.service().close(h_);
}

native_handle_type
win_object_handle::native_handle() const noexcept
{
    if (!h_)
        return ~native_handle_type{};
    return get().native_handle();
}

void
win_object_handle::cancel() noexcept
{
    if (!is_open())
        return;
    get().cancel();
}

} // namespace boost::corosio

#endif // BOOST_COROSIO_HAS_IOCP
