// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <string>

#include <gtest/gtest.h>

#include "arrow/array.h"
// TODO ipc shouldn't be included here
#include "arrow/ipc/test_common.h"
#include "arrow/testing/gtest_util.h"
#include "arrow/testing/util.h"
#include "arrow/type.h"
#include "arrow/util/checked_cast.h"

namespace arrow {

using internal::checked_cast;

TEST(TestUnionArray, TestSliceEquals) {
  std::shared_ptr<RecordBatch> batch;
  ASSERT_OK(ipc::test::MakeUnion(&batch));

  auto CheckUnion = [](std::shared_ptr<Array> array) {
    const int64_t size = array->length();
    std::shared_ptr<Array> slice, slice2;
    slice = array->Slice(2);
    ASSERT_EQ(size - 2, slice->length());

    slice2 = array->Slice(2);
    ASSERT_EQ(size - 2, slice->length());

    ASSERT_TRUE(slice->Equals(slice2));
    ASSERT_TRUE(array->RangeEquals(2, array->length(), 0, slice));

    // Chained slices
    slice2 = array->Slice(1)->Slice(1);
    ASSERT_TRUE(slice->Equals(slice2));

    slice = array->Slice(1, 5);
    slice2 = array->Slice(1, 5);
    ASSERT_EQ(5, slice->length());

    ASSERT_TRUE(slice->Equals(slice2));
    ASSERT_TRUE(array->RangeEquals(1, 6, 0, slice));

    AssertZeroPadded(*array);
    TestInitialized(*array);
  };

  CheckUnion(batch->column(1));
  CheckUnion(batch->column(2));
}

TEST(TestSparseUnionArray, Validate) {
  auto a = ArrayFromJSON(int32(), "[4, 5]");
  auto type = union_({field("a", int32())}, UnionMode::SPARSE);
  auto children = std::vector<std::shared_ptr<Array>>{a};
  auto type_ids_array = ArrayFromJSON(int8(), "[0, 0, 0]");
  auto type_ids = type_ids_array->data()->buffers[1];

  auto arr = std::make_shared<UnionArray>(type, 2, children, type_ids);
  ASSERT_OK(arr->ValidateFull());
  arr = std::make_shared<UnionArray>(type, 1, children, type_ids, nullptr, nullptr, 0,
                                     /*offset=*/1);
  ASSERT_OK(arr->ValidateFull());
  arr = std::make_shared<UnionArray>(type, 0, children, type_ids, nullptr, nullptr, 0,
                                     /*offset=*/2);
  ASSERT_OK(arr->ValidateFull());

  // Length + offset < child length, but it's ok
  arr = std::make_shared<UnionArray>(type, 1, children, type_ids, nullptr, nullptr, 0,
                                     /*offset=*/0);
  ASSERT_OK(arr->ValidateFull());

  // Length + offset > child length
  arr = std::make_shared<UnionArray>(type, 1, children, type_ids, nullptr, nullptr, 0,
                                     /*offset=*/2);
  ASSERT_RAISES(Invalid, arr->ValidateFull());

  // Offset > child length
  arr = std::make_shared<UnionArray>(type, 0, children, type_ids, nullptr, nullptr, 0,
                                     /*offset=*/3);
  ASSERT_RAISES(Invalid, arr->ValidateFull());
}

// -------------------------------------------------------------------------
// Tests for MakeDense and MakeSparse

class TestUnionArrayFactories : public ::testing::Test {
 public:
  void SetUp() {
    pool_ = default_memory_pool();
    type_codes_ = {1, 2, 4, 8};
    ArrayFromVector<Int8Type>({0, 1, 2, 0, 1, 3, 2, 0, 2, 1}, &type_ids_);
    ArrayFromVector<Int8Type>({1, 2, 4, 1, 2, 8, 4, 1, 4, 2}, &logical_type_ids_);
    ArrayFromVector<Int8Type>({1, 2, 4, 1, -2, 8, 4, 1, 4, 2}, &invalid_type_ids1_);
    ArrayFromVector<Int8Type>({1, 2, 4, 1, 3, 8, 4, 1, 4, 2}, &invalid_type_ids2_);
  }

  void CheckUnionArray(const UnionArray& array, UnionMode::type mode,
                       const std::vector<std::string>& field_names,
                       const std::vector<int8_t>& type_codes) {
    ASSERT_EQ(mode, array.mode());
    CheckFieldNames(array, field_names);
    CheckTypeCodes(array, type_codes);
    const auto& type_ids = checked_cast<const Int8Array&>(*type_ids_);
    for (int64_t i = 0; i < type_ids.length(); ++i) {
      ASSERT_EQ(array.child_id(i), type_ids.Value(i));
    }
  }

  void CheckFieldNames(const UnionArray& array, const std::vector<std::string>& names) {
    const auto& type = checked_cast<const UnionType&>(*array.type());
    ASSERT_EQ(type.num_children(), names.size());
    for (int i = 0; i < type.num_children(); ++i) {
      ASSERT_EQ(type.child(i)->name(), names[i]);
    }
  }

  void CheckTypeCodes(const UnionArray& array, const std::vector<int8_t>& codes) {
    const auto& type = checked_cast<const UnionType&>(*array.type());
    ASSERT_EQ(codes, type.type_codes());
  }

 protected:
  MemoryPool* pool_;
  std::vector<int8_t> type_codes_;
  std::shared_ptr<Array> type_ids_;
  std::shared_ptr<Array> logical_type_ids_;
  std::shared_ptr<Array> invalid_type_ids1_;
  std::shared_ptr<Array> invalid_type_ids2_;
};

TEST_F(TestUnionArrayFactories, TestMakeDense) {
  std::shared_ptr<Array> value_offsets;
  ArrayFromVector<Int32Type, int32_t>({1, 0, 0, 0, 1, 0, 1, 2, 1, 2}, &value_offsets);

  auto children = std::vector<std::shared_ptr<Array>>(4);
  ArrayFromVector<StringType, std::string>({"abc", "def", "xyz"}, &children[0]);
  ArrayFromVector<UInt8Type>({10, 20, 30}, &children[1]);
  ArrayFromVector<DoubleType>({1.618, 2.718, 3.142}, &children[2]);
  ArrayFromVector<Int8Type>({-12}, &children[3]);

  std::vector<std::string> field_names = {"str", "int1", "real", "int2"};

  std::shared_ptr<Array> result;
  const UnionArray* union_array;

  // without field names and type codes
  ASSERT_OK(UnionArray::MakeDense(*type_ids_, *value_offsets, children, &result));
  ASSERT_OK(result->ValidateFull());
  union_array = checked_cast<const UnionArray*>(result.get());
  CheckUnionArray(*union_array, UnionMode::DENSE, {"0", "1", "2", "3"}, {0, 1, 2, 3});

  // with field name
  ASSERT_RAISES(Invalid, UnionArray::MakeDense(*type_ids_, *value_offsets, children,
                                               {"one"}, &result));
  ASSERT_OK(
      UnionArray::MakeDense(*type_ids_, *value_offsets, children, field_names, &result));
  ASSERT_OK(result->ValidateFull());
  union_array = checked_cast<const UnionArray*>(result.get());
  CheckUnionArray(*union_array, UnionMode::DENSE, field_names, {0, 1, 2, 3});

  // with type codes
  ASSERT_RAISES(Invalid,
                UnionArray::MakeDense(*logical_type_ids_, *value_offsets, children,
                                      std::vector<int8_t>{0}, &result));
  ASSERT_OK(UnionArray::MakeDense(*logical_type_ids_, *value_offsets, children,
                                  type_codes_, &result));
  ASSERT_OK(result->ValidateFull());
  union_array = checked_cast<const UnionArray*>(result.get());
  CheckUnionArray(*union_array, UnionMode::DENSE, {"0", "1", "2", "3"}, type_codes_);

  // with field names and type codes
  ASSERT_RAISES(Invalid, UnionArray::MakeDense(*logical_type_ids_, *value_offsets,
                                               children, {"one"}, type_codes_, &result));
  ASSERT_OK(UnionArray::MakeDense(*logical_type_ids_, *value_offsets, children,
                                  field_names, type_codes_, &result));
  ASSERT_OK(result->ValidateFull());
  union_array = checked_cast<const UnionArray*>(result.get());
  CheckUnionArray(*union_array, UnionMode::DENSE, field_names, type_codes_);

  // Invalid type codes
  ASSERT_OK(UnionArray::MakeDense(*invalid_type_ids1_, *value_offsets, children,
                                  type_codes_, &result));
  ASSERT_RAISES(Invalid, result->ValidateFull());
  ASSERT_OK(UnionArray::MakeDense(*invalid_type_ids2_, *value_offsets, children,
                                  type_codes_, &result));
  ASSERT_RAISES(Invalid, result->ValidateFull());

  // Invalid offsets
  std::shared_ptr<Array> invalid_offsets;
  ArrayFromVector<Int32Type, int32_t>({1, 0, 0, 0, 1, 1, 1, 2, 1, 2}, &invalid_offsets);
  ASSERT_OK(UnionArray::MakeDense(*type_ids_, *invalid_offsets, children, &result));
  ASSERT_RAISES(Invalid, result->ValidateFull());
  ArrayFromVector<Int32Type, int32_t>({1, 0, 0, 0, 1, -1, 1, 2, 1, 2}, &invalid_offsets);
  ASSERT_OK(UnionArray::MakeDense(*type_ids_, *invalid_offsets, children, &result));
  ASSERT_RAISES(Invalid, result->ValidateFull());
}

TEST_F(TestUnionArrayFactories, TestMakeSparse) {
  auto children = std::vector<std::shared_ptr<Array>>(4);
  ArrayFromVector<StringType, std::string>(
      {"abc", "", "", "def", "", "", "", "xyz", "", ""}, &children[0]);
  ArrayFromVector<UInt8Type>({0, 10, 0, 0, 20, 0, 0, 0, 0, 30}, &children[1]);
  ArrayFromVector<DoubleType>({0.0, 0.0, 1.618, 0.0, 0.0, 0.0, 2.718, 0.0, 3.142, 0.0},
                              &children[2]);
  ArrayFromVector<Int8Type>({0, 0, 0, 0, 0, -12, 0, 0, 0, 0}, &children[3]);

  std::vector<std::string> field_names = {"str", "int1", "real", "int2"};

  std::shared_ptr<Array> result;

  // without field names and type codes
  ASSERT_OK(UnionArray::MakeSparse(*type_ids_, children, &result));
  ASSERT_OK(result->ValidateFull());
  CheckUnionArray(checked_cast<UnionArray&>(*result), UnionMode::SPARSE,
                  {"0", "1", "2", "3"}, {0, 1, 2, 3});

  // with field names
  ASSERT_RAISES(Invalid, UnionArray::MakeSparse(*type_ids_, children, {"one"}, &result));
  ASSERT_OK(UnionArray::MakeSparse(*type_ids_, children, field_names, &result));
  ASSERT_OK(result->ValidateFull());
  CheckUnionArray(checked_cast<UnionArray&>(*result), UnionMode::SPARSE, field_names,
                  {0, 1, 2, 3});

  // with type codes
  ASSERT_RAISES(Invalid, UnionArray::MakeSparse(*logical_type_ids_, children,
                                                std::vector<int8_t>{0}, &result));
  ASSERT_OK(UnionArray::MakeSparse(*logical_type_ids_, children, type_codes_, &result));
  ASSERT_OK(result->ValidateFull());
  CheckUnionArray(checked_cast<UnionArray&>(*result), UnionMode::SPARSE,
                  {"0", "1", "2", "3"}, type_codes_);

  // with field names and type codes
  ASSERT_RAISES(Invalid, UnionArray::MakeSparse(*logical_type_ids_, children, {"one"},
                                                type_codes_, &result));
  ASSERT_OK(UnionArray::MakeSparse(*logical_type_ids_, children, field_names, type_codes_,
                                   &result));
  ASSERT_OK(result->ValidateFull());
  CheckUnionArray(checked_cast<UnionArray&>(*result), UnionMode::SPARSE, field_names,
                  type_codes_);

  // Invalid type codes
  ASSERT_OK(UnionArray::MakeSparse(*invalid_type_ids1_, children, type_codes_, &result));
  ASSERT_RAISES(Invalid, result->ValidateFull());
  ASSERT_OK(UnionArray::MakeSparse(*invalid_type_ids2_, children, type_codes_, &result));
  ASSERT_RAISES(Invalid, result->ValidateFull());

  // Invalid child length
  ArrayFromVector<Int8Type>({0, 0, 0, 0, 0, -12, 0, 0, 0}, &children[3]);
  ASSERT_RAISES(Invalid, UnionArray::MakeSparse(*type_ids_, children, &result));
}

template <typename B>
class UnionBuilderTest : public ::testing::Test {
 public:
  int8_t I8 = 8, STR = 13, DBL = 7;

  virtual void AppendInt(int8_t i) {
    expected_types_vector.push_back(I8);
    ASSERT_OK(union_builder->Append(I8));
    ASSERT_OK(i8_builder->Append(i));
  }

  virtual void AppendString(const std::string& str) {
    expected_types_vector.push_back(STR);
    ASSERT_OK(union_builder->Append(STR));
    ASSERT_OK(str_builder->Append(str));
  }

  virtual void AppendDouble(double dbl) {
    expected_types_vector.push_back(DBL);
    ASSERT_OK(union_builder->Append(DBL));
    ASSERT_OK(dbl_builder->Append(dbl));
  }

  void AppendBasics() {
    AppendInt(33);
    AppendString("abc");
    AppendDouble(1.0);
    AppendDouble(-1.0);
    AppendString("");
    AppendInt(10);
    AppendString("def");
    AppendInt(-10);
    AppendDouble(0.5);
    ASSERT_OK(union_builder->Finish(&actual));
    ArrayFromVector<Int8Type, uint8_t>(expected_types_vector, &expected_types);
  }

  void AppendInferred() {
    I8 = union_builder->AppendChild(i8_builder, "i8");
    ASSERT_EQ(I8, 0);
    AppendInt(33);
    AppendInt(10);

    STR = union_builder->AppendChild(str_builder, "str");
    ASSERT_EQ(STR, 1);
    AppendString("abc");
    AppendString("");
    AppendString("def");
    AppendInt(-10);

    DBL = union_builder->AppendChild(dbl_builder, "dbl");
    ASSERT_EQ(DBL, 2);
    AppendDouble(1.0);
    AppendDouble(-1.0);
    AppendDouble(0.5);
    ASSERT_OK(union_builder->Finish(&actual));
    ArrayFromVector<Int8Type, uint8_t>(expected_types_vector, &expected_types);

    ASSERT_EQ(I8, 0);
    ASSERT_EQ(STR, 1);
    ASSERT_EQ(DBL, 2);
  }

  void AppendListOfInferred(std::shared_ptr<ListArray>* actual) {
    ListBuilder list_builder(default_memory_pool(), union_builder);

    ASSERT_OK(list_builder.Append());
    I8 = union_builder->AppendChild(i8_builder, "i8");
    ASSERT_EQ(I8, 0);
    AppendInt(10);

    ASSERT_OK(list_builder.Append());
    STR = union_builder->AppendChild(str_builder, "str");
    ASSERT_EQ(STR, 1);
    AppendString("abc");
    AppendInt(-10);

    ASSERT_OK(list_builder.Append());
    DBL = union_builder->AppendChild(dbl_builder, "dbl");
    ASSERT_EQ(DBL, 2);
    AppendDouble(0.5);

    ASSERT_OK(list_builder.Finish(actual));
    ArrayFromVector<Int8Type, uint8_t>(expected_types_vector, &expected_types);
  }

  std::vector<uint8_t> expected_types_vector;
  std::shared_ptr<Array> expected_types;
  std::shared_ptr<Int8Builder> i8_builder = std::make_shared<Int8Builder>();
  std::shared_ptr<StringBuilder> str_builder = std::make_shared<StringBuilder>();
  std::shared_ptr<DoubleBuilder> dbl_builder = std::make_shared<DoubleBuilder>();
  std::shared_ptr<B> union_builder = std::make_shared<B>(default_memory_pool());
  std::shared_ptr<UnionArray> actual;
};

class DenseUnionBuilderTest : public UnionBuilderTest<DenseUnionBuilder> {};
class SparseUnionBuilderTest : public UnionBuilderTest<SparseUnionBuilder> {
 public:
  using Base = UnionBuilderTest<SparseUnionBuilder>;

  void AppendInt(int8_t i) override {
    Base::AppendInt(i);
    ASSERT_OK(str_builder->AppendNull());
    ASSERT_OK(dbl_builder->AppendNull());
  }

  void AppendString(const std::string& str) override {
    Base::AppendString(str);
    ASSERT_OK(i8_builder->AppendNull());
    ASSERT_OK(dbl_builder->AppendNull());
  }

  void AppendDouble(double dbl) override {
    Base::AppendDouble(dbl);
    ASSERT_OK(i8_builder->AppendNull());
    ASSERT_OK(str_builder->AppendNull());
  }
};

TEST_F(DenseUnionBuilderTest, Basics) {
  union_builder.reset(new DenseUnionBuilder(
      default_memory_pool(), {i8_builder, str_builder, dbl_builder},
      union_({field("i8", int8()), field("str", utf8()), field("dbl", float64())},
             {I8, STR, DBL}, UnionMode::DENSE)));
  AppendBasics();

  auto expected_i8 = ArrayFromJSON(int8(), "[33, 10, -10]");
  auto expected_str = ArrayFromJSON(utf8(), R"(["abc", "", "def"])");
  auto expected_dbl = ArrayFromJSON(float64(), "[1.0, -1.0, 0.5]");

  auto expected_offsets = ArrayFromJSON(int32(), "[0, 0, 0, 1, 1, 1, 2, 2, 2]");

  std::shared_ptr<Array> expected;
  ASSERT_OK(UnionArray::MakeDense(*expected_types, *expected_offsets,
                                  {expected_i8, expected_str, expected_dbl},
                                  {"i8", "str", "dbl"}, {I8, STR, DBL}, &expected));

  ASSERT_EQ(expected->type()->ToString(), actual->type()->ToString());
  ASSERT_ARRAYS_EQUAL(*expected, *actual);
}

TEST_F(DenseUnionBuilderTest, InferredType) {
  AppendInferred();

  auto expected_i8 = ArrayFromJSON(int8(), "[33, 10, -10]");
  auto expected_str = ArrayFromJSON(utf8(), R"(["abc", "", "def"])");
  auto expected_dbl = ArrayFromJSON(float64(), "[1.0, -1.0, 0.5]");

  auto expected_offsets = ArrayFromJSON(int32(), "[0, 1, 0, 1, 2, 2, 0, 1, 2]");

  std::shared_ptr<Array> expected;
  ASSERT_OK(UnionArray::MakeDense(*expected_types, *expected_offsets,
                                  {expected_i8, expected_str, expected_dbl},
                                  {"i8", "str", "dbl"}, {I8, STR, DBL}, &expected));

  ASSERT_EQ(expected->type()->ToString(), actual->type()->ToString());
  ASSERT_ARRAYS_EQUAL(*expected, *actual);
}

TEST_F(DenseUnionBuilderTest, ListOfInferredType) {
  std::shared_ptr<ListArray> actual;
  AppendListOfInferred(&actual);

  auto expected_type =
      list(union_({field("i8", int8()), field("str", utf8()), field("dbl", float64())},
                  {I8, STR, DBL}, UnionMode::DENSE));
  ASSERT_EQ(expected_type->ToString(), actual->type()->ToString());
}

TEST_F(SparseUnionBuilderTest, Basics) {
  union_builder.reset(new SparseUnionBuilder(
      default_memory_pool(), {i8_builder, str_builder, dbl_builder},
      union_({field("i8", int8()), field("str", utf8()), field("dbl", float64())},
             {I8, STR, DBL}, UnionMode::SPARSE)));

  AppendBasics();

  auto expected_i8 =
      ArrayFromJSON(int8(), "[33, null, null, null, null, 10, null, -10, null]");
  auto expected_str =
      ArrayFromJSON(utf8(), R"([null, "abc", null, null, "",  null, "def", null, null])");
  auto expected_dbl =
      ArrayFromJSON(float64(), "[null, null, 1.0, -1.0, null, null, null, null, 0.5]");

  std::shared_ptr<Array> expected;
  ASSERT_OK(UnionArray::MakeSparse(*expected_types,
                                   {expected_i8, expected_str, expected_dbl},
                                   {"i8", "str", "dbl"}, {I8, STR, DBL}, &expected));

  ASSERT_EQ(expected->type()->ToString(), actual->type()->ToString());
  ASSERT_ARRAYS_EQUAL(*expected, *actual);
}

TEST_F(SparseUnionBuilderTest, InferredType) {
  AppendInferred();

  auto expected_i8 =
      ArrayFromJSON(int8(), "[33, 10, null, null, null, -10, null, null, null]");
  auto expected_str =
      ArrayFromJSON(utf8(), R"([null, null, "abc", "", "def",  null, null, null, null])");
  auto expected_dbl =
      ArrayFromJSON(float64(), "[null, null, null, null, null, null, 1.0, -1.0, 0.5]");

  std::shared_ptr<Array> expected;
  ASSERT_OK(UnionArray::MakeSparse(*expected_types,
                                   {expected_i8, expected_str, expected_dbl},
                                   {"i8", "str", "dbl"}, {I8, STR, DBL}, &expected));

  ASSERT_EQ(expected->type()->ToString(), actual->type()->ToString());
  ASSERT_ARRAYS_EQUAL(*expected, *actual);
}

TEST_F(SparseUnionBuilderTest, StructWithUnion) {
  auto union_builder = std::make_shared<SparseUnionBuilder>(default_memory_pool());
  StructBuilder builder(struct_({field("u", union_builder->type())}),
                        default_memory_pool(), {union_builder});
  ASSERT_EQ(union_builder->AppendChild(std::make_shared<Int32Builder>(), "i"), 0);
  ASSERT_TRUE(
      builder.type()->Equals(struct_({field("u", union_({field("i", int32())}, {0}))})));
}

}  // namespace arrow
