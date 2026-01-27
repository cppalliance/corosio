//
// Copyright (c) 2025 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/cppalliance/corosio
//

#ifndef BOOST_COROSIO_DETAIL_INTRUSIVE_HPP
#define BOOST_COROSIO_DETAIL_INTRUSIVE_HPP

namespace boost::corosio::detail {

//------------------------------------------------

/** An intrusive doubly linked list.

    This container provides O(1) push and pop operations for
    elements that derive from @ref node. Elements are not
    copied or moved; they are linked directly into the list.

    @tparam T The element type. Must derive from `intrusive_list<T>::node`.
*/
template<class T>
class intrusive_list
{
public:
    /** Base class for list elements.

        Derive from this class to make a type usable with
        @ref intrusive_list. The `next_` and `prev_` pointers
        are private and accessible only to the list.
    */
    class node
    {
        friend class intrusive_list;

    private:
        T* next_;
        T* prev_;
    };

private:
    T* head_ = nullptr;
    T* tail_ = nullptr;

public:
    intrusive_list() = default;

    intrusive_list(intrusive_list&& other) noexcept
        : head_(other.head_)
        , tail_(other.tail_)
    {
        other.head_ = nullptr;
        other.tail_ = nullptr;
    }

    intrusive_list(intrusive_list const&) = delete;
    intrusive_list& operator=(intrusive_list const&) = delete;
    intrusive_list& operator=(intrusive_list&&) = delete;

    bool
    empty() const noexcept
    {
        return head_ == nullptr;
    }

    void
    push_back(T* w) noexcept
    {
        w->next_ = nullptr;
        w->prev_ = tail_;
        if(tail_)
            tail_->next_ = w;
        else
            head_ = w;
        tail_ = w;
    }

    void
    splice_back(intrusive_list& other) noexcept
    {
        if(other.empty())
            return;
        if(tail_)
        {
            tail_->next_ = other.head_;
            other.head_->prev_ = tail_;
            tail_ = other.tail_;
        }
        else
        {
            head_ = other.head_;
            tail_ = other.tail_;
        }
        other.head_ = nullptr;
        other.tail_ = nullptr;
    }

    T*
    pop_front() noexcept
    {
        if(!head_)
            return nullptr;
        T* w = head_;
        head_ = head_->next_;
        if(head_)
            head_->prev_ = nullptr;
        else
            tail_ = nullptr;
        return w;
    }

    void
    remove(T* w) noexcept
    {
        if(w->prev_)
            w->prev_->next_ = w->next_;
        else
            head_ = w->next_;
        if(w->next_)
            w->next_->prev_ = w->prev_;
        else
            tail_ = w->prev_;
    }
};

//------------------------------------------------

/** An intrusive singly linked FIFO queue.

    This container provides O(1) push and pop operations for
    elements that derive from @ref node. Elements are not
    copied or moved; they are linked directly into the queue.

    Unlike @ref intrusive_list, this uses only a single `next_`
    pointer per node, saving memory at the cost of not supporting
    O(1) removal of arbitrary elements.

    @tparam T The element type. Must derive from `intrusive_queue<T>::node`.
*/
template<class T>
class intrusive_queue
{
public:
    /** Base class for queue elements.

        Derive from this class to make a type usable with
        @ref intrusive_queue. The `next_` pointer is private
        and accessible only to the queue.
    */
    class node
    {
        friend class intrusive_queue;

    private:
        T* next_;
    };

private:
    T* head_ = nullptr;
    T* tail_ = nullptr;

public:
    intrusive_queue() = default;

    intrusive_queue(intrusive_queue&& other) noexcept
        : head_(other.head_)
        , tail_(other.tail_)
    {
        other.head_ = nullptr;
        other.tail_ = nullptr;
    }

    intrusive_queue(intrusive_queue const&) = delete;
    intrusive_queue& operator=(intrusive_queue const&) = delete;
    intrusive_queue& operator=(intrusive_queue&&) = delete;

    bool
    empty() const noexcept
    {
        return head_ == nullptr;
    }

    void
    push(T* w) noexcept
    {
        w->next_ = nullptr;
        if(tail_)
            tail_->next_ = w;
        else
            head_ = w;
        tail_ = w;
    }

    void
    splice(intrusive_queue& other) noexcept
    {
        if(other.empty())
            return;
        if(tail_)
            tail_->next_ = other.head_;
        else
            head_ = other.head_;
        tail_ = other.tail_;
        other.head_ = nullptr;
        other.tail_ = nullptr;
    }

    T*
    pop() noexcept
    {
        if(!head_)
            return nullptr;
        T* w = head_;
        head_ = head_->next_;
        if(!head_)
            tail_ = nullptr;
        return w;
    }
};

} // namespace boost::corosio::detail

#endif
