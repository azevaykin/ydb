#include <ydb/core/statistics/ut_common/ut_common.h>
#include <ydb/core/statistics/aggregator/analyze_actor.h>
#include <ydb/library/testlib/helpers.h>
#include <ydb/core/statistics/service/service.h>
#include <ydb/core/testlib/actors/block_events.h>
#include <ydb/core/testlib/tablet_helpers.h>
#include <limits>
#include <numeric>

namespace NKikimr::NStat {
namespace {

TResponse ReadSample(TTestActorRuntime& runtime, const TPathId& pathId, EStatType type,
        TColumnTags columns = {}) {
    auto request = std::make_unique<TEvStatistics::TEvGetStatistics>();
    request->Database = "/Root/Database";
    request->StatType = type;
    request->StatRequests.push_back({.PathId = pathId, .ColumnTags = std::move(columns), .AcceptSampledStatistics = true});
    auto sender = runtime.AllocateEdgeActor(1);
    runtime.Send(MakeStatServiceID(runtime.GetNodeId(1)), sender, request.release(), 1, true);
    auto response = runtime.GrabEdgeEventRethrow<TEvStatistics::TEvGetStatisticsResult>(sender);
    UNIT_ASSERT_VALUES_EQUAL(response->Get()->StatResponses.size(), 1);
    return response->Get()->StatResponses.front();
}

NKikimrStat::TEvAnalyzeResponse Sample(TTestActorRuntime& runtime, const TTableInfo& table,
        TString operation, double rate, const TVector<ui32>& columns = {}) {
    auto request = MakeAnalyzeRequest({table.PathId}, operation, "/Root/Database");
    request->Record.MutableTables(0)->SetSampleRate(rate);
    request->Record.MutableTables(0)->SetPath(table.Path);
    request->Record.MutableTables(0)->MutableColumnTags()->Assign(columns.begin(), columns.end());
    auto sender = runtime.AllocateEdgeActor();
    runtime.SendToPipe(table.SaTabletId, sender, request.release());
    return runtime.GrabEdgeEventRethrow<TEvStatistics::TEvAnalyzeResponse>(sender)->Get()->Record;
}

} // namespace

Y_UNIT_TEST_SUITE(AnalyzeSampling) {
    Y_UNIT_TEST(RateAndGranularity) {
        TVector<ui64> tablets(100);
        std::iota(tablets.begin(), tablets.end(), 1000);
        UNIT_ASSERT_VALUES_EQUAL(SelectAnalyzeSample(tablets, 0.05, 1).size(), 5);
        UNIT_ASSERT_VALUES_EQUAL(SelectAnalyzeSample(tablets, 0.07, 1).size(), 7);
        UNIT_ASSERT_VALUES_EQUAL(SelectAnalyzeSample(tablets, 0.0701, 1).size(), 8);
        tablets.resize(8);
        UNIT_ASSERT_VALUES_EQUAL(SelectAnalyzeSample(tablets, 0.05, 1).size(), 1);
        UNIT_ASSERT(SelectAnalyzeSample(tablets, 1, 1) == tablets);
        UNIT_ASSERT(SelectAnalyzeSample({}, 0.05, 1).empty());
    }
}

Y_UNIT_TEST_SUITE(AnalyzeSampleCollection) {
    Y_UNIT_TEST(SamplingUsesReadableShards) {
        TTestEnv env(1, 1, false);
        auto& runtime = *env.GetServer().GetRuntime();
        CreateDatabase(env, "Database");
        const auto table = PrepareColumnTable(env, "Database", "Table", 4);
        bool updatedSharding = false;
        auto observer = runtime.AddObserver<TEvTxProxySchemeCache::TEvNavigateKeySetResult>([&](auto& ev) {
            for (auto& entry : ev->Get()->Request->ResultSet) {
                if (!entry.SyncVersion || entry.TableId.PathId != table.PathId || !entry.ColumnTableInfo) {
                    continue;
                }
                auto info = MakeIntrusive<NSchemeCache::TSchemeCacheNavigate::TColumnTableInfo>();
                info->Kind = entry.ColumnTableInfo->Kind;
                info->Description = entry.ColumnTableInfo->Description;
                info->OlapStoreId = entry.ColumnTableInfo->OlapStoreId;
                auto& sharding = *info->Description.MutableSharding();
                sharding.ClearShardsInfo();
                for (size_t i = 0; i < sharding.ColumnShardsSize(); ++i) {
                    auto* shard = sharding.AddShardsInfo();
                    shard->SetTabletId(sharding.GetColumnShards(i));
                    shard->SetSequenceIdx(i);
                    shard->SetShardingVersion(0);
                    shard->SetIsOpenForRead(i != 0);
                    shard->SetIsOpenForWrite(i != 0);
                }
                entry.ColumnTableInfo = std::move(info);
                updatedSharding = true;
            }
        });
        const auto result = Sample(runtime, table, "readable-shards", 0.99);
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NKikimrStat::TEvAnalyzeResponse::STATUS_SUCCESS, result.DebugString());
        UNIT_ASSERT(updatedSharding);
        const auto sampled = ReadSample(runtime, table.PathId, EStatType::TABLE_SUMMARY);
        UNIT_ASSERT(sampled.Success && sampled.Sampling);
        UNIT_ASSERT_VALUES_EQUAL(sampled.Sampling->GetEligibleUnits(), 3);
        UNIT_ASSERT_VALUES_EQUAL(sampled.Sampling->GetSelectedUnits(), 3);
    }

    Y_UNIT_TEST(SampledStatisticsRequireOptIn) {
        TTestEnv env(1, 1, false);
        auto& runtime = *env.GetServer().GetRuntime();
        CreateDatabase(env, "Database");
        const auto table = PrepareMultiColumnTable(env, "Database", "Table", true);
        Analyze(runtime, table.SaTabletId, {table.PathId}, "full", "/Root/Database");

        ui32 shardsTotal = 0;
        ui32 shardsDone = 0;
        auto progressObserver = runtime.AddObserver<TEvStatistics::TEvAnalyzeActorProgress>([&](auto& ev) {
            shardsTotal = ev->Get()->ShardsTotal;
            shardsDone = ev->Get()->ShardsDone;
        });
        const auto result = Sample(runtime, table, "sample", 0.5);
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NKikimrStat::TEvAnalyzeResponse::STATUS_SUCCESS, result.DebugString());

        const auto sampled = ReadSample(runtime, table.PathId, EStatType::TABLE_SUMMARY);
        UNIT_ASSERT(sampled.Success && sampled.Sampling && sampled.TableSummary.Data);
        const auto& metadata = *sampled.Sampling;
        UNIT_ASSERT_VALUES_EQUAL(metadata.GetSelectedUnits(), 2);
        UNIT_ASSERT_VALUES_EQUAL(metadata.GetEligibleUnits(), 4);
        UNIT_ASSERT_VALUES_EQUAL(sampled.TableSummary.Data->GetRowCount(),
            metadata.GetSampleRows());
        UNIT_ASSERT_VALUES_EQUAL(shardsTotal, metadata.GetSelectedUnits());
        UNIT_ASSERT_VALUES_EQUAL(shardsDone, shardsTotal);

        const auto legacy = GetStatistics(runtime, table.PathId, EStatType::TABLE_SUMMARY, {std::nullopt});
        UNIT_ASSERT(legacy.front().Success && !legacy.front().Sampling);
        UNIT_ASSERT_VALUES_EQUAL(legacy.front().TableSummary.Data->GetRowCount(), ColumnTableRowsNumber);

        const auto simple = ReadSample(runtime, table.PathId, EStatType::SIMPLE_COLUMN, TColumnTags(2u));
        UNIT_ASSERT(simple.Success && simple.SimpleColumn.Data && simple.Sampling);
        UNIT_ASSERT_VALUES_EQUAL(simple.SimpleColumn.Data->GetCount(), simple.Sampling->GetSampleRows());
        const auto cms = ReadSample(runtime, table.PathId, EStatType::COUNT_MIN_SKETCH, TColumnTags(2u));
        UNIT_ASSERT(cms.Success && cms.CountMinSketch.CountMin && cms.Sampling);
        ui64 frequencies = 0;
        for (ui32 value = 0; value < 10; ++value) {
            const auto key = ToString(value);
            frequencies += cms.CountMinSketch.CountMin->Probe(key.data(), key.size());
        }
        UNIT_ASSERT_VALUES_EQUAL(frequencies, simple.SimpleColumn.Data->GetCount());
        if (metadata.GetSelectedUnits() < metadata.GetEligibleUnits()) {
            UNIT_ASSERT(!simple.SimpleColumn.Data->HasCountDistinct());
        }

        Analyze(runtime, table.SaTabletId, {table.PathId}, "full-again", "/Root/Database");
        const auto fullAgain = ReadSample(runtime, table.PathId, EStatType::TABLE_SUMMARY);
        UNIT_ASSERT(fullAgain.Success && !fullAgain.Sampling);
        UNIT_ASSERT_VALUES_EQUAL(fullAgain.TableSummary.Data->GetRowCount(), ColumnTableRowsNumber);
    }

    Y_UNIT_TEST_TWIN(PartialAnalyzePreservesOtherStatistics, Restart) {
        TTestEnv env(1, 1, false, [](Tests::TServerSettings& settings) {
            settings.AppConfig->MutableStatisticsConfig()->SetAnalyzeCollectPrimaryKeyHistogram(true);
        });
        auto& runtime = *env.GetServer().GetRuntime();
        CreateDatabase(env, "Database");
        const auto table = PrepareMultiColumnTable(env, "Database", "Table", true);
        UNIT_ASSERT_VALUES_EQUAL(Sample(runtime, table, "all", 0.5).GetStatus(), NKikimrStat::TEvAnalyzeResponse::STATUS_SUCCESS);
        const auto previous = ReadSample(runtime, table.PathId, EStatType::SIMPLE_COLUMN, TColumnTags(1u));
        UNIT_ASSERT(previous.Success && previous.Sampling && previous.SimpleColumn.Data);

        if (Restart) {
            TBlockEvents<TEvStatistics::TEvAnalyzeActorResult> results(runtime);
            const auto sender = runtime.AllocateEdgeActor();
            auto request = MakeAnalyzeRequest({table.PathId}, "partial", "/Root/Database");
            request->Record.MutableTables(0)->SetSampleRate(0.25);
            request->Record.MutableTables(0)->AddColumnTags(2);
            runtime.SendToPipe(table.SaTabletId, sender, request.release());
            runtime.WaitFor("partial collection", [&] { return !results.empty(); }, TDuration::Seconds(30));
            results.Stop();
            RebootTablet(runtime, table.SaTabletId, sender);
            const auto response = runtime.GrabEdgeEventRethrow<TEvStatistics::TEvAnalyzeResponse>(sender);
            UNIT_ASSERT_VALUES_EQUAL(response->Get()->Record.GetStatus(), NKikimrStat::TEvAnalyzeResponse::STATUS_SUCCESS);
        } else {
            UNIT_ASSERT_VALUES_EQUAL(Sample(runtime, table, "partial", 0.25, {2}).GetStatus(),
                NKikimrStat::TEvAnalyzeResponse::STATUS_SUCCESS);
        }
        const auto current = ReadSample(runtime, table.PathId, EStatType::SIMPLE_COLUMN, TColumnTags(1u));
        UNIT_ASSERT(current.Success && current.Sampling && current.SimpleColumn.Data);
        UNIT_ASSERT_VALUES_EQUAL(current.Sampling->SerializeAsString(), previous.Sampling->SerializeAsString());
        UNIT_ASSERT_VALUES_EQUAL(current.SimpleColumn.Data->SerializeAsString(), previous.SimpleColumn.Data->SerializeAsString());
        const auto histogram = ReadSample(runtime, table.PathId, EStatType::EQ_HEIGHT_HISTOGRAM, TColumnTags(1u));
        UNIT_ASSERT(histogram.Success && histogram.Sampling);
        UNIT_ASSERT_VALUES_EQUAL(histogram.Sampling->GetRequestedRate(), 0.25);
    }

    Y_UNIT_TEST(RowTableSamplingRejected) {
        TTestEnv env(1, 1, false);
        auto& runtime = *env.GetServer().GetRuntime();
        CreateDatabase(env, "Database");
        const auto table = PrepareMultiColumnTable(env, "Database", "Table", false);
        const auto result = Sample(runtime, table, "row-sample", 0.5);
        UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), NKikimrStat::TEvAnalyzeResponse::STATUS_ERROR);
        UNIT_ASSERT_STRING_CONTAINS(result.DebugString(), "ANALYZE SAMPLE is supported only for column tables");
        UNIT_ASSERT_VALUES_EQUAL(Sample(runtime, table, "row-full", 1).GetStatus(),
            NKikimrStat::TEvAnalyzeResponse::STATUS_SUCCESS);
        const auto full = ReadSample(runtime, table.PathId, EStatType::TABLE_SUMMARY);
        UNIT_ASSERT(full.Success && !full.Sampling && full.TableSummary.Data);
        UNIT_ASSERT_VALUES_EQUAL(full.TableSummary.Data->GetRowCount(), 1000);
    }

    Y_UNIT_TEST(EmptySampleSucceeds) {
        TTestEnv env(1, 1, false);
        auto& runtime = *env.GetServer().GetRuntime();
        CreateDatabase(env, "Database");
        const auto table = CreateEmptyTable(env, "Database", "Table", true);
        Analyze(runtime, table.SaTabletId, {table.PathId}, "empty-full", "/Root/Database");
        const auto result = Sample(runtime, table, "empty-sample", 0.5);
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NKikimrStat::TEvAnalyzeResponse::STATUS_SUCCESS, result.DebugString());
        const auto sampled = ReadSample(runtime, table.PathId, EStatType::TABLE_SUMMARY);
        UNIT_ASSERT(sampled.Success && sampled.Sampling && sampled.TableSummary.Data);
        UNIT_ASSERT_VALUES_EQUAL(sampled.Sampling->GetSampleRows(), 0);
        UNIT_ASSERT_VALUES_EQUAL(sampled.TableSummary.Data->GetRowCount(), 0);
    }

    Y_UNIT_TEST(FailedCollectionKeepsCompletedBatches) {
        TTestEnv env(1, 1, false);
        auto& runtime = *env.GetServer().GetRuntime();
        CreateDatabase(env, "Database");
        const auto table = PrepareMultiColumnTable(env, "Database", "Table", true);
        size_t batches = 0;
        auto observer = runtime.AddObserver<TEvStatistics::TEvAnalyzeActorResult>([&](auto& ev) {
            if (++batches == 2) {
                ev->Get()->Status = TEvStatistics::TEvAnalyzeActorResult::EStatus::InternalError;
                ev->Get()->Final = true;
                ev->Get()->Issues.AddIssue(NYql::TIssue("injected collection failure"));
            }
        });
        UNIT_ASSERT_VALUES_EQUAL(Sample(runtime, table, "failed", 0.5).GetStatus(), NKikimrStat::TEvAnalyzeResponse::STATUS_ERROR);
        runtime.WaitFor("saved first batch", [&] {
            return ReadSample(runtime, table.PathId, EStatType::TABLE_SUMMARY).Success;
        }, TDuration::Seconds(30));
        const auto sampled = ReadSample(runtime, table.PathId, EStatType::TABLE_SUMMARY);
        UNIT_ASSERT(sampled.Sampling);
        UNIT_ASSERT_VALUES_EQUAL(sampled.Sampling->GetRequestedRate(), 0.5);
    }

    Y_UNIT_TEST(InvalidSampleRate) {
        TTestEnv env(1, 1, false);
        auto& runtime = *env.GetServer().GetRuntime();
        CreateDatabase(env, "Database");
        const auto table = PrepareColumnTable(env, "Database", "Table", 4);
        for (double rate : {0.0, -0.1, 1.1, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
            const auto result = Sample(runtime, table, "invalid", rate);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), NKikimrStat::TEvAnalyzeResponse::STATUS_ERROR);
            UNIT_ASSERT_STRING_CONTAINS(result.DebugString(), "finite number in (0, 1]");
        }
    }
}

} // namespace NKikimr::NStat
