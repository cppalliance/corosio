//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_WSA_INIT_HPP
#define BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_WSA_INIT_HPP

#include <boost/corosio/detail/platform.hpp>

#if BOOST_COROSIO_HAS_IOCP

#include <boost/corosio/detail/config.hpp>
#include <boost/corosio/detail/make_err.hpp>
#include <boost/corosio/detail/except.hpp>

#include <boost/corosio/native/detail/iocp/win_windows.hpp>

namespace boost::corosio::detail {

/** RAII class for Winsock initialization.

    Uses reference counting to ensure WSAStartup is called once on
    first construction and WSACleanup on last destruction.

    Derive from this class to ensure Winsock is initialized before
    any socket operations.
*/
class win_wsa_init
{
protected:
    win_wsa_init();
    ~win_wsa_init();

    win_wsa_init(win_wsa_init const&)            = delete;
    win_wsa_init& operator=(win_wsa_init const&) = delete;

private:
    static long count_;
};

inline long win_wsa_init::count_ = 0;

inline win_wsa_init::win_wsa_init()
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

inline win_wsa_init::~win_wsa_init()
{
    if (::InterlockedDecrement(&count_) == 0)
        ::WSACleanup();
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_WSA_INIT_HPP
