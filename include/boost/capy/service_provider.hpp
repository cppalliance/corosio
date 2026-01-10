//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_CAPY_SERVICE_PROVIDER_HPP
#define BOOST_CAPY_SERVICE_PROVIDER_HPP

#include <boost/capy/config.hpp>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <typeindex>

namespace boost {
namespace capy {

class service_provider;

//------------------------------------------------

/** Abstract base class for all services.

    Services must derive from this class and implement
    the shutdown() member function which is called when
    the owning service_provider is stopped.
*/
class service
{
public:
    virtual ~service() = default;

protected:
    service() = default;

    /** Called when the service_provider is stopped.

        Derived classes implement this to perform cleanup.
        Services are stopped in reverse order of creation.
    */
    virtual void shutdown() = 0;

private:
    friend class service_provider;

    service* next_ = nullptr;
    std::type_index t0_ = typeid(void);
    std::type_index t1_ = typeid(void);
};

//------------------------------------------------

/** A container of polymorphic service objects.

    Services are stored and retrieved by their type.
    Each type may be stored at most once. Types may
    specify a nested `key_type` to be used as an
    additional lookup key. In this case, a reference
    to the type must be convertible to a reference
    to the key type.

    @par Thread Safety
    All member functions are thread-safe.

    @par Example
    @code
    struct file_service : service
    {
    };

    struct posix_file_service : file_service
    {
        using key_type = file_service;

        posix_file_service(service_provider& sp)
        {
        }

        void shutdown() override
        {
        }
    };

    class io_context : public service_provider
    {
    public:
        ~io_context()
        {
            do_shutdown();
        }
    };

    io_context ctx;
    ctx.make_service<posix_file_service>();
    ctx.find_service<file_service>();  // works
    ctx.find_service<posix_file_service>(); // works
    @endcode
*/
class service_provider
{
    template<class T, class = void>
    struct get_key : std::false_type
    {};

    template<class T>
    struct get_key<T, std::void_t<typename T::key_type>> : std::true_type
    {
        using type = typename T::key_type;
    };

public:
    ~service_provider();

    /** Return true if a service of type T exists.

        @tparam T The type of service to check.
        @return true if the service exists.
    */
    template<class T>
    bool has_service() const noexcept;

    /** Return a pointer to the service of type T, or nullptr.

        @tparam T The type of service to find.
        @return A pointer to the service, or nullptr if not present.
    */
    template<class T>
    T* find_service() const noexcept;

    /** Return a reference to the service of type T, creating it if needed.

        If no service of type T exists, one is created by calling
        `T(service_provider&)`. If T has a nested key_type, the
        service is also indexed under that type.

        @par Constraints
        @li `T` must derive from `service`
        @li `T` must be constructible from `service_provider&`

        @tparam T The type of service to retrieve or create.
        @return A reference to the service.
    */
    template<class T>
    T& use_service();

    /** Construct and add a service.

        A new service of type T is constructed using the provided
        arguments and added to the container. If T has a nested
        key_type, the service is also indexed under that type.

        @par Constraints
        @li `T` must derive from `service`
        @li `T` must be constructible from `service_provider&, Args...`
        @li If `T::key_type` exists, `T&` must be convertible to `key_type&`

        @throws std::invalid_argument if a service of the same type
            or key_type already exists.

        @tparam T The type of service to create.
        @param args Arguments forwarded to the constructor of T.
        @return A reference to the created service.
    */
    template<class T, class... Args>
    T& make_service(Args&&... args);

protected:
    service_provider();
    service_provider(service_provider const&) = delete;
    service_provider& operator=(service_provider const&) = delete;

private:
    struct ptr
    {
        service* p;
        ~ptr() { delete p; }
    };
    service* find_impl(std::type_index ti) const noexcept;

    mutable std::mutex mutex_;
    service* head_ = nullptr;
};

//------------------------------------------------

inline service_provider::~service_provider()
{
    service* p = head_;
    while(p)
    {
        service* next = p->next_;
        p->shutdown();
        delete p;
        p = next;
    }
}

inline service_provider::service_provider() = default;

template<class T>
bool service_provider::has_service() const noexcept
{
    return find_service<T>() != nullptr;
}

template<class T>
T* service_provider::find_service() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<T*>(find_impl(typeid(T)));
}

template<class T>
T& service_provider::use_service()
{
    static_assert(std::is_base_of<service, T>::value, "T must derive from service");
    static_assert(std::is_constructible<T, service_provider&>::value,
        "T must be constructible from service_provider&");

    std::unique_lock<std::mutex> lock(mutex_);

    if(auto* p = find_impl(typeid(T)))
        return *static_cast<T*>(p);

    lock.unlock();

    // Create the service outside lock, enabling nested calls
    ptr sp{new T(*this)};

    sp.p->t0_ = typeid(T);
    if constexpr(get_key<T>::value)
    {
        static_assert(std::is_convertible<T&, typename get_key<T>::type&>::value,
            "T& must be convertible to key_type&");
        sp.p->t1_ = typeid(typename get_key<T>::type);
    }
    else
    {
        sp.p->t1_ = sp.p->t0_;
    }

    lock.lock();

    if(auto p = find_impl(typeid(T)))
        return *static_cast<T*>(p);

    sp.p->next_ = head_;
    head_ = sp.p;
    sp.p = nullptr;

    return *static_cast<T*>(head_);
}

template<class T, class... Args>
T& service_provider::make_service(Args&&... args)
{
    static_assert(std::is_base_of<service, T>::value, "T must derive from service");

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(find_impl(typeid(T)))
            throw std::invalid_argument("service already exists");
        if constexpr(get_key<T>::value)
        {
            static_assert(std::is_convertible<T&, typename get_key<T>::type&>::value,
                "T& must be convertible to key_type&");
            if(find_impl(typeid(typename get_key<T>::type)))
                throw std::invalid_argument("service key_type already exists");
        }
    }

    // Unlocked to allow nested service creation from constructor
    auto p = new T(*this, std::forward<Args>(args)...);

    std::lock_guard<std::mutex> lock(mutex_);
    if(find_impl(typeid(T)))
        throw std::invalid_argument("service already exists");

    p->t0_ = typeid(T);
    if constexpr(get_key<T>::value)
    {
        if(find_impl(typeid(typename get_key<T>::type)))
            throw std::invalid_argument("service key_type already exists");
        p->t1_ = typeid(typename get_key<T>::type);
    }
    else
    {
        p->t1_ = p->t0_;
    }

    p->next_ = head_;
    head_ = p;

    return *p;
}

inline service* service_provider::find_impl(std::type_index ti) const noexcept
{
    auto p = head_;
    while(p)
    {
        if(p->t0_ == ti || p->t1_ == ti)
            break;
        p = p->next_;
    }
    return p;
}

} // namespace capy
} // namespace boost

#endif
