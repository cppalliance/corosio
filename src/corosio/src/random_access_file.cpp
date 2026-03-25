//
// Copyright (c) 2026 Michael Vandeberg
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include <boost/corosio/random_access_file.hpp>
#include <boost/corosio/detail/except.hpp>
#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP
#include <boost/corosio/native/detail/iocp/win_random_access_file_service.hpp>
#else
#include <boost/corosio/detail/random_access_file_service.hpp>
#endif

namespace boost::corosio {

random_access_file::~random_access_file()
{
    close();
}

random_access_file::random_access_file(capy::execution_context& ctx)
#if BOOST_COROSIO_HAS_IOCP
    : io_object(create_handle<detail::win_random_access_file_service>(ctx))
#else
    : io_object(create_handle<detail::random_access_file_service>(ctx))
#endif
{
}

void
random_access_file::open(
    std::filesystem::path const& path, file_base::flags mode)
{
    if (is_open())
        close();
    auto& svc = static_cast<detail::random_access_file_service&>(h_.service());
    std::error_code ec = svc.open_file(get(), path, mode);
    if (ec)
        detail::throw_system_error(ec, "random_access_file::open");
}

void
random_access_file::close()
{
    if (!is_open())
        return;
    h_.service().close(h_);
}

void
random_access_file::cancel()
{
    if (!is_open())
        return;
    get().cancel();
}

native_handle_type
random_access_file::native_handle() const noexcept
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
random_access_file::size() const
{
    if (!is_open())
        detail::throw_system_error(
            make_error_code(std::errc::bad_file_descriptor),
            "random_access_file::size");
    return get().size();
}

void
random_access_file::resize(std::uint64_t new_size)
{
    if (!is_open())
        detail::throw_system_error(
            make_error_code(std::errc::bad_file_descriptor),
            "random_access_file::resize");
    get().resize(new_size);
}

void
random_access_file::sync_data()
{
    if (!is_open())
        detail::throw_system_error(
            make_error_code(std::errc::bad_file_descriptor),
            "random_access_file::sync_data");
    get().sync_data();
}

void
random_access_file::sync_all()
{
    if (!is_open())
        detail::throw_system_error(
            make_error_code(std::errc::bad_file_descriptor),
            "random_access_file::sync_all");
    get().sync_all();
}

native_handle_type
random_access_file::release()
{
    if (!is_open())
        detail::throw_system_error(
            make_error_code(std::errc::bad_file_descriptor),
            "random_access_file::release");
    return get().release();
}

void
random_access_file::assign(native_handle_type handle)
{
    if (is_open())
        close();
    get().assign(handle);
}

} // namespace boost::corosio
