//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_WIN_WSA_INIT_HPP
#define BOOST_COROSIO_DETAIL_WIN_WSA_INIT_HPP

#include <boost/corosio/detail/config.hpp>

#include "src/detail/windows.hpp"

namespace boost {
namespace corosio {
namespace detail {

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

    win_wsa_init(win_wsa_init const&) = delete;
    win_wsa_init& operator=(win_wsa_init const&) = delete;

private:
    static long count_;
};

} // namespace detail
} // namespace corosio
} // namespace boost

#endif
