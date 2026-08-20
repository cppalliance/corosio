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

std::error_code
random_access_file::open(
    std::filesystem::path const& path, file_base::flags mode) noexcept
{
    if (is_open())
        close();
    auto& svc = static_cast<detail::random_access_file_service&>(h_.service());
    return svc.open_file(get(), path, mode);
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

std::error_code
random_access_file::resize(std::uint64_t new_size) noexcept
{
    if (!is_open())
        return make_error_code(std::errc::bad_file_descriptor);
    return get().resize(new_size);
}

std::error_code
random_access_file::sync_data() noexcept
{
    if (!is_open())
        return make_error_code(std::errc::bad_file_descriptor);
    return get().sync_data();
}

std::error_code
random_access_file::sync_all() noexcept
{
    if (!is_open())
        return make_error_code(std::errc::bad_file_descriptor);
    return get().sync_all();
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

std::error_code
random_access_file::assign(native_handle_type handle) noexcept
{
    if (is_open())
        close();
    return get().assign(handle);
}

} // namespace boost::corosio
