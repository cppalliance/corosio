//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_UNIQUE_PTR_HPP
#define BOOST_COROSIO_DETAIL_UNIQUE_PTR_HPP

#include <memory>
#include <utility>

namespace boost::corosio::detail {

/** A unique_ptr with a function pointer deleter.

    This alias provides a unique_ptr that stores a function pointer
    as its deleter, enabling type-erased custom deletion behavior
    while maintaining a fixed size regardless of the deleter type.
*/
template<class T>
using unique_ptr = std::unique_ptr<T, void(*)(void const*)>;

/** Create a unique_ptr with the default deleter.

    @tparam T The type to create.
    @tparam Args Constructor argument types.

    @param args Arguments forwarded to T's constructor.

    @return A unique_ptr owning a new instance of T.
*/
template<class T, class... Args>
unique_ptr<T>
make_unique(Args&&... args)
{
    return unique_ptr<T>(
        new T(std::forward<Args>(args)...),
        [](void const* p)
        {
            delete static_cast<T const*>(p);
        });
}

} // namespace boost::corosio::detail

#endif
