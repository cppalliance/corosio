//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/stream_file.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/platform.hpp>
#include <system_error>

#if BOOST_COROSIO_HAS_IOCP
#include <boost/corosio/native/detail/iocp/win_file_service.hpp>
#else
#include <boost/corosio/detail/file_service.hpp>
#endif

namespace boost::corosio {

stream_file::~stream_file()
{
    close();
}

stream_file::stream_file(capy::execution_context& ctx)
#if BOOST_COROSIO_HAS_IOCP
    : io_object(create_handle<detail::win_file_service>(ctx))
#else
    : io_object(create_handle<detail::file_service>(ctx))
#endif
{
}

void
stream_file::open(std::filesystem::path const& path, file_base::flags mode)
{
    std::error_code ec;
    open(path, mode, ec);
    if (ec)
    {
        detail::throw_system_error(ec, "stream_file::open");
    }
}

void
stream_file::open(
    std::filesystem::path const& path,
    file_base::flags mode,
    std::error_code& ec)
{
    ec.clear();
    if (is_open())
        close();
    auto& svc = static_cast<detail::file_service&>(h_.service());
    ec        = svc.open_file(get(), path, mode);
}

void
stream_file::close()
{
    if (!is_open())
        return;
    h_.service().close(h_);
}

void
stream_file::cancel()
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
    std::error_code ec;
    std::uint64_t sz = size(ec);
    if (ec)
    {
        detail::throw_system_error(ec, "stream_file::size");
    }
    return sz;
}

std::uint64_t
stream_file::size(std::error_code& ec) const
{
    ec.clear();
    if (!is_open())
        ec = make_error_code(std::errc::bad_file_descriptor);
    return get().size();
}

void
stream_file::resize(std::uint64_t new_size)
{
    std::error_code ec;
    resize(new_size, ec);
    if (ec)
    {
        detail::throw_system_error(ec, "stream_file::resize");
    }
}

void
stream_file::resize(std::uint64_t new_size, std::error_code& ec)
{
    ec.clear();
    if (!is_open())
        ec = make_error_code(std::errc::bad_file_descriptor);
    get().resize(new_size);
}

void
stream_file::sync_data()
{
    std::error_code ec;
    sync_data(ec);
    if (ec)
    {
        detail::throw_system_error(ec, "stream_file::sync_data");
    }
}

void
stream_file::sync_data(std::error_code& ec)
{
    if (!is_open())
        ec = make_error_code(std::errc::bad_file_descriptor);
    get().sync_data();
}

void
stream_file::sync_all()
{
    std::error_code ec;
    sync_all(ec);
    if (ec)
    {
        detail::throw_system_error(ec, "stream_file::sync_all");
    }
}

void
stream_file::sync_all(std::error_code& ec)
{
    ec.clear();
    if (!is_open())
        ec = make_error_code(std::errc::bad_file_descriptor);
    get().sync_all();
}

native_handle_type
stream_file::release()
{
    std::error_code ec;
    native_handle_type h = release(ec);
    if (ec)
    {
        detail::throw_system_error(ec, "stream_file::release");
    }
    return h;
}
native_handle_type
stream_file::release(std::error_code& ec)
{
    ec.clear();
    if (!is_open())
        ec = make_error_code(std::errc::bad_file_descriptor);
    return get().release();
}

void
stream_file::assign(native_handle_type handle)
{
    if (is_open())
        close();
    get().assign(handle);
}

std::uint64_t
stream_file::seek(std::int64_t offset, file_base::seek_basis origin)
{
    std::error_code ec;
    std::uint64_t pos = seek(offset, origin, ec);
    if (ec)
    {
        detail::throw_system_error(ec, "stream_file::seek");
    }
    return pos;
}

std::uint64_t
stream_file::seek(
    std::int64_t offset, file_base::seek_basis origin, std::error_code& ec)
{
    ec.clear();
    if (!is_open())
        ec = make_error_code(std::errc::bad_file_descriptor);
    return get().seek(offset, origin);
}

} // namespace boost::corosio
