//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_IOCP_WINDOWS_HPP
#define BOOST_COROSIO_DETAIL_IOCP_WINDOWS_HPP


#if defined(_WIN32)

#if defined(_WIN32_WINNT) && (_WIN32_WINNT < 0x0600)
#error "corosio requires Windows Vista or later (_WIN32_WINNT >= 0x0600)"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <WinSock2.h>
#include <Windows.h>

#endif // _WIN32

#endif // BOOST_COROSIO_DETAIL_IOCP_WINDOWS_HPP
