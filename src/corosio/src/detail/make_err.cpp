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
#include <boost/system/system_category.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <errno.h>
#endif

namespace boost::corosio::detail {

#if defined(_WIN32)

system::error_code
make_err(unsigned long dwError) noexcept
{
    if (dwError == 0)
        return {};

    if (dwError == ERROR_OPERATION_ABORTED ||
        dwError == ERROR_CANCELLED)
        return capy::error::canceled;

    if (dwError == ERROR_HANDLE_EOF)
        return capy::error::eof;

    return system::error_code(
        static_cast<int>(dwError),
        system::system_category());
}

#else

system::error_code
make_err(int errn) noexcept
{
    if (errn == 0)
        return {};

    if (errn == ECANCELED)
        return capy::error::canceled;

    return system::error_code(errn, system::system_category());
}

#endif

} // namespace boost::corosio::detail
