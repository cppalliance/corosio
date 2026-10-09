#include <boost/corosio/detail/object_ref.hpp>
#include <boost/corosio/io/io_object.hpp>
#include "test_suite.hpp"

namespace boost::corosio::detail {

namespace {

struct counted_impl final : io_object::implementation
{
    int* last_ref_hits;
    explicit counted_impl(int* hits) : last_ref_hits(hits) {}
    void retire() noexcept override { ++*last_ref_hits; }
};

} // namespace

struct object_ref_test
{
    void run()
    {
        // acquire/release pairs; zero-action fires exactly once
        {
            int hits = 0;
            counted_impl impl(&hits);           // refs_ starts at 1
            release(&impl);                     // 1 -> 0
            BOOST_TEST_EQ(hits, 1);
        }
        // object_ref RAII: ctor acquires, dtor releases
        {
            int hits = 0;
            counted_impl impl(&hits);
            {
                object_ref r(&impl);              // 1 -> 2
                BOOST_TEST(r);
                object_ref r2 = r;                // copy: 2 -> 3
                object_ref r3 = std::move(r2);    // move: still 3
                BOOST_TEST(!r2);
                BOOST_TEST_EQ(r3.get(), &impl);
            }                                   // back to 1
            BOOST_TEST_EQ(hits, 0);
            release(&impl);
            BOOST_TEST_EQ(hits, 1);
        }
        // try_from adds a reference to a live impl and refuses a
        // retiring one
        {
            int hits = 0;
            counted_impl impl(&hits);
            {
                auto r = object_ref::try_from(&impl); // 1 -> 2
                BOOST_TEST_EQ(r.get(), &impl);
                release(&impl); // 2 -> 1
                BOOST_TEST_EQ(hits, 0);
            } // 1 -> 0
            BOOST_TEST_EQ(hits, 1);
            auto dead = object_ref::try_from(&impl);
            BOOST_TEST(!dead);
            BOOST_TEST_EQ(hits, 1);
        }
        // reset releases early
        {
            int hits = 0;
            counted_impl impl(&hits);
            object_ref r(&impl);
            release(&impl);                     // service ref gone, r holds it
            BOOST_TEST_EQ(hits, 0);
            r.reset();
            BOOST_TEST_EQ(hits, 1);
        }
        // copy-assignment: operator=(object_ref const&) is never
        // otherwise executed anywhere in the tree. Verify it releases
        // the old target and acquires the new one, and is safe under
        // self-assignment.
        {
            int hits_a = 0;
            int hits_b = 0;
            counted_impl a(&hits_a);
            counted_impl b(&hits_b);

            object_ref ra(&a);              // a.refs_: 1 -> 2
            object_ref rb(&b);              // b.refs_: 1 -> 2

            rb = ra;                        // rb releases b, acquires a
            BOOST_TEST_EQ(rb.get(), &a);
            BOOST_TEST_EQ(
                a.refs_.load(), 3u);         // service + ra + rb
            release(&b);                    // b's last (service) ref
            BOOST_TEST_EQ(hits_b, 1);        // retired: rb no longer holds it

            // Deliberate self-assignment: must not release before
            // acquiring. Laundered through a named alias rather than
            // `rb = rb;` -- clang's -Wself-assign-overloaded flags the
            // literal spelling even when it is intentional.
            object_ref& rb_alias = rb;
            rb                   = rb_alias;
            BOOST_TEST_EQ(rb.get(), &a);
            BOOST_TEST_EQ(a.refs_.load(), 3u);
            BOOST_TEST_EQ(hits_a, 0);        // no spurious retire

            ra.reset();                     // a.refs_: 3 -> 2
            release(&a);                    // service ref: 2 -> 1
            BOOST_TEST_EQ(hits_a, 0);
            rb.reset();                     // a.refs_: 1 -> 0 -> retire
            BOOST_TEST_EQ(hits_a, 1);
        }
    }
};

TEST_SUITE(object_ref_test, "boost.corosio.object_ref");

} // namespace boost::corosio::detail
