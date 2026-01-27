//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_CONFIG_HPP
#define BOOST_COROSIO_DETAIL_CONFIG_HPP

#include <boost/config.hpp>

namespace boost::corosio {

//------------------------------------------------

# if (defined(BOOST_COROSIO_DYN_LINK) || defined(BOOST_ALL_DYN_LINK)) && !defined(BOOST_COROSIO_STATIC_LINK)
#  if defined(BOOST_COROSIO_SOURCE)
#   define BOOST_COROSIO_DECL        BOOST_SYMBOL_EXPORT
#   define BOOST_COROSIO_BUILD_DLL
#  else
#   define BOOST_COROSIO_DECL        BOOST_SYMBOL_IMPORT
#  endif
# endif // shared lib

# ifndef  BOOST_COROSIO_DECL
#  define BOOST_COROSIO_DECL
# endif

# if !defined(BOOST_COROSIO_SOURCE) && !defined(BOOST_ALL_NO_LIB) && !defined(BOOST_COROSIO_NO_LIB)
#  define BOOST_LIB_NAME boost_corosio
#  if defined(BOOST_ALL_DYN_LINK) || defined(BOOST_COROSIO_DYN_LINK)
#   define BOOST_DYN_LINK
#  endif
#  include <boost/config/auto_link.hpp>
# endif

//------------------------------------------------

} // namespace boost::corosio

#endif
