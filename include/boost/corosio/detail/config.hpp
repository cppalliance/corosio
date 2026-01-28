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

#include <cassert>

#ifndef BOOST_COROSIO_ASSERT
# define BOOST_COROSIO_ASSERT(expr) assert(expr)
#endif

// Symbol export/import for shared libraries
#if defined(_WIN32) || defined(__CYGWIN__)
# define BOOST_COROSIO_SYMBOL_EXPORT __declspec(dllexport)
# define BOOST_COROSIO_SYMBOL_IMPORT __declspec(dllimport)
#elif defined(__GNUC__) && __GNUC__ >= 4
# define BOOST_COROSIO_SYMBOL_EXPORT __attribute__((visibility("default")))
# define BOOST_COROSIO_SYMBOL_IMPORT __attribute__((visibility("default")))
#else
# define BOOST_COROSIO_SYMBOL_EXPORT
# define BOOST_COROSIO_SYMBOL_IMPORT
#endif

namespace boost::corosio {

#if (defined(BOOST_COROSIO_DYN_LINK) || defined(BOOST_ALL_DYN_LINK)) && !defined(BOOST_COROSIO_STATIC_LINK)
# if defined(BOOST_COROSIO_SOURCE)
#  define BOOST_COROSIO_DECL        BOOST_COROSIO_SYMBOL_EXPORT
#  define BOOST_COROSIO_BUILD_DLL
# else
#  define BOOST_COROSIO_DECL        BOOST_COROSIO_SYMBOL_IMPORT
# endif
#endif // shared lib

#ifndef  BOOST_COROSIO_DECL
# define BOOST_COROSIO_DECL
#endif

} // namespace boost::corosio

#endif
