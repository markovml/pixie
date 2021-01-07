#include <gtest/gtest.h>
#include "src/common/testing/testing.h"

#include "src/stirling/core/types.h"

using ::pl::types::DataType;
using ::pl::types::PatternType;
using ::pl::types::SemanticType;

using ::testing::IsEmpty;
using ::testing::Pair;
using ::testing::UnorderedElementsAre;

namespace pl {
namespace stirling {

TEST(DataElementTest, data_element_proto_getters_test) {
  DataElement element("user_percentage", "", DataType::FLOAT64, SemanticType::ST_NONE,
                      PatternType::METRIC_GAUGE);

  EXPECT_EQ("user_percentage", std::string(element.name().data()));
  EXPECT_EQ(DataType::FLOAT64, element.type());

  stirlingpb::Element element_pb;
  element_pb = element.ToProto();
  EXPECT_EQ("user_percentage", element_pb.name());
  EXPECT_EQ(DataType::FLOAT64, element_pb.type());
}

TEST(DataTableSchemaTest, table_schema_proto_getters_test) {
  enum class BEnum : int64_t { kLow, kMed, kHigh = 99 };
  const std::map<int64_t, std::string_view> enum_decoder = EnumDefToMap<BEnum>();
  DataElement elements[] = {
      {"time_", "", DataType::TIME64NS, SemanticType::ST_NONE, PatternType::METRIC_COUNTER},
      {"a", "", DataType::INT64, SemanticType::ST_NONE, PatternType::GENERAL},
      {"b", "", DataType::INT64, SemanticType::ST_NONE, PatternType::GENERAL_ENUM, &enum_decoder},
      {"c", "", DataType::INT64, SemanticType::ST_NONE, PatternType::GENERAL},
      {"d", "", DataType::INT64, SemanticType::ST_NONE, PatternType::GENERAL},
      {"e", "", DataType::FLOAT64, SemanticType::ST_NONE, PatternType::GENERAL},
  };
  auto table_schema = DataTableSchema("table", elements);

  EXPECT_EQ(1, table_schema.ColIndex("a"));
  EXPECT_EQ("a", table_schema.ColName(1));
  EXPECT_EQ("table", table_schema.name());
  EXPECT_EQ(false, table_schema.tabletized());
  EXPECT_EQ(6, table_schema.elements().size());
  EXPECT_EQ("c", table_schema.elements()[3].name());
  EXPECT_EQ(nullptr, table_schema.elements()[1].decoder());
  EXPECT_NE(nullptr, table_schema.elements()[2].decoder());

  stirlingpb::TableSchema table_schema_pb;
  table_schema_pb = table_schema.ToProto();
  EXPECT_EQ("table", table_schema_pb.name());
  EXPECT_EQ(false, table_schema_pb.tabletized());
  EXPECT_EQ(6, table_schema_pb.elements().size());
  EXPECT_EQ("c", table_schema_pb.elements(3).name());
  EXPECT_THAT(table_schema_pb.elements(3).decoder(), IsEmpty());
  EXPECT_THAT(table_schema_pb.elements(2).decoder(),
              UnorderedElementsAre(Pair(0, "kLow"), Pair(1, "kMed"), Pair(99, "kHigh")));
}

TEST(DynamicDataTableSchemaTest, Create) {
  BackedDataElements columns(3);
  columns.emplace_back("a", "this is column a", types::DataType::INT64);
  columns.emplace_back("b", "this is column b", types::DataType::STRING);
  columns.emplace_back("c", "this is column c", types::DataType::INT64);

  auto data_table_schema = DynamicDataTableSchema::Create("out_table", std::move(columns));

  const DataTableSchema& table_schema = data_table_schema->Get();

  EXPECT_EQ(table_schema.name(), "out_table");
  ASSERT_EQ(table_schema.elements().size(), 3);
  EXPECT_EQ(table_schema.tabletized(), false);
  EXPECT_EQ(table_schema.elements()[0].type(), DataType::INT64);
  EXPECT_EQ(table_schema.elements()[1].type(), DataType::STRING);
  EXPECT_EQ(table_schema.elements()[2].type(), DataType::INT64);
  EXPECT_EQ(table_schema.elements()[0].name(), "a");
  EXPECT_EQ(table_schema.elements()[1].name(), "b");
  EXPECT_EQ(table_schema.elements()[2].name(), "c");
  EXPECT_EQ(table_schema.elements()[0].desc(), "this is column a");
  EXPECT_EQ(table_schema.elements()[1].desc(), "this is column b");
  EXPECT_EQ(table_schema.elements()[2].desc(), "this is column c");
}

}  // namespace stirling
}  // namespace pl