//
// Copyright (c) 2026 Steve Gerbino
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

// Global allocation replacement consulting the `cpp_new` arm. Unlike
// the syscall shadows this needs no interposition machinery: replacing
// the global allocation functions in any translation unit of the
// program rebinds every `new` in the executable — and, on ELF and
// Mach-O, in the shared libraries it loads — so a test can fail the
// nth allocation of a scope and drive the library's `bad_alloc`
// recovery arms. The replacements forward to `malloc`, whose own
// failure still reports `bad_alloc` the standard way.

#include "fault.hpp"
#include "fault_slot.hpp"

#include <cstdlib>
#include <new>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace {

using boost::corosio::test::fault::should_fail;
using boost::corosio::test::fault::sys;

void*
plain_alloc(std::size_t n)
{
    if (should_fail(sys::cpp_new))
        return nullptr;
    // malloc(0) may return null without that being exhaustion.
    return std::malloc(n ? n : 1);
}

void*
aligned_alloc_impl(std::size_t n, std::size_t align)
{
    if (should_fail(sys::cpp_new))
        return nullptr;
#if defined(_WIN32)
    return ::_aligned_malloc(n ? n : 1, align);
#else
    void* p = nullptr;
    if (::posix_memalign(&p, align, n ? n : 1) != 0)
        return nullptr;
    return p;
#endif
}

void
aligned_free_impl(void* p) noexcept
{
#if defined(_WIN32)
    ::_aligned_free(p);
#else
    std::free(p);
#endif
}

} // namespace

void*
operator new(std::size_t n)
{
    if (void* p = plain_alloc(n))
        return p;
    throw std::bad_alloc{};
}

void*
operator new[](std::size_t n)
{
    return ::operator new(n);
}

void*
operator new(std::size_t n, std::nothrow_t const&) noexcept
{
    return plain_alloc(n);
}

void*
operator new[](std::size_t n, std::nothrow_t const&) noexcept
{
    return plain_alloc(n);
}

void*
operator new(std::size_t n, std::align_val_t align)
{
    if (void* p = aligned_alloc_impl(n, static_cast<std::size_t>(align)))
        return p;
    throw std::bad_alloc{};
}

void*
operator new[](std::size_t n, std::align_val_t align)
{
    return ::operator new(n, align);
}

void*
operator new(std::size_t n, std::align_val_t align,
    std::nothrow_t const&) noexcept
{
    return aligned_alloc_impl(n, static_cast<std::size_t>(align));
}

void*
operator new[](std::size_t n, std::align_val_t align,
    std::nothrow_t const&) noexcept
{
    return aligned_alloc_impl(n, static_cast<std::size_t>(align));
}

void
operator delete(void* p) noexcept
{
    std::free(p);
}

void
operator delete[](void* p) noexcept
{
    std::free(p);
}

void
operator delete(void* p, std::size_t) noexcept
{
    std::free(p);
}

void
operator delete[](void* p, std::size_t) noexcept
{
    std::free(p);
}

void
operator delete(void* p, std::nothrow_t const&) noexcept
{
    std::free(p);
}

void
operator delete[](void* p, std::nothrow_t const&) noexcept
{
    std::free(p);
}

void
operator delete(void* p, std::align_val_t) noexcept
{
    aligned_free_impl(p);
}

void
operator delete[](void* p, std::align_val_t) noexcept
{
    aligned_free_impl(p);
}

void
operator delete(void* p, std::size_t, std::align_val_t) noexcept
{
    aligned_free_impl(p);
}

void
operator delete[](void* p, std::size_t, std::align_val_t) noexcept
{
    aligned_free_impl(p);
}

void
operator delete(void* p, std::align_val_t, std::nothrow_t const&) noexcept
{
    aligned_free_impl(p);
}

void
operator delete[](void* p, std::align_val_t, std::nothrow_t const&) noexcept
{
    aligned_free_impl(p);
}
