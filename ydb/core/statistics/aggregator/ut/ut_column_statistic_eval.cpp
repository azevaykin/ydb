#include <ydb/core/statistics/aggregator/column_statistic_eval.h>
#include <ydb/core/statistics/aggregator/select_builder.h>
#include <ydb/library/yql/udfs/statistics_internal/eqh_agg_func.h>
#include <ydb/public/api/protos/ydb_value.pb.h>

#include <yql/essentials/core/minsketch/count_min_sketch.h>
#include <yql/essentials/minikql/mkql_alloc.h>
#include <yql/essentials/minikql/mkql_string_util.h>

#include <library/cpp/testing/unittest/registar.h>

namespace NKikimr::NStat {
namespace {

using TSampledEqh = NAggFuncs::TEQHSampledAggFunc;

TSampledEqh::TState MakeSampledEqh() {
    return TSampledEqh::CreateState(0, {});
}

void AddSampledKey(TSampledEqh::TState& state, TStringBuf key) {
    NYql::NUdf::TUnboxedValue value = NMiniKQL::MakeString(
        NYql::NUdf::TStringRef(key.data(), key.size()));
    TSampledEqh::CreateStateUpdater(0)(state, value);
}

NYdb::TValue OptionalBytes(const TString& bytes) {
    return NYdb::TValueBuilder().OptionalString(std::string(bytes.data(), bytes.size())).Build();
}

} // namespace

Y_UNIT_TEST_SUITE(SampledColumnStatistics) {
    Y_UNIT_TEST(SimpleSkipsHllAndEmptyPartitionMinMax) {
        TSimpleColumnStatisticEval eval(NScheme::TTypeInfo(NScheme::NTypeIds::Uint64), {}, true);
        TSelectBuilder builder(true);
        eval.AddAggregations("value", builder);
        UNIT_ASSERT_VALUES_EQUAL(eval.EstimateSize(), 1u << 10);
        UNIT_ASSERT_VALUES_EQUAL(builder.ColumnCount(), 2);
        UNIT_ASSERT(!builder.Build("table").Contains("HLL"));

        const auto null = NYdb::TValueBuilder().OptionalUint64(std::nullopt).Build();
        const auto ten = NYdb::TValueBuilder().OptionalUint64(10).Build();
        const auto twenty = NYdb::TValueBuilder().OptionalUint64(20).Build();
        eval.Merge({null, null});
        eval.Merge({ten, twenty});
        const auto result = eval.Extract(2, {null, null});
        UNIT_ASSERT_VALUES_EQUAL(result.GetCount(), 2);
        UNIT_ASSERT(!result.HasCountDistinct());
        UNIT_ASSERT_VALUES_EQUAL(result.GetMin().uint64_value(), 10);
        UNIT_ASSERT_VALUES_EQUAL(result.GetMax().uint64_value(), 20);
        UNIT_ASSERT(result.ByteSizeLong() <= eval.EstimateSize());

        TSimpleColumnStatisticEval full(NScheme::TTypeInfo(NScheme::NTypeIds::Uint64), {});
        TSelectBuilder fullBuilder(true);
        full.AddAggregations("value", fullBuilder);
        UNIT_ASSERT(fullBuilder.Build("table").Contains("HLL"));
    }

    Y_UNIT_TEST(SketchesNeedNoPreliminaryCount) {
        const auto null = NYdb::TValueBuilder().OptionalString(std::nullopt).Build();
        for (bool intermediate : {false, true}) {
            auto eval = IStage2ColumnStatisticEval::CreateSampled(EStatType::COUNT_MIN_SKETCH);
            UNIT_ASSERT(eval);
            TSelectBuilder builder(intermediate);
            eval->AddAggregations("value", builder);
            UNIT_ASSERT(eval->EstimateSize() <= 64u << 10);
            const auto bytes = eval->ExtractData({null});
            UNIT_ASSERT_VALUES_EQUAL(bytes.size(), eval->EstimateSize());
            const auto sketch = std::unique_ptr<TCountMinSketch>(
                TCountMinSketch::FromString(bytes.data(), bytes.size()));
            UNIT_ASSERT_VALUES_EQUAL(sketch->GetElementCount(), 0);

            auto tuple = IMultiColumnStatisticEval::MaybeCreate(
                EStatType::COUNT_MIN_SKETCH, {"a", "b"}, {1, 2}, 0, {.Sampled = true});
            UNIT_ASSERT(tuple);
            TSelectBuilder tupleBuilder(intermediate);
            tuple->AddAggregations(tupleBuilder);
            UNIT_ASSERT_VALUES_EQUAL(tuple->EstimateSize(), eval->EstimateSize());
            const auto tupleBytes = tuple->ExtractData({null});
            UNIT_ASSERT(tupleBytes);
            UNIT_ASSERT_VALUES_EQUAL(tupleBytes->size(), bytes.size());
        }
    }

    Y_UNIT_TEST(WideKeysAreOmittedBeforeStorageAndSurviveMerge) {
        NMiniKQL::TScopedAlloc alloc(__LOCATION__);
        auto omitted = MakeSampledEqh();
        AddSampledKey(omitted, TString(257, 'x'));
        AddSampledKey(omitted, TString(10u << 20, 'y'));
        const auto wire = omitted.Histogram.Serialize();
        UNIT_ASSERT_VALUES_EQUAL(omitted.OmittedKeys, 2);
        UNIT_ASSERT_VALUES_EQUAL(wire.GetTotalCount(), 0);
        UNIT_ASSERT_VALUES_EQUAL(wire.EntriesSize(), 0);
        UNIT_ASSERT(wire.GetMinKey().empty());
        UNIT_ASSERT(TSampledEqh::SerializeState(omitted).size() < 128);

        const auto bytes = TSampledEqh::SerializeState(omitted);
        auto restored = TSampledEqh::DeserializeState(bytes.data(), bytes.size());
        auto valid = MakeSampledEqh();
        AddSampledKey(valid, TString(256, 'a'));
        UNIT_ASSERT(!TSampledEqh::FinalizeState(valid).empty());
        TSampledEqh::MergeStates(restored, valid);
        UNIT_ASSERT_VALUES_EQUAL(valid.OmittedKeys, 2);
        UNIT_ASSERT(TSampledEqh::FinalizeState(valid).empty());
        TSampledEqh::MergeStates(valid, restored);
        UNIT_ASSERT_VALUES_EQUAL(restored.OmittedKeys, 4);
        UNIT_ASSERT(TSampledEqh::FinalizeState(restored).empty());
    }

    Y_UNIT_TEST(EqhSerializedBudgetIncludesOmissions) {
        NMiniKQL::TScopedAlloc alloc(__LOCATION__);
        auto merged = MakeSampledEqh();
        for (ui32 part = 0; part < 8; ++part) {
            auto state = MakeSampledEqh();
            for (ui32 i = 0; i < 1024; ++i) {
                // Interleaved shards and reverse input exercise staging and merge.
                const ui32 value = (1023 - i) * 8 + part;
                TString key(256, 'x');
                for (size_t byte = 0; byte < sizeof(value); ++byte) {
                    key[byte] = static_cast<char>(value >> (8 * (3 - byte)));
                }
                AddSampledKey(state, key);
            }
            const auto bytes = TSampledEqh::SerializeState(state);
            UNIT_ASSERT(bytes.size() <= 32u << 10);
            merged.Merge(TSampledEqh::DeserializeState(bytes.data(), bytes.size()));
            UNIT_ASSERT(TSampledEqh::SerializeState(merged).size() <= 32u << 10);
            UNIT_ASSERT(TSampledEqh::FinalizeState(merged).size() <= 32u << 10);
        }
        auto omitted = MakeSampledEqh();
        AddSampledKey(omitted, TString(257, 'x'));
        merged.Merge(omitted);
        const auto bytes = TSampledEqh::SerializeState(merged);
        UNIT_ASSERT(bytes.size() <= 32u << 10);
        const auto restored = TSampledEqh::DeserializeState(bytes.data(), bytes.size());
        UNIT_ASSERT_VALUES_EQUAL(restored.OmittedKeys, 1);
        UNIT_ASSERT(TSampledEqh::FinalizeState(restored).empty());
    }

    Y_UNIT_TEST(EqhEvaluatorSuppressesOnePartitionsOmission) {
        NMiniKQL::TScopedAlloc alloc(__LOCATION__);
        for (const auto& columns : {std::vector<TString>{"a"}, std::vector<TString>{"a", "b"}}) {
            const auto ids = columns.size() == 1 ? std::vector<ui32>{1} : std::vector<ui32>{1, 2};
            auto eval = IMultiColumnStatisticEval::MaybeCreate(
                EStatType::EQ_HEIGHT_HISTOGRAM, columns, ids, 0, {.Sampled = true});
            UNIT_ASSERT(eval);
            UNIT_ASSERT_VALUES_EQUAL(eval->EstimateSize(), 32u << 10);
            TSelectBuilder builder(true);
            eval->AddAggregations(builder);
            UNIT_ASSERT(builder.Build("table").Contains("EQHSampled"));
            auto valid = MakeSampledEqh();
            AddSampledKey(valid, "valid");
            eval->Merge({OptionalBytes(TSampledEqh::SerializeState(valid))});
            auto omitted = MakeSampledEqh();
            AddSampledKey(omitted, TString(257, 'x'));
            UNIT_ASSERT(!eval->ExtractData({OptionalBytes(TSampledEqh::SerializeState(omitted))}));
        }
    }
}

} // namespace NKikimr::NStat
