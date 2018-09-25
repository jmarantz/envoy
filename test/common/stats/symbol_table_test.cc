#include <string>

#include "common/stats/symbol_table_impl.h"

#include "test/test_common/logging.h"
#include "test/test_common/utility.h"

#include "gtest/gtest.h"

namespace Envoy {
namespace Stats {

class StatNameTest : public testing::Test {
protected:
  //SymbolVec getSymbols(StatName& stat_name) { return stat_name.symbolVec(); }
  //std::string decodeSymbolVec(const SymbolVec& symbol_vec) { return table_.decode(symbol_vec); }
  Symbol monotonicCounter() { return table_.monotonicCounter(); }
  std::string EncodeDecode(absl::string_view stat_name) {
    return makeStat(stat_name).toString(table_);
  }

  StatName makeStat(absl::string_view name) {
    SymbolVec symbol_vec = table_.encode(name);
    uint8_t* buffer = new uint8_t[StatName::size(symbol_vec)];
    stat_name_buffers_.emplace_back(std::unique_ptr<uint8_t[]>(buffer));
    StatName stat_name;
    stat_name.init(symbol_vec, buffer);
    return stat_name;
  }

  SymbolTable table_;

  std::vector<std::unique_ptr<uint8_t[]>> stat_name_buffers_;
};

TEST_F(StatNameTest, TestArbitrarySymbolRoundtrip) {
  const std::vector<std::string> stat_names = {"", " ", "  ", ",", "\t", "$", "%", "`", "."};
  for (auto stat_name : stat_names) {
    EXPECT_EQ(stat_name, EncodeDecode(stat_name));
  }
}

TEST_F(StatNameTest, TestMillionSymbolsRoundtrip) {
  for (int i = 0; i < 1*1000*1000; ++i) {
    const std::string stat_name = absl::StrCat("symbol_", i);
    EXPECT_EQ(stat_name, EncodeDecode(stat_name));
  }
}

TEST_F(StatNameTest, TestUnusualDelimitersRoundtrip) {
  const std::vector<std::string> stat_names = {".",    "..",    "...",    "foo",    "foo.",
                                               ".foo", ".foo.", ".foo..", "..foo.", "..foo.."};
  for (auto stat_name : stat_names) {
    EXPECT_EQ(stat_name, EncodeDecode(stat_name));
  }
}

TEST_F(StatNameTest, TestSuccessfulDoubleLookup) {
  StatName stat_name_1(makeStat("foo.bar.baz"));
  StatName stat_name_2(makeStat("foo.bar.baz"));
  EXPECT_EQ(stat_name_1, stat_name_2);
}

TEST_F(StatNameTest, TestSuccessfulDecode) {
  std::string stat_name = "foo.bar.baz";
  StatName stat_name_1(makeStat(stat_name));
  StatName stat_name_2(makeStat(stat_name));
  EXPECT_EQ(stat_name_1.toString(table_), stat_name_2.toString(table_));
  EXPECT_EQ(stat_name_1.toString(table_), stat_name);
}

/*
TEST_F(StatNameTest, TestBadDecodes) {
  {
    // If a symbol doesn't exist, decoding it should trigger an ASSERT() and crash.
    SymbolVec bad_symbol_vec = {0};
    EXPECT_DEATH(decodeSymbolVec(bad_symbol_vec), "");
  }

  {
    StatName stat_name_1 = makeStat("foo");
    SymbolVec vec_1 = getSymbols(stat_name_1);
    // Decoding a symbol vec that exists is perfectly normal...
    EXPECT_NO_THROW(decodeSymbolVec(vec_1));
    stat_name_1.free(table_);
    // But when the StatName is destroyed, its symbols are as well.
    EXPECT_DEATH(decodeSymbolVec(vec_1), "");
  }
  }*/

TEST_F(StatNameTest, TestDifferentStats) {
  StatName stat_name_1(makeStat("foo.bar"));
  StatName stat_name_2(makeStat("bar.foo"));
  EXPECT_NE(stat_name_1.toString(table_), stat_name_2.toString(table_));
  EXPECT_NE(stat_name_1, stat_name_2);
}

/*
TEST_F(StatNameTest, TestSymbolConsistency) {
  StatName stat_name_1(makeStat("foo.bar"));
  StatName stat_name_2(makeStat("bar.foo"));
  // We expect the encoding of "foo" in one context to be the same as another.
  SymbolVec vec_1 = getSymbols(stat_name_1);
  SymbolVec vec_2 = getSymbols(stat_name_2);
  EXPECT_EQ(vec_1[0], vec_2[1]);
  EXPECT_EQ(vec_2[0], vec_1[1]);
}
*/

/*
TEST_F(StatNameTest, TestSameValueOnPartialFree) {
  // This should hold true for components as well. Since "foo" persists even when "foo.bar" is
  // freed, we expect both instances of "foo" to have the same symbol.
  StatName stat_foo(makeStat("foo"));
  StatName stat_foobar_1(makeStat("foo.bar"));
  SymbolVec stat_foobar_1_symbols = getSymbols(stat_foobar_1);
  stat_foobar_1.free(table_);

  StatName stat_foobar_2(makeStat("foo.bar"));
  SymbolVec stat_foobar_2_symbols = getSymbols(stat_foobar_2);

  EXPECT_EQ(stat_foobar_1_symbols[0],
            stat_foobar_2_symbols[0]); // Both "foo" components have the same symbol,
  // And we have no expectation for the "bar" components, because of the free pool.
}
*/

TEST_F(StatNameTest, FreePoolTest) {
  // To ensure that the free pool is being used, we should be able to cycle through a large number
  // of stats while validating that:
  //   a) the size of the table has not increased, and
  //   b) the monotonically increasing counter has not risen to more than the maximum number of
  //   coexisting symbols during the life of the table.

  {
    StatName stat_1(makeStat("1a"));
    StatName stat_2(makeStat("2a"));
    StatName stat_3(makeStat("3a"));
    StatName stat_4(makeStat("4a"));
    StatName stat_5(makeStat("5a"));
    EXPECT_EQ(monotonicCounter(), 5);
    EXPECT_EQ(table_.size(), 5);
    stat_1.free(table_);
    stat_2.free(table_);
    stat_3.free(table_);
    stat_4.free(table_);
    stat_5.free(table_);
  }
  EXPECT_EQ(monotonicCounter(), 5);
  EXPECT_EQ(table_.size(), 0);

  // These are different strings being encoded, but they should recycle through the same symbols as
  // the stats above.
  makeStat("1b");
  makeStat("2b");
  makeStat("3b");
  makeStat("4b");
  makeStat("5b");
  EXPECT_EQ(monotonicCounter(), 5);
  EXPECT_EQ(table_.size(), 5);

  makeStat("6");
  EXPECT_EQ(monotonicCounter(), 6);
  EXPECT_EQ(table_.size(), 6);
}

TEST_F(StatNameTest, TestShrinkingExpectation) {
  // We expect that as we free stat names, the memory used to store those underlying symbols will
  // be freed.
  // ::size() is a public function, but should only be used for testing.
  size_t table_size_0 = table_.size();

  StatName stat_a(makeStat("a"));
  size_t table_size_1 = table_.size();

  StatName stat_aa(makeStat("a.a"));
  EXPECT_EQ(table_size_1, table_.size());

  StatName stat_ab(makeStat("a.b"));
  size_t table_size_2 = table_.size();

  StatName stat_ac(makeStat("a.c"));
  size_t table_size_3 = table_.size();

  StatName stat_acd(makeStat("a.c.d"));
  size_t table_size_4 = table_.size();

  StatName stat_ace(makeStat("a.c.e"));
  size_t table_size_5 = table_.size();
  EXPECT_GE(table_size_5, table_size_4);

  stat_ace.free(table_);
  EXPECT_EQ(table_size_4, table_.size());

  stat_acd.free(table_);
  EXPECT_EQ(table_size_3, table_.size());

  stat_ac.free(table_);
  EXPECT_EQ(table_size_2, table_.size());

  stat_ab.free(table_);
  EXPECT_EQ(table_size_1, table_.size());

  stat_aa.free(table_);
  EXPECT_EQ(table_size_1, table_.size());

  stat_a.free(table_);
  EXPECT_EQ(table_size_0, table_.size());
}

} // namespace Stats
} // namespace Envoy
