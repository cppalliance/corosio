//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_MAKE_ERR_HPP
#define BOOST_COROSIO_DETAIL_MAKE_ERR_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/platform.hpp>
#include <boost/capy/error.hpp>
#include <system_error>

#if BOOST_COROSIO_POSIX
#include <errno.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace boost::corosio::detail {

#if BOOST_COROSIO_POSIX

/** Convert a POSIX errno value to std::error_code.

    Maps ECANCELED to capy::error::canceled.

    @param errn The errno value.
    @return The corresponding std::error_code.
*/
inline std::error_code
make_err(int errn) noexcept
{
    if (errn == 0)
        return {};

    if (errn == ECANCELED)
        return capy::error::canceled;

    return std::error_code(errn, std::system_category());
}

#else

/** Convert a Windows error code to std::error_code.

    Maps ERROR_OPERATION_ABORTED and ERROR_CANCELLED to capy::error::canceled.
    Maps ERROR_HANDLE_EOF to capy::error::eof.

    @param dwError The Windows error code (DWORD).
    @return The corresponding std::error_code.
*/
inline std::error_code
make_err(unsigned long dwError) noexcept
{
    if (dwError == 0)
        return {};

    if (dwError == ERROR_OPERATION_ABORTED || dwError == ERROR_CANCELLED)
        return capy::error::canceled;

    if (dwError == ERROR_HANDLE_EOF)
        return capy::error::eof;

    return std::error_code(static_cast<int>(dwError), std::system_category());
}

#endif

} // namespace boost::corosio::detail

#endif
