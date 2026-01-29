//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "src/detail/make_err.hpp"

#include <boost/capy/error.hpp>

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

std::error_code
make_err(int errn) noexcept
{
    if (errn == 0)
        return {};

    if (errn == ECANCELED)
        return capy::error::canceled;

    return std::error_code(errn, std::system_category());
}

#else

std::error_code
make_err(unsigned long dwError) noexcept
{
    if (dwError == 0)
        return {};

    if (dwError == ERROR_OPERATION_ABORTED ||
        dwError == ERROR_CANCELLED)
        return capy::error::canceled;

    if (dwError == ERROR_HANDLE_EOF)
        return capy::error::eof;

    return std::error_code(
        static_cast<int>(dwError),
        std::system_category());
}

#endif

} // namespace boost::corosio::detail
