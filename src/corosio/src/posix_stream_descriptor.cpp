//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/posix_stream_descriptor.hpp>
#include <boost/corosio/error.hpp>

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_POSIX

#include <boost/corosio/detail/descriptor_service.hpp>
#include <boost/corosio/detail/except.hpp>

#include "src/detail/use_backend_service.hpp"

namespace boost::corosio {

posix_stream_descriptor::~posix_stream_descriptor()
{
    close();
}

posix_stream_descriptor::posix_stream_descriptor(capy::execution_context& ctx)
    : io_object(handle(
          ctx,
          detail::use_backend_service<
              detail::descriptor_service_of,
              detail::descriptor_service>(ctx)))
{
}

std::error_code
posix_stream_descriptor::assign(native_handle_type fd) noexcept
{
    if (is_open())
        return make_error_code(error::already_open);
    auto& svc = static_cast<detail::descriptor_service&>(h_.service());
    return svc.assign_descriptor(get(), fd);
}

native_handle_type
posix_stream_descriptor::release()
{
    if (!is_open())
        detail::throw_system_error(
            make_error_code(std::errc::bad_file_descriptor),
            "posix_stream_descriptor::release");
    return get().release_descriptor();
}

void
posix_stream_descriptor::close() noexcept
{
    if (!is_open())
        return;
    h_.service().close(h_);
}

native_handle_type
posix_stream_descriptor::native_handle() const noexcept
{
    if (!h_)
        return -1;
    return get().native_handle();
}

void
posix_stream_descriptor::cancel() noexcept
{
    if (!is_open())
        return;
    get().cancel();
}

} // namespace boost::corosio

#endif // BOOST_COROSIO_POSIX
