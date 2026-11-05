#include "digest_test_common.h"
#include <ydb/core/scheme/table_digest/table_digest.h>

#include <yql/essentials/types/binary_json/write.h>
#include <yql/essentials/types/dynumber/dynumber.h>

#include <util/string/cast.h>

namespace NKikimr::NKqp {
namespace {

using namespace NDigestTest;
namespace NDigest = NYql::NDigest;
namespace NNative = NKikimr::NTableDigest;
using namespace NYdb;
using namespace NYdb::NTable;
using NScheme::TDecimalType;
using NScheme::TTypeInfo;
namespace NTypeIds = NScheme::NTypeIds;

template <class T> TString NativeBytes(T value) {
  return TString(reinterpret_cast<const char *>(&value), sizeof(value));
}

struct TNativeYqlCase {
  TTypeInfo Type;
  TString Constructor;
  TString TypeName;
  TString Bytes;
};

TVector<TNativeYqlCase> NativeYqlCases() {
  const NYdb::TUuidValue uuid("00112233-4455-6677-8899-aabbccddeeff");
  const auto json = std::get<NBinaryJson::TBinaryJson>(
      NBinaryJson::SerializeToBinaryJson(R"({"b":2})"));
  const auto dyNumber = NDyNumber::ParseDyNumberString("-10.23");
  UNIT_ASSERT(dyNumber);
  // Decimal(5, 2) stores 11.3 as the integer coefficient 1130.
  const std::array<ui64, 2> decimal = {1130, 0};
  return {
      {TTypeInfo(NTypeIds::Bool), "Bool('true')", "Bool", NativeBytes<ui8>(1)},
      {TTypeInfo(NTypeIds::Int8), "Int8('-8')", "Int8", NativeBytes<i8>(-8)},
      {TTypeInfo(NTypeIds::Uint8), "Uint8('200')", "Uint8",
       NativeBytes<ui8>(200)},
      {TTypeInfo(NTypeIds::Int16), "Int16('-16000')", "Int16",
       NativeBytes<i16>(-16000)},
      {TTypeInfo(NTypeIds::Uint16), "Uint16('60000')", "Uint16",
       NativeBytes<ui16>(60000)},
      {TTypeInfo(NTypeIds::Int32), "Int32('-100000')", "Int32",
       NativeBytes<i32>(-100000)},
      {TTypeInfo(NTypeIds::Uint32), "Uint32('100000')", "Uint32",
       NativeBytes<ui32>(100000)},
      {TTypeInfo(NTypeIds::Int64), "Int64('-5000000000')", "Int64",
       NativeBytes<i64>(-5000000000)},
      {TTypeInfo(NTypeIds::Uint64), "Uint64('5000000000')", "Uint64",
       NativeBytes<ui64>(5000000000)},
      {TTypeInfo(NTypeIds::Float), "Float('1.5')", "Float",
       NativeBytes<float>(1.5f)},
      {TTypeInfo(NTypeIds::Double), "Double('2.25')", "Double",
       NativeBytes<double>(2.25)},
      {TTypeInfo(NTypeIds::String), "String('hello')", "String", "hello"},
      {TTypeInfo(NTypeIds::Utf8), "Utf8('мир')", "Utf8", "мир"},
      {TTypeInfo(NTypeIds::Yson), "Yson('[1;2]')", "Yson", "[1;2]"},
      {TTypeInfo(NTypeIds::Json), "Json('{\"a\":1}')", "Json", R"({"a":1})"},
      {TTypeInfo(NTypeIds::JsonDocument), "JsonDocument('{\"b\":2}')",
       "JsonDocument", TString(json.Data(), json.Size())},
      {TTypeInfo(NTypeIds::Uuid),
       "Uuid('00112233-4455-6677-8899-aabbccddeeff')", "Uuid",
       TString(uuid.Buf_.Bytes, sizeof(uuid.Buf_.Bytes))},
      {TTypeInfo(NTypeIds::DyNumber), "DyNumber('-10.23')", "DyNumber",
       *dyNumber},
      {TTypeInfo(NTypeIds::Date), "Date('1970-01-02')", "Date",
       NativeBytes<ui16>(1)},
      {TTypeInfo(NTypeIds::Datetime), "Datetime('1970-01-02T00:00:00Z')",
       "Datetime", NativeBytes<ui32>(86400)},
      {TTypeInfo(NTypeIds::Timestamp),
       "Timestamp('1970-01-02T00:00:00.000000Z')", "Timestamp",
       NativeBytes<ui64>(86400000000)},
      {TTypeInfo(NTypeIds::Interval), "Interval('P1D')", "Interval",
       NativeBytes<i64>(86400000000)},
      {TTypeInfo(NTypeIds::Date32), "Date32('1969-12-31')", "Date32",
       NativeBytes<i32>(-1)},
      {TTypeInfo(NTypeIds::Datetime64), "Datetime64('1969-12-31T00:00:00Z')",
       "Datetime64", NativeBytes<i64>(-86400)},
      {TTypeInfo(NTypeIds::Timestamp64),
       "Timestamp64('1969-12-31T00:00:00.000000Z')", "Timestamp64",
       NativeBytes<i64>(-86400000000)},
      {TTypeInfo(NTypeIds::Interval64), "Interval64('P1D')", "Interval64",
       NativeBytes<i64>(86400000000)},
      {TTypeInfo(TDecimalType(5, 2)), "Decimal('11.3',5,2)", "Decimal(5,2)",
       TString(reinterpret_cast<const char *>(decimal.data()),
               sizeof(decimal))},
  };
}

} // namespace

Y_UNIT_TEST_SUITE(DigestNativeParity) {
  Y_UNIT_TEST(AllScalarTypesAndNulls) {
    TKikimrRunner kikimr(TKikimrSettings().SetWithSampleTables(false));
    auto session =
        kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
    const auto cases = NativeYqlCases();
    UNIT_ASSERT_VALUES_EQUAL(cases.size(), 27);
    TVector<NNative::TColumn> columns;
    TVector<TCell> cells;
    TStringBuilder present;
    TStringBuilder nulls;
    for (size_t i = 0; i < cases.size(); ++i) {
      const auto &item = cases[i];
      const TString name = "c" + ToString(i);
      columns.push_back({name, item.Type, true});
      cells.emplace_back(item.Bytes.data(), item.Bytes.size());
      if (i) {
        present << ", ";
        nulls << ", ";
      }
      present << "Just(" << item.Constructor << ") AS " << name;
      nulls << "CAST(NULL AS " << item.TypeName << "?) AS " << name;
    }
    for (auto algorithm :
         {NDigest::EAlgorithm::CityHash64, NDigest::EAlgorithm::CityHash128,
          NDigest::EAlgorithm::Sha256}) {
      NNative::TAccumulator native(columns, algorithm);
      native.AddRow(cells);
      native.AddRow(TVector<TCell>(columns.size()));
      const TString query = TStringBuilder()
                            << DigestFactorySql()
                            << "$data = (SELECT " << present
                            << " UNION ALL SELECT " << nulls << ");\n"
                            << "SELECT AGGREGATE_BY(TableRow(), $DigestFactory(\""
                            << NDigest::GetAlgorithmName(algorithm)
                            << "\")) FROM $data;";
      const auto result =
          session.ExecuteDataQuery(query, TTxControl::BeginTx().CommitTx())
              .GetValueSync();
      UNIT_ASSERT_C(result.IsSuccess(), result.GetIssues().ToString());
      AssertDigest(ReadDigest(result.GetResultSet(0)), native.GetState());
    }
  }
}

} // namespace NKikimr::NKqp
