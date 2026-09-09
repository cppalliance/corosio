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
#include <boost/corosio/native/detail/make_err.hpp>
#include <boost/corosio/detail/except.hpp>

#include <boost/corosio/native/detail/iocp/win_windows.hpp>

namespace boost::corosio::detail {

/** RAII class for Winsock initialization.

    Uses reference counting to ensure WSAStartup is called once on
    first construction and WSACleanup on last destruction.

    Derive from this class to ensure Winsock is initialized before
    any socket operations.
*/
class BOOST_COROSIO_DECL win_wsa_init
{
protected:
    win_wsa_init();
    ~win_wsa_init();

    win_wsa_init(win_wsa_init const&)            = delete;
    win_wsa_init& operator=(win_wsa_init const&) = delete;
};

// Process-wide Winsock init refcount. Kept as an inline function-local
// static (one shared instance across TUs) rather than a static data member
// so win_wsa_init can carry BOOST_COROSIO_DECL: an exported/imported static
// data member defined in a header is rejected by MSVC and clang-cl
// ("definition of dllimport static field not allowed").
inline long&
win_wsa_init_count() noexcept
{
    static long count = 0;
    return count;
}

// Hold Winsock for the life of the process, so the count never falls back
// to zero between one io_context and the next. An asynchronous
// GetAddrInfoExW keeps settling inside ws2_32 past its completion
// routine, with no way to observe when, and a WSACleanup under it faults
// on a system DNS thread.
inline void
win_wsa_hold_process() noexcept
{
    struct holder
    {
        holder() noexcept
        {
            ::InterlockedIncrement(&win_wsa_init_count());
        }

        ~holder()
        {
            if (::InterlockedDecrement(&win_wsa_init_count()) == 0)
                ::WSACleanup();
        }
    };

    static holder const instance;
}

inline win_wsa_init::win_wsa_init()
{
    if (::InterlockedIncrement(&win_wsa_init_count()) == 1)
    {
        WSADATA wsaData;
        int result = ::WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (result != 0)
        {
            ::InterlockedDecrement(&win_wsa_init_count());
            throw_system_error(make_err(result));
        }
    }

    // After the startup above, which must own the first count.
    win_wsa_hold_process();
}

inline win_wsa_init::~win_wsa_init()
{
    if (::InterlockedDecrement(&win_wsa_init_count()) == 0)
        ::WSACleanup();
}

} // namespace boost::corosio::detail

#endif // BOOST_COROSIO_HAS_IOCP

#endif // BOOST_COROSIO_NATIVE_DETAIL_IOCP_WIN_WSA_INIT_HPP
