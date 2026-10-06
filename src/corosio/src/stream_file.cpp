//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/stream_file.hpp>
#include <boost/corosio/error.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP
#include <boost/corosio/native/detail/iocp/win_file_service.hpp>
#else
#include <boost/corosio/detail/file_service.hpp>
#endif

#include "src/detail/use_backend_service.hpp"

namespace boost::corosio {

stream_file::~stream_file()
{
    close();
}

stream_file::stream_file(capy::execution_context& ctx)
    : io_object(handle(
          ctx,
          detail::use_backend_service<
              detail::stream_file_service_of,
#if BOOST_COROSIO_HAS_IOCP
              detail::win_file_service
#else
              detail::file_service
#endif
              >(ctx)))
{
}

std::error_code
stream_file::open(
    std::filesystem::path const& path, file_base::flags mode) noexcept
{
    if (is_open())
        close();
    auto& svc = static_cast<detail::file_service&>(h_.service());
    return svc.open_file(get(), path, mode);
}

void
stream_file::close() noexcept
{
    if (!is_open())
        return;
    h_.service().close(h_);
}

void
stream_file::cancel() noexcept
{
    if (!is_open())
        return;
    get().cancel();
}

native_handle_type
stream_file::native_handle() const noexcept
{
    if (!is_open())
    {
#if BOOST_COROSIO_HAS_IOCP
        return static_cast<native_handle_type>(~0ull);
#else
        return -1;
#endif
    }
    return get().native_handle();
}

std::uint64_t
stream_file::size() const
{
    if (!is_open())
        detail::throw_system_error(
            make_error_code(std::errc::bad_file_descriptor),
            "stream_file::size");
    return get().size();
}

std::error_code
stream_file::resize(std::uint64_t new_size) noexcept
{
    if (!is_open())
        return make_error_code(std::errc::bad_file_descriptor);
    return get().resize(new_size);
}

std::error_code
stream_file::sync_data() noexcept
{
    if (!is_open())
        return make_error_code(std::errc::bad_file_descriptor);
    return get().sync_data();
}

std::error_code
stream_file::sync_all() noexcept
{
    if (!is_open())
        return make_error_code(std::errc::bad_file_descriptor);
    return get().sync_all();
}

native_handle_type
stream_file::release()
{
    if (!is_open())
        detail::throw_system_error(
            make_error_code(std::errc::bad_file_descriptor),
            "stream_file::release");
    return get().release();
}

std::error_code
stream_file::assign(native_handle_type handle) noexcept
{
    if (is_open())
        return make_error_code(error::already_open);
    return get().assign(handle);
}

capy::io_result<std::uint64_t>
stream_file::seek(std::int64_t offset, file_base::seek_basis origin) noexcept
{
    if (!is_open())
        return {make_error_code(std::errc::bad_file_descriptor), 0};
    return get().seek(offset, origin);
}

} // namespace boost::corosio
