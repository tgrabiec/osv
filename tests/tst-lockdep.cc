#define BOOST_TEST_MODULE tst-lockdep

#include <boost/test/unit_test.hpp>
#include <iostream>

#include "osv/lockdep.hh"
#include "debug.hh"

struct my_lock {
    lockdep::lock_hook<my_lock> hook;
};

typedef lockdep::simple_set<my_lock, &my_lock::hook> set_t;

static void assert_holds(set_t& set, std::vector<my_lock*> locks)
{
    auto i = set.first();
    for (auto lock_ptr : locks) {
        BOOST_REQUIRE(i == lock_ptr);
        BOOST_REQUIRE(i != nullptr);
        i = set.next(i);
    }

    BOOST_REQUIRE(i == nullptr);
}

BOOST_AUTO_TEST_CASE(test_lock_set_operations)
{
    set_t set;

    my_lock l1;
    my_lock l2;
    my_lock l3;

    set.add(&l1);
    set.add(&l2);
    set.add(&l3);

    set.remove(&l2);

    assert_holds(set, {&l3, &l1});

    set.remove(&l1);

    assert_holds(set, {&l3});

    set.remove(&l3);

    assert_holds(set, {});
}
