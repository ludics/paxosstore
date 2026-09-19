#include "utils/light_list_modern.h"

#include "gtest/gtest.h"

struct Foo {
  int bar;
  certain::LightListHook<Foo> test_list_entry;

  Foo() = delete;

  Foo(int _bar) : bar(_bar) {}
};

typedef certain::LightList<Foo, &Foo::test_list_entry> FooList;

TEST(LightListModernTest, Basic) {
  FooList test_list;

  Foo e1(1);
  Foo e2(2);
  Foo e3(3);

  ASSERT_FALSE(FooList::contains(&e1));
  test_list.insert_head(&e1);
  ASSERT_TRUE(FooList::contains(&e1));
  test_list.remove(&e1);
  ASSERT_FALSE(FooList::contains(&e1));

  test_list.insert_head(&e1);
  test_list.insert_head(&e2);
  test_list.insert_head(&e3);

  ASSERT_FALSE(test_list.empty());
  ASSERT_EQ(test_list.first()->bar, 3);
  ASSERT_TRUE(FooList::contains(&e3));
  test_list.remove(&e3);
  ASSERT_FALSE(FooList::contains(&e3));

  ASSERT_EQ(test_list.first()->bar, 2);
  ASSERT_TRUE(FooList::contains(&e2));
  test_list.remove(&e2);
  ASSERT_FALSE(FooList::contains(&e2));

  ASSERT_EQ(test_list.first()->bar, 1);
  ASSERT_TRUE(FooList::contains(&e1));
  test_list.remove(&e1);
  ASSERT_FALSE(FooList::contains(&e1));

  ASSERT_TRUE(test_list.empty());
}

TEST(LightListModernTest, InsertTailAndIterate) {
  FooList test_list;
  Foo e1(1);
  Foo e2(2);
  Foo e3(3);

  test_list.insert_tail(&e1);
  test_list.insert_tail(&e2);
  test_list.insert_tail(&e3);

  ASSERT_EQ(test_list.first()->bar, 1);
  ASSERT_EQ(test_list.last()->bar, 3);

  int sum = 0;
  int count = 0;
  for (Foo* p = test_list.first(); !test_list.is_sentinel(p);
       p = test_list.next(p)) {
    sum += p->bar;
    ++count;
  }
  ASSERT_EQ(count, 3);
  ASSERT_EQ(sum, 6);

  test_list.remove(&e2);
  ASSERT_EQ(test_list.first()->bar, 1);
  ASSERT_EQ(test_list.last()->bar, 3);
  ASSERT_EQ(test_list.next(&e1)->bar, 3);
}

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
