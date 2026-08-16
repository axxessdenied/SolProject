#include <sol/core/hash.hpp>

#include <sol/test/test.hpp>

#include <cstdint>
#include <string>

using sol::core::fnv1a;
using sol::core::hashCombine;

SOL_TEST(hash_matches_known_fnv1a_vectors)
{
    // Reference values for 64-bit FNV-1a; a drift here would silently change
    // every id built on top of it.
    SOL_CHECK(fnv1a("") == 14695981039346656037ull);
    SOL_CHECK(fnv1a("a") == 12638187200555641996ull);
    SOL_CHECK(fnv1a("foobar") == 9625390261332436968ull);
}

SOL_TEST(hash_is_usable_at_compile_time)
{
    static_assert(fnv1a("compile") == fnv1a("compile"));
    static_assert(fnv1a("compile") != fnv1a("runtime"));
    static_assert(hashCombine(fnv1a("panel"), 3) != hashCombine(fnv1a("panel"), 4));
    SOL_CHECK(true);
}

SOL_TEST(hash_separates_similar_inputs)
{
    SOL_CHECK(fnv1a("Accept") != fnv1a("accept"));
    SOL_CHECK(fnv1a("ab") != fnv1a("ba"));
    SOL_CHECK(fnv1a("a") != fnv1a("aa"));

    // Embedded nulls are content, not terminators.
    const std::string withNull("a\0b", 3);
    SOL_CHECK(fnv1a(withNull) != fnv1a("a"));
}

SOL_TEST(hash_seeding_chains_like_a_prefix)
{
    // Seeding with a previous hash must equal hashing the concatenation, so an
    // id stack can be built incrementally without keeping the strings around.
    SOL_CHECK(fnv1a("bar", fnv1a("foo")) == fnv1a("foobar"));
    SOL_CHECK(fnv1a("", fnv1a("foo")) == fnv1a("foo"));
}

SOL_TEST(hash_combine_orders_matter)
{
    const std::uint64_t base = fnv1a("row");
    SOL_CHECK(hashCombine(base, 1) != hashCombine(base, 2));
    SOL_CHECK(hashCombine(hashCombine(base, 1), 2) != hashCombine(hashCombine(base, 2), 1));
    SOL_CHECK(hashCombine(base, 0) != base);

    // Values differing only in a high byte must still separate.
    SOL_CHECK(hashCombine(base, 0x0100000000000000ull) != hashCombine(base, 0));
}
