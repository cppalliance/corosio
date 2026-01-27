//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#include "src/detail/config_backend.hpp"

#if defined(BOOST_COROSIO_BACKEND_IOCP)

#include "src/detail/iocp/wsa_init.hpp"
#include "src/detail/make_err.hpp"

#include <boost/corosio/detail/except.hpp>

namespace boost::corosio::detail {

long win_wsa_init::count_ = 0;

win_wsa_init::win_wsa_init()
{
    if (::InterlockedIncrement(&count_) == 1)
    {
        WSADATA wsaData;
        int result = ::WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (result != 0)
        {
            ::InterlockedDecrement(&count_);
            throw_system_error(make_err(result));
        }
    }
}

win_wsa_init::~win_wsa_init()
{
    if (::InterlockedDecrement(&count_) == 0)
        ::WSACleanup();
}

} // namespace boost::corosio::detail

#endif // _WIN32
