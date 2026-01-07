//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef CALLBACK_HANDLER_HPP
#define CALLBACK_HANDLER_HPP

#include <cstddef>

namespace callback {

// Size of the largest possible member function pointer
constexpr std::size_t memfn_size = [] {
    struct base1
    {
        virtual void f1() {}
    };
    struct base2
    {
        virtual void f2() {}
    };
    struct derived : virtual base1, virtual base2
    {
        virtual void f3() {}
    };
    return sizeof(void (derived::*)());
}();

static_assert(memfn_size >= 2 * sizeof(void*),
    "member function pointer size should be at least 2 * sizeof(void*)");

/** A handler struct that mimics the size of member function pointers.

    This struct is designed to have the same size as a member function pointer
    on the target platform, reflecting real-world callback usage where handlers
    are often member function pointers or similar-sized callable objects.
*/
struct handler
{
    static_assert(
        memfn_size >= sizeof(int*), "member function pointer size must be at least pointer size");

    int* count_ptr_;
    std::byte padding_[memfn_size - sizeof(int*)];

    explicit handler(int& count) noexcept : count_ptr_(&count)
    {
        // Initialize padding to avoid uninitialized memory
        for(std::size_t i = 0; i < sizeof(padding_); ++i)
            padding_[i] = std::byte{0};
    }

    void operator()() const noexcept { ++(*count_ptr_); }
};

static_assert(
    sizeof(handler) == memfn_size, "handler size must match member function pointer size");

} // namespace callback

#endif

