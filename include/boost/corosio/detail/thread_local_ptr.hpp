//
// Copyright (c) 2025 Vinnie Falco (vinnie.falco@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_THREAD_LOCAL_PTR_HPP
#define BOOST_COROSIO_DETAIL_THREAD_LOCAL_PTR_HPP

#include <boost/corosio/detail/config.hpp>

#include <type_traits>

// Detect thread-local storage mechanism
#if !defined(BOOST_COROSIO_TLS_KEYWORD)
# if defined(_MSC_VER)
#  define BOOST_COROSIO_TLS_KEYWORD __declspec(thread)
# elif defined(__GNUC__) || defined(__clang__)
#  define BOOST_COROSIO_TLS_KEYWORD __thread
# endif
#endif

namespace boost::corosio::detail {

/** A thread-local pointer.

    This class provides thread-local storage for a pointer to T.
    Each thread has its own independent pointer value, initially
    nullptr. The user is responsible for managing the lifetime
    of the pointed-to objects.

    The storage is static per type T. All instances of
    `thread_local_ptr<T>` share the same underlying slot.

    The implementation uses the most efficient available mechanism:
    1. Compiler keyword (__declspec(thread) or __thread) - enforces POD
    2. C++11 thread_local (fallback)

    @tparam T The pointed-to type.

    @par Declaration

    Typically declared at namespace or class scope. The object
    is stateless, so local variables work but are redundant.

    @code
    // Recommended: namespace scope
    namespace {
    thread_local_ptr<session> current_session;
    }

    // Also works: static class member
    class server {
        static thread_local_ptr<request> current_request_;
    };

    // Works but unusual: local variable (still accesses static storage)
    void foo() {
        thread_local_ptr<context> ctx;  // same slot on every call
        ctx = new context();
    }
    @endcode

    @note The user is responsible for deleting pointed-to objects
    before threads exit to avoid memory leaks.
*/
template<class T>
class thread_local_ptr;

//------------------------------------------------------------------------------

#if defined(BOOST_COROSIO_TLS_KEYWORD)

// Use compiler-specific keyword (__declspec(thread) or __thread)
// Most efficient: static linkage, no dynamic init, enforces POD

template<class T>
class thread_local_ptr
{
    static BOOST_COROSIO_TLS_KEYWORD T* ptr_;

public:
    thread_local_ptr() = default;
    ~thread_local_ptr() = default;

    thread_local_ptr(thread_local_ptr const&) = delete;
    thread_local_ptr& operator=(thread_local_ptr const&) = delete;

    /** Return the pointer for this thread.

        @return The stored pointer, or nullptr if not set.
    */
    T*
    get() const noexcept
    {
        return ptr_;
    }

    /** Set the pointer for this thread.

        @param p The pointer to store. The user manages its lifetime.
    */
    void
    set(T* p) noexcept
    {
        ptr_ = p;
    }

    /** Dereference the stored pointer.

        @pre get() != nullptr
    */
    T&
    operator*() const noexcept
    {
        return *ptr_;
    }

    /** Member access through the stored pointer.

        @pre get() != nullptr
    */
    T*
    operator->() const noexcept
        requires std::is_class_v<T>
    {
        return ptr_;
    }

    /** Assign a pointer value.

        @param p The pointer to store.
        @return The stored pointer.
    */
    T*
    operator=(T* p) noexcept
    {
        ptr_ = p;
        return p;
    }
};

template<class T>
BOOST_COROSIO_TLS_KEYWORD T* thread_local_ptr<T>::ptr_ = nullptr;

//------------------------------------------------------------------------------

#else

// Use C++11 thread_local keyword (fallback)

template<class T>
class thread_local_ptr
{
    static thread_local T* ptr_;

public:
    thread_local_ptr() = default;
    ~thread_local_ptr() = default;

    thread_local_ptr(thread_local_ptr const&) = delete;
    thread_local_ptr& operator=(thread_local_ptr const&) = delete;

    T*
    get() const noexcept
    {
        return ptr_;
    }

    void
    set(T* p) noexcept
    {
        ptr_ = p;
    }

    T&
    operator*() const noexcept
    {
        return *ptr_;
    }

    T*
    operator->() const noexcept
        requires std::is_class_v<T>
    {
        return ptr_;
    }

    T*
    operator=(T* p) noexcept
    {
        ptr_ = p;
        return p;
    }
};

template<class T>
thread_local T* thread_local_ptr<T>::ptr_ = nullptr;

#endif

} // namespace boost::corosio::detail

#endif
