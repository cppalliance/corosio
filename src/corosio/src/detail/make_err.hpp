//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef SRC_DETAIL_MAKE_ERR_HPP
#define SRC_DETAIL_MAKE_ERR_HPP

#include <boost/corosio/detail/config.hpp>
#include <boost/system/error_code.hpp>

namespace boost::corosio::detail {

#if defined(_WIN32)
/** Convert a Windows error code to system::error_code.

    Maps ERROR_OPERATION_ABORTED and ERROR_CANCELLED to capy::error::canceled.
    Maps ERROR_HANDLE_EOF to capy::error::eof.

    @param dwError The Windows error code (DWORD).
    @return The corresponding system::error_code.
*/
system::error_code make_err(unsigned long dwError) noexcept;
#else
/** Convert a POSIX errno value to system::error_code.

    Maps ECANCELED to capy::error::canceled.

    @param errn The errno value.
    @return The corresponding system::error_code.
*/
system::error_code make_err(int errn) noexcept;
#endif

} // namespace boost::corosio::detail

#endif
