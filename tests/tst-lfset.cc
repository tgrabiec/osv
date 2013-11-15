#define BOOST_TEST_MODULE tst-lfset

#include <boost/test/unit_test.hpp>
#include <iostream>

#include "lockfree/hash_set.hh"
#include "lockfree/hashing.hh"
#include "debug.hh"

static lockfree::hash_set<int> make_int_set(int size) {
	return lockfree::hash_set<int>(size,
		[] (const int& elem) -> int {
			return elem;
		},
		[] (const int& e1, const int& e2) -> bool {
			return e1 == e2;
		});
}

BOOST_AUTO_TEST_CASE(test_basic_operations)
{
	int v1 = 1;
	int v2 = 2;
	int v3 = 3;

	auto set = make_int_set(10);

	BOOST_TEST_MESSAGE("Populating the set");
	BOOST_REQUIRE(set.add_if_absent(&v1) == nullptr);
	BOOST_REQUIRE(set.add_if_absent(&v2) == nullptr);
	BOOST_REQUIRE(set.add_if_absent(&v3) == nullptr);

	BOOST_TEST_MESSAGE("Testing insertion of the same elements");
	BOOST_REQUIRE(set.add_if_absent(&v1) == &v1);
	BOOST_REQUIRE(set.add_if_absent(&v3) == &v3);

	BOOST_TEST_MESSAGE("Testing insertion of different but equal elements");
	int v1_ = 1;
	int v2_ = 2;
	BOOST_REQUIRE(set.add_if_absent(&v1_) == &v1);
	BOOST_REQUIRE(set.add_if_absent(&v2_) == &v2);

	BOOST_TEST_MESSAGE("done");

	size_t h = hash<void*>(nullptr);
}

// BOOST_AUTO_TEST_CASE(test_overflow)
// {
// 	int v1 = 1;
// 	int v2 = 2;
// 	int v3 = 3;

// 	auto set = make_int_set(3);

// 	set.add_if_absent(&v1);
// 	set.add_if_absent(&v2);
// 	try {
// 		set.add_if_absent(&v3);
// 		BOOST_REQUIRE_MESSAGE(false, "Should have thrown");
// 	} catch (lockfree::table_full e) {
// 		BOOST_TEST_MESSAGE("Exception caught: " + std::string(e.what()));
// 	}
// }