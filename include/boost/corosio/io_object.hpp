//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_IO_OBJECT_HPP
#define BOOST_COROSIO_IO_OBJECT_HPP

#include <boost/corosio/detail/config.hpp>

namespace boost {
namespace corosio {

/** Base class for I/O objects in the library hierarchy.

    This class provides a common base for all I/O object implementations
    in the library. It holds the implementation pointer (`impl_`) which
    provides a unified interface for all derived classes in the hierarchy.

    By using a single pointer to a polymorphic base (`impl_base`), all
    classes in the I/O object hierarchy can leverage type erasure to
    share common implementation patterns while maintaining type safety
    through the virtual interface.

    @note This class is intended for use as a protected base class.
        The implementation pointer is accessible to derived classes
        through the protected member `impl_`.
*/
class io_object
{
public:
    struct io_object_impl
    {
        virtual ~io_object_impl() = default;

        virtual void release() = 0;
    };

protected:
    virtual ~io_object() = default;
    io_object() = default;
    
    io_object_impl* impl_ = nullptr;
};

} // namespace corosio
} // namespace boost

#endif
