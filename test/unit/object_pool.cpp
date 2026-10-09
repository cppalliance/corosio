#include <boost/corosio/detail/object_pool.hpp>
#include <boost/corosio/io/io_object.hpp>
#include <boost/corosio/detail/object_ref.hpp>
#include "test_suite.hpp"

namespace boost::corosio::detail {

namespace {

struct pool_impl final
    : io_object::implementation
    , intrusive_list<pool_impl>::node
{
    static inline int instances = 0;

    object_pool<pool_impl>* pool = nullptr;
    int reused_count            = 0;

    pool_impl() { ++instances; }
    ~pool_impl() override { --instances; }

    void reuse() noexcept { ++reused_count; }
    void retire() noexcept override { pool->recycle(this); }
};

// Exercises the sanctioned-extension mechanism documented on
// object_pool: a derived pool re-exposing adopt()/remove() as public,
// the same way timer_object_pool does for the timer service.
struct test_pool : object_pool<pool_impl>
{
    using object_pool::adopt;
    using object_pool::remove;
};

// An impl holding an embedded object_ref to itself never reaches
// zero through ordinary release() calls; only ~object_pool()'s sweep
// can free it, and that sweep's `delete` releases the embedded ref.
struct self_ref_impl final
    : io_object::implementation
    , intrusive_list<self_ref_impl>::node
{
    static inline int instances = 0;

    object_pool<self_ref_impl>* pool = nullptr;
    object_ref self_ref;

    self_ref_impl() { ++instances; }
    ~self_ref_impl() override { --instances; }

    void reuse() noexcept { }
    void retire() noexcept override { pool->recycle(this); }
};

// The same self-reference held by a base class while only the final
// class overrides retire(), the layout reactor impls use: a release
// reaching zero during `delete` would call retire() through a base
// whose own override is pure.
struct self_ref_base
    : io_object::implementation
    , intrusive_list<struct self_ref_derived>::node
{
    object_ref self_ref;
};

struct self_ref_derived final : self_ref_base
{
    static inline int instances = 0;

    object_pool<self_ref_derived>* pool = nullptr;

    self_ref_derived() { ++instances; }
    ~self_ref_derived() override { --instances; }

    void reuse() noexcept { }
    void retire() noexcept override { pool->recycle(this); }
};

} // namespace

struct object_pool_test
{
    void run()
    {
        // try_acquire refuses an impl parked on the free list: its count
        // holds the pool's sentinel, not a live reference
        {
            object_pool<pool_impl> pool;
            auto* a = pool.acquire();
            a->pool = &pool;
            release(a);                       // parked in free_
            BOOST_TEST(!try_acquire(a));
            BOOST_TEST(!object_ref::try_from(a));
            auto* b = pool.acquire();         // still recyclable intact
            BOOST_TEST_EQ(b, a);
            BOOST_TEST_EQ(b->refs_.load(), 1u);
            release(b);
        }
        // acquire news on a cold pool and hands the same impl back,
        // reuse()-reset, after it is recycled
        {
            object_pool<pool_impl> pool;
            auto* a = pool.acquire();
            a->pool = &pool;
            BOOST_TEST_EQ(a->reused_count, 0);
            release(a);                       // service ref -> recycle
            BOOST_TEST_EQ(pool_impl::instances, 1);
            auto* b = pool.acquire();         // warm: recycled, not newed
            BOOST_TEST_EQ(b, a);
            BOOST_TEST_EQ(b->reused_count, 1);
            BOOST_TEST_EQ(pool_impl::instances, 1);
            release(b);                       // back to free; dtor frees
        }
        BOOST_TEST_EQ(pool_impl::instances, 0);
        // keepalive defers recycling until the last ref drops
        {
            object_pool<pool_impl> pool;
            auto* a = pool.acquire();
            a->pool = &pool;
            object_ref op_ref(a);             // simulated in-flight op
            release(a);                       // service ref gone...
            auto* b = pool.acquire();         // ...a not recycled: news
            b->pool = &pool;
            BOOST_TEST_NE(b, a);
            BOOST_TEST_EQ(pool_impl::instances, 2);
            op_ref.reset();                   // op drains -> a recycles
            auto* c = pool.acquire();         // warm again
            BOOST_TEST_EQ(c, a);
            release(b);
            release(c);
        }
        BOOST_TEST_EQ(pool_impl::instances, 0);
        // shutdown mode deletes instead of pooling
        {
            object_pool<pool_impl> pool;
            auto* a = pool.acquire();
            a->pool = &pool;
            pool.shutdown([](pool_impl*) {});
            release(a);                       // deleted, not pooled
            BOOST_TEST_EQ(pool_impl::instances, 0);
        }
        // shutdown() skips an impl that already reached zero: its
        // retire() is about to recycle (and, once shutting down,
        // delete) it, so the callback must never take a reference.
        {
            object_pool<pool_impl> pool;
            auto* live     = pool.acquire();
            auto* retiring = pool.acquire();
            live->pool     = &pool;
            retiring->pool = &pool;
            retiring->refs_.store(0); // its recycle() has not run yet
            int visited = 0;
            pool.shutdown(
                [&](pool_impl* p)
                {
                    BOOST_TEST_EQ(p, live);
                    ++visited;
                });
            BOOST_TEST_EQ(visited, 1);
            pool.recycle(retiring); // shutting down: deleted
            BOOST_TEST_EQ(pool_impl::instances, 1);
            release(live);
        }
        BOOST_TEST_EQ(pool_impl::instances, 0);
        // shutdown() pins each visited impl for the callback and drops
        // the pin afterwards; the last reference released by that drop
        // deletes, since the pool is shutting down.
        {
            object_pool<pool_impl> pool;
            auto* a = pool.acquire();
            a->pool = &pool;
            pool.shutdown(
                [&](pool_impl* p)
                {
                    BOOST_TEST_EQ(p->refs_.load(), 2u);
                    release(p); // the handle's reference goes meanwhile
                });
            BOOST_TEST_EQ(pool_impl::instances, 0);
        }
        // remove() transfers ownership out of the pool entirely: the
        // impl leaves live_ and the caller now owns it outright (a
        // direct delete here must not be seen again by the pool's own
        // destructor -- the scope exit below proves no double free).
        {
            test_pool pool;
            auto* a = pool.acquire();
            a->pool = &pool;
            BOOST_TEST(pool.remove(a));       // unlinked from live_
            delete a;                         // caller-owned now
            // pool goes out of scope here with empty live_/free_: if
            // remove() had failed to unlink `a`, ~object_pool() would
            // delete it a second time.
        }
        BOOST_TEST_EQ(pool_impl::instances, 0);
        // remove() is idempotent: a second call on an already-detached
        // impl returns false instead of corrupting the list (mirrors
        // intrusive_list::remove()'s own already-detached contract).
        {
            test_pool pool;
            auto* a = pool.acquire();
            a->pool = &pool;
            BOOST_TEST(pool.remove(a));
            BOOST_TEST(!pool.remove(a));      // already detached
            delete a;
        }
        // Sentinel guard: remove() must refuse to touch a free_-parked
        // impl (the same hazard recycle() guards against in reverse).
        // The guard's debug-build path asserts and aborts before
        // returning, so only its release-build-observable contract --
        // refused, false, free list left intact -- can be exercised
        // here; it is skipped entirely when assertions are compiled in
        // (the normal state of this test binary).
#ifdef NDEBUG
        {
            test_pool pool;
            auto* a = pool.acquire();
            a->pool = &pool;
            release(a); // parks a on free_ (refs_ == pooled_sentinel)
            BOOST_TEST(!pool.remove(a));      // refused
            auto* b = pool.acquire();         // free_ untouched: warm hit
            BOOST_TEST_EQ(b, a);
            release(b); // back to free_; pool dtor frees it
        }
#endif
        BOOST_TEST_EQ(pool_impl::instances, 0);
        // recycle()'s own guards, release-build contract, mirroring
        // the remove() block above: both skip entirely under
        // assertions (the normal state of this test binary) because
        // the guarded branch asserts and aborts before returning.
#ifdef NDEBUG
        {
            // Sentinel guard: a duplicate recycle() on an impl
            // already parked in free_ (e.g. a double retire() for
            // one zero-crossing) must refuse before touching either
            // list, not corrupt free_ by removing it through live_'s
            // boundary pointers.
            test_pool pool;
            auto* a = pool.acquire();
            a->pool = &pool;
            release(a);                // 1 -> 0: recycle() parks a on free_
            BOOST_TEST_EQ(pool_impl::instances, 1);
            pool.recycle(a);           // duplicate retire while parked: refused
            auto* b = pool.acquire();  // free_ untouched: still a warm hit
            BOOST_TEST_EQ(b, a);
            release(b);
        }
        BOOST_TEST_EQ(pool_impl::instances, 0);
        {
            // Accounting-bug branch: an impl detached from live_
            // (not parked, not shutting down) reaching recycle() is
            // deleted outright instead of being stranded (reachable
            // through neither acquire() nor shutdown()'s visit while
            // still allocated).
            test_pool pool;
            auto* a = pool.acquire();
            a->pool = &pool;
            BOOST_TEST(pool.remove(a)); // detach from live_ without recycling
            pool.recycle(a);            // not live, not pooled: deleted
            BOOST_TEST_EQ(pool_impl::instances, 0);
        }
#endif
        // A self-referential impl is freed exactly once by the
        // destructor's sweep; releasing its embedded ref during that
        // `delete` must not retire it again.
        {
            object_pool<self_ref_impl> pool;
            auto* a     = pool.acquire();
            a->pool     = &pool;
            a->self_ref = object_ref(a);   // refs_: 1 -> 2
            release(a);          // refs_: 2 -> 1; self-ref keeps it alive
            BOOST_TEST_EQ(self_ref_impl::instances, 1);
            pool.shutdown([](self_ref_impl*) {});
        }
        BOOST_TEST_EQ(self_ref_impl::instances, 0);
        // Same, with the self-reference held by a base class.
        {
            object_pool<self_ref_derived> pool;
            auto* a     = pool.acquire();
            a->pool     = &pool;
            a->self_ref = object_ref(a);
            release(a);
            pool.shutdown([](self_ref_derived*) {});
        }
        BOOST_TEST_EQ(self_ref_derived::instances, 0);
    }
};

TEST_SUITE(object_pool_test, "boost.corosio.object_pool");

} // namespace boost::corosio::detail
