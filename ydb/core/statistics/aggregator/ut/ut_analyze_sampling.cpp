#include <ydb/core/statistics/ut_common/ut_common.h>
#include <ydb/core/statistics/aggregator/analyze_actor.h>
#include <ydb/library/testlib/helpers.h>
#include <ydb/core/statistics/service/service.h>
#include <ydb/core/base/request_types.h>
#include <ydb/core/base/sampling.h>
#include <ydb/core/kqp/common/events/events.h>
#include <ydb/core/kqp/executer_actor/kqp_executer.h>
#include <ydb/core/tx/datashard/datashard.h>
#include <ydb/core/tx/schemeshard/schemeshard.h>
#include <ydb/core/testlib/actors/block_events.h>
#include <ydb/core/testlib/tablet_helpers.h>
#include <ydb/core/testlib/tx_helpers.h>
#include <ydb/library/mkql_proto/protos/minikql.pb.h>
#include <algorithm>
#include <limits>
#include <numeric>

namespace NKikimr::NStat {
namespace {

bool IsAnalyzeScan(const NKqp::TEvKqp::TEvQueryRequest::TPtr& ev) {
    // Count the original request, before the proxy forwards it to a session.
    const auto& request = *ev->Get();
    return ev->Sender == request.GetRequestActorId()
        && request.GetRequestType() == NRequestTypes::Analyze
        && request.GetType() == NKikimrKqp::QUERY_TYPE_SQL_SCAN;
}

auto ObserveScannedShards(TTestActorRuntime& runtime, THashSet<ui64>& shards) {
    return runtime.AddObserver<NKqp::TEvKqp::TEvQueryRequest>([&](auto& ev) {
        if (IsAnalyzeScan(ev)) {
            const auto& query = ev->Get()->GetQuery();
            TStringBuf prefix, tablet;
            UNIT_ASSERT_C(TStringBuf(query).TrySplit(" WITH TabletId = '", prefix, tablet), query);
            shards.insert(FromString<ui64>(tablet.Before('\'')));
        }
    });
}

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

TActorId StartSample(TTestActorRuntime& runtime, const TTableInfo& table,
        TString operation, double rate, const TVector<ui32>& columns = {}) {
    auto request = MakeAnalyzeRequest({table.PathId}, operation, "/Root/Database");
    request->Record.MutableTables(0)->SetSampleRate(rate);
    request->Record.MutableTables(0)->SetPath(table.Path);
    request->Record.MutableTables(0)->MutableColumnTags()->Assign(columns.begin(), columns.end());
    const auto sender = runtime.AllocateEdgeActor();
    runtime.SendToPipe(table.SaTabletId, sender, request.release());
    return sender;
}

NKikimrStat::TEvAnalyzeResponse Sample(TTestActorRuntime& runtime, const TTableInfo& table,
        TString operation, double rate, const TVector<ui32>& columns = {}) {
    const auto sender = StartSample(runtime, table, operation, rate, columns);
    return runtime.GrabEdgeEventRethrow<TEvStatistics::TEvAnalyzeResponse>(sender)->Get()->Record;
}

TTableInfo PrepareSamplingRowTable(TTestEnv& env, ui32 shards = 13) {
    TStringBuilder create;
    create << "CREATE TABLE `/Root/Database/Table` (Key Uint64 NOT NULL, Value1 String, Value2 String, "
        << "PRIMARY KEY (Key), STATISTICS tuple_hist ON (Value1, Value2) WITH (EQ_HEIGHT_HISTOGRAM))";
    if (shards > 1) {
        create << " WITH (PARTITION_AT_KEYS = (";
        for (ui32 shard = 1; shard < shards; ++shard) {
            if (shard > 1) {
                create << ", ";
            }
            create << shard * 10;
        }
        create << "))";
    }
    create << ";";
    ExecuteYqlScript(env, create);
    InsertDataIntoTable(env, "Database", "Table", shards * 10, MultiColumnValueColumns());
    auto& runtime = *env.GetServer().GetRuntime();
    TTableInfo table;
    table.Path = "/Root/Database/Table";
    table.PathId = ResolvePathId(runtime, table.Path, &table.DomainKey, &table.SaTabletId);
    table.ShardIds = GetTableShards(runtime, runtime.AllocateEdgeActor(), table.Path);
    UNIT_ASSERT_VALUES_EQUAL(table.ShardIds.size(), shards);
    return table;
}

} // namespace

Y_UNIT_TEST_SUITE(AnalyzeSampling) {
    Y_UNIT_TEST(SampleSize) {
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

    Y_UNIT_TEST(SamplingUsesReadableShards) {
        TTestEnv env(1, 1, false);
        auto& runtime = *env.GetServer().GetRuntime();
        CreateDatabase(env, "Database");
        const auto table = PrepareColumnTable(env, "Database", "Table", 4);
        bool updatedSharding = false;
        ui64 closedShard = 0;
        THashSet<ui64> scannedShards;
        auto scans = ObserveScannedShards(runtime, scannedShards);
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
                closedShard = sharding.GetColumnShards(0);
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
        UNIT_ASSERT_VALUES_EQUAL(scannedShards.size(), 3);
        UNIT_ASSERT(!scannedShards.contains(closedShard));
        const auto sampled = ReadSample(runtime, table.PathId, EStatType::TABLE_SUMMARY);
        UNIT_ASSERT(sampled.Success && sampled.Sampling);
        UNIT_ASSERT_VALUES_EQUAL(sampled.Sampling->GetEligibleUnits(), 3);
        UNIT_ASSERT_VALUES_EQUAL(sampled.Sampling->GetSelectedUnits(), 3);
    }

    Y_UNIT_TEST(CollectAndPersistStatistics) {
        TTestEnv env(1, 1, false);
        auto& runtime = *env.GetServer().GetRuntime();
        CreateDatabase(env, "Database");
        const auto table = PrepareMultiColumnTable(env, "Database", "Table", true);
        THashSet<ui64> scannedShards;
        auto scans = ObserveScannedShards(runtime, scannedShards);

        ui32 shardsTotal = 0;
        ui32 shardsDone = 0;
        auto progressObserver = runtime.AddObserver<TEvStatistics::TEvAnalyzeActorProgress>([&](auto& ev) {
            shardsTotal = ev->Get()->ShardsTotal;
            shardsDone = ev->Get()->ShardsDone;
        });
        const auto result = Sample(runtime, table, "sample", 0.5);
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NKikimrStat::TEvAnalyzeResponse::STATUS_SUCCESS, result.DebugString());
        scans.Remove();
        UNIT_ASSERT_VALUES_EQUAL(scannedShards.size(), 2);

        ui64 expectedRows = 0;
        THashMap<TString, ui64> expectedFrequencies;
        for (const auto shard : scannedShards) {
            const auto rows = ExecuteYqlScriptWithResult(env, TStringBuilder()
                << "SELECT Value1, COUNT(*) FROM `" << table.Path
                << "` WITH TabletId = '" << shard << "' GROUP BY Value1;");
            for (const auto& row : rows.rows()) {
                expectedFrequencies[row.items(0).bytes_value()] += row.items(1).uint64_value();
                expectedRows += row.items(1).uint64_value();
            }
        }
        UNIT_ASSERT(expectedRows > 0 && expectedRows < ColumnTableRowsNumber);

        const auto sampled = ReadSample(runtime, table.PathId, EStatType::TABLE_SUMMARY);
        UNIT_ASSERT(sampled.Success && sampled.Sampling && sampled.TableSummary.Data);
        const auto& metadata = *sampled.Sampling;
        UNIT_ASSERT(metadata.GetMethod() == NKikimrStat::TSamplingStatistics::SHARD_SUBSET);
        UNIT_ASSERT_VALUES_EQUAL(metadata.GetEffectiveRate(), 0.5);
        UNIT_ASSERT(metadata.HasSeed());
        UNIT_ASSERT_VALUES_EQUAL(metadata.GetRequestedRate(), 0.5);
        UNIT_ASSERT_VALUES_EQUAL(metadata.GetSelectedUnits(), 2);
        UNIT_ASSERT_VALUES_EQUAL(metadata.GetEligibleUnits(), 4);
        UNIT_ASSERT_VALUES_EQUAL(metadata.GetSampleRows(), expectedRows);
        UNIT_ASSERT_VALUES_EQUAL(sampled.TableSummary.Data->GetRowCount(), expectedRows);
        UNIT_ASSERT_VALUES_EQUAL(shardsTotal, metadata.GetSelectedUnits());
        UNIT_ASSERT_VALUES_EQUAL(shardsDone, shardsTotal);

        const auto legacy = GetStatistics(runtime, table.PathId, EStatType::TABLE_SUMMARY, {std::nullopt});
        UNIT_ASSERT_VALUES_EQUAL(legacy.size(), 1);
        UNIT_ASSERT(!legacy.front().Success && !legacy.front().Sampling);

        const auto simple = ReadSample(runtime, table.PathId, EStatType::SIMPLE_COLUMN, TColumnTags(2u));
        UNIT_ASSERT(simple.Success && simple.SimpleColumn.Data && simple.Sampling);
        UNIT_ASSERT_VALUES_EQUAL(simple.SimpleColumn.Data->GetCount(), expectedRows);
        UNIT_ASSERT_VALUES_EQUAL(simple.Sampling->GetSampleRows(), expectedRows);
        UNIT_ASSERT(!simple.SimpleColumn.Data->HasCountDistinct());
        const auto cms = ReadSample(runtime, table.PathId, EStatType::COUNT_MIN_SKETCH, TColumnTags(2u));
        UNIT_ASSERT(cms.Success && cms.CountMinSketch.CountMin && cms.Sampling);
        for (ui32 value = 0; value < 10; ++value) {
            const auto key = ToString(value);
            UNIT_ASSERT_VALUES_EQUAL(cms.CountMinSketch.CountMin->Probe(key.data(), key.size()), expectedFrequencies[key]);
        }
    }

    Y_UNIT_TEST(BackgroundTraversalDoesNotCompleteSample) {
        TTestEnv env(1, 1, false, [](Tests::TServerSettings& settings) {
            auto* stats = settings.AppConfig->MutableStatisticsConfig();
            stats->SetEnableBackgroundColumnStatsCollection(true);
            stats->SetBaseStatsSendInitialDelaySeconds(3);
        });
        auto& runtime = *env.GetServer().GetRuntime();
        CreateDatabase(env, "Database");
        TBlockEvents<TEvStatistics::TEvAnalyzeActorResult> results(runtime, [](auto& ev) {
            return ev->Get()->Final;
        });
        const auto table = PrepareColumnTable(env, "Database", "Table", 4);
        runtime.WaitFor("background collection", [&] { return !results.empty(); }, TDuration::Seconds(30));

        const TString operation = "queued-sample";
        const auto sender = StartSample(runtime, table, operation, 0.5);
        AnalyzeStatus(runtime, sender, table.SaTabletId, operation,
            NKikimrStat::TEvAnalyzeStatusResponse::STATUS_ENQUEUED);

        // Keep subsequent completions blocked while checking the queued request.
        results.Unblock();
        runtime.WaitFor("background completed", [&] {
            return GetBackgroundAnalyzeCompletedCount(runtime) > 0;
        }, TDuration::Seconds(30));
        const auto status = TestGetAnalyzeOp(runtime, table.SaTabletId, "/Root/Database", operation);
        UNIT_ASSERT_C(status.GetAnalyzeOperation().GetState() != Ydb::Table::AnalyzeState::STATE_DONE,
            status.ShortDebugString());
        results.Stop().Unblock();
        const auto response = runtime.GrabEdgeEventRethrow<TEvStatistics::TEvAnalyzeResponse>(sender);
        UNIT_ASSERT_VALUES_EQUAL(response->Get()->Record.GetStatus(), NKikimrStat::TEvAnalyzeResponse::STATUS_SUCCESS);

        const auto sampled = ReadSample(runtime, table.PathId, EStatType::TABLE_SUMMARY);
        UNIT_ASSERT(sampled.Success && sampled.Sampling && sampled.TableSummary.Data);
        UNIT_ASSERT_VALUES_EQUAL(sampled.Sampling->GetRequestedRate(), 0.5);
        UNIT_ASSERT_VALUES_EQUAL(sampled.Sampling->GetEligibleUnits(), 4);
        UNIT_ASSERT_VALUES_EQUAL(sampled.Sampling->GetSelectedUnits(), 2);
        UNIT_ASSERT(sampled.Sampling->GetSampleRows() > 0 && sampled.Sampling->GetSampleRows() < ColumnTableRowsNumber);
        UNIT_ASSERT_VALUES_EQUAL(sampled.TableSummary.Data->GetRowCount(), sampled.Sampling->GetSampleRows());
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
            const auto sender = StartSample(runtime, table, "partial", 0.25, {2});
            runtime.WaitFor("partial collection", [&] { return !results.empty(); }, TDuration::Seconds(30));
            results.Stop();
            RebootTablet(runtime, table.SaTabletId, sender);
            const auto response = runtime.GrabEdgeEventRethrow<TEvStatistics::TEvAnalyzeResponse>(sender);
            UNIT_ASSERT_VALUES_EQUAL(response->Get()->Record.GetOperationId(), "partial");
            UNIT_ASSERT_VALUES_EQUAL(response->Get()->Record.GetStatus(), NKikimrStat::TEvAnalyzeResponse::STATUS_ERROR);
            UNIT_ASSERT_STRING_CONTAINS(response->Get()->Record.DebugString(), "cannot resume");
        } else {
            UNIT_ASSERT_VALUES_EQUAL(Sample(runtime, table, "partial", 0.25, {2}).GetStatus(),
                NKikimrStat::TEvAnalyzeResponse::STATUS_SUCCESS);
        }
        const auto current = ReadSample(runtime, table.PathId, EStatType::SIMPLE_COLUMN, TColumnTags(1u));
        UNIT_ASSERT(current.Success && current.Sampling && current.SimpleColumn.Data);
        UNIT_ASSERT_VALUES_EQUAL(current.Sampling->SerializeAsString(), previous.Sampling->SerializeAsString());
        UNIT_ASSERT_VALUES_EQUAL(current.SimpleColumn.Data->SerializeAsString(), previous.SimpleColumn.Data->SerializeAsString());
        const auto updated = ReadSample(runtime, table.PathId, EStatType::SIMPLE_COLUMN, TColumnTags(2u));
        UNIT_ASSERT(updated.Success && updated.Sampling && updated.SimpleColumn.Data);
        UNIT_ASSERT_VALUES_EQUAL(updated.Sampling->GetRequestedRate(), Restart ? 0.5 : 0.25);
        UNIT_ASSERT_VALUES_EQUAL(updated.Sampling->GetSelectedUnits(), Restart ? 2 : 1);
        UNIT_ASSERT_VALUES_EQUAL(updated.Sampling->GetEligibleUnits(), 4);
        UNIT_ASSERT(updated.SimpleColumn.Data->GetCount() > 0);
        UNIT_ASSERT_VALUES_EQUAL(updated.SimpleColumn.Data->GetCount(), updated.Sampling->GetSampleRows());
        const auto histogram = ReadSample(runtime, table.PathId, EStatType::EQ_HEIGHT_HISTOGRAM, TColumnTags(1u));
        UNIT_ASSERT(histogram.Success && histogram.Sampling);
        UNIT_ASSERT_VALUES_EQUAL(histogram.Sampling->GetRequestedRate(), Restart ? 0.5 : 0.25);
    }

    Y_UNIT_TEST(RowTableSamplingDisabled) {
        TTestEnv env(1, 1, false, [](Tests::TServerSettings& settings) {
            settings.FeatureFlags.SetEnableAnalyzeSampling(false);
        });
        auto& runtime = *env.GetServer().GetRuntime();
        CreateDatabase(env, "Database");
        const auto table = CreateEmptyTable(env, "Database", "Table", false);
        const auto result = Sample(runtime, table, "row-sample", 0.5);
        UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), NKikimrStat::TEvAnalyzeResponse::STATUS_ERROR);
        UNIT_ASSERT_STRING_CONTAINS(result.DebugString(), "ANALYZE sampling is disabled");
        UNIT_ASSERT_VALUES_EQUAL(Sample(runtime, table, "row-full", 1).GetStatus(),
            NKikimrStat::TEvAnalyzeResponse::STATUS_SUCCESS);
        const auto full = ReadSample(runtime, table.PathId, EStatType::TABLE_SUMMARY);
        UNIT_ASSERT(full.Success && !full.Sampling && full.TableSummary.Data);
        UNIT_ASSERT_VALUES_EQUAL(full.TableSummary.Data->GetRowCount(), 0);
    }

    Y_UNIT_TEST(EmptyTable) {
        TTestEnv env(1, 1, false);
        auto& runtime = *env.GetServer().GetRuntime();
        CreateDatabase(env, "Database");
        const auto table = CreateEmptyTable(env, "Database", "Table", true);
        const auto result = Sample(runtime, table, "empty-sample", 0.5);
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NKikimrStat::TEvAnalyzeResponse::STATUS_SUCCESS, result.DebugString());
        const auto sampled = ReadSample(runtime, table.PathId, EStatType::TABLE_SUMMARY);
        UNIT_ASSERT(sampled.Success && sampled.Sampling && sampled.TableSummary.Data);
        UNIT_ASSERT_VALUES_EQUAL(sampled.Sampling->GetSampleRows(), 0);
        UNIT_ASSERT_VALUES_EQUAL(sampled.TableSummary.Data->GetRowCount(), 0);
    }

    Y_UNIT_TEST(FailedCollectionPublishesNothing) {
        TTestEnv env(1, 1, false);
        auto& runtime = *env.GetServer().GetRuntime();
        CreateDatabase(env, "Database");
        const auto table = PrepareMultiColumnTable(env, "Database", "Table", true);
        size_t batches = 0;
        size_t saves = 0;
        auto observer = runtime.AddObserver<TEvStatistics::TEvAnalyzeActorResult>([&](auto& ev) {
            ++batches;
            UNIT_ASSERT(ev->Get()->Final);
            ev->Get()->Status = TEvStatistics::TEvAnalyzeActorResult::EStatus::InternalError;
            ev->Get()->Issues.AddIssue(NYql::TIssue("injected collection failure"));
        });
        auto saveObserver = runtime.AddObserver<TEvStatistics::TEvSaveStatisticsQueryResponse>([&](auto&) {
            ++saves;
        });
        const auto result = Sample(runtime, table, "failed", 0.5);
        UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), NKikimrStat::TEvAnalyzeResponse::STATUS_ERROR);
        UNIT_ASSERT_STRING_CONTAINS(result.DebugString(), "injected collection failure");
        UNIT_ASSERT_VALUES_EQUAL(batches, 1);
        UNIT_ASSERT_VALUES_EQUAL(saves, 0);
        UNIT_ASSERT(!ReadSample(runtime, table.PathId, EStatType::TABLE_SUMMARY).Success);
        UNIT_ASSERT(!ReadSample(runtime, table.PathId, EStatType::SIMPLE_COLUMN, TColumnTags(2u)).Success);
    }

    Y_UNIT_TEST(InvalidSampleRate) {
        TTestEnv env(1, 1, false);
        auto& runtime = *env.GetServer().GetRuntime();
        CreateDatabase(env, "Database");
        const auto table = CreateEmptyTable(env, "Database", "Table", true);
        for (double rate : {0.0, -0.1, 1.1, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
            const auto result = Sample(runtime, table, "invalid", rate);
            UNIT_ASSERT_VALUES_EQUAL(result.GetStatus(), NKikimrStat::TEvAnalyzeResponse::STATUS_ERROR);
            UNIT_ASSERT_STRING_CONTAINS(result.DebugString(), "finite number in (0, 1]");
        }
    }
}

Y_UNIT_TEST_SUITE(AnalyzeSampleCollection) {
    Y_UNIT_TEST(SamplePreservesFullRefreshBaseline) {
        TTestEnv env(1, 1, false);
        auto& runtime = *env.GetServer().GetRuntime();
        CreateDatabase(env, "Database");
        const auto table = PrepareSamplingRowTable(env, 1);
        WaitForRowCount(runtime, 1, table.PathId, 10);
        UNIT_ASSERT_VALUES_EQUAL(Sample(runtime, table, "full", 1.0).GetStatus(),
            NKikimrStat::TEvAnalyzeResponse::STATUS_SUCCESS);
        const auto readBaseline = [&] {
            const TString query = Sprintf(R"((
                (let key '('('OwnerId (Uint64 '%lu)) '('LocalPathId (Uint64 '%lu))))
                (let columns '('LastUpdateTime 'LastAnalyzeRowUpdates 'LastAnalyzeRowDeletes))
                (return (AsList (SetResult 'baseline (SelectRow 'ScheduleTraversals key columns))))
            ))", table.PathId.OwnerId, table.PathId.LocalPathId);
            NKikimrMiniKQL::TResult result;
            UNIT_ASSERT_VALUES_EQUAL(LocalQuery(runtime, table.SaTabletId, query, result), NKikimrProto::OK);
            UNIT_ASSERT_VALUES_EQUAL(result.GetValue().StructSize(), 1);
            UNIT_ASSERT(result.GetValue().GetStruct(0).GetOptional().HasOptional());
            return result.SerializeAsString();
        };
        const auto baseline = readBaseline();
        InsertDataIntoTable(env, "Database", "Table", 20, MultiColumnValueColumns());
        WaitForRowCount(runtime, 1, table.PathId, 20);
        runtime.SimulateSleep(TDuration::Seconds(1));
        UNIT_ASSERT_VALUES_EQUAL(Sample(runtime, table, "sample", 0.5).GetStatus(),
            NKikimrStat::TEvAnalyzeResponse::STATUS_SUCCESS);
        UNIT_ASSERT_VALUES_EQUAL(readBaseline(), baseline);
        const auto full = GetStatistics(runtime, table.PathId, EStatType::TABLE_SUMMARY, {std::nullopt});
        UNIT_ASSERT_VALUES_EQUAL(full.size(), 1);
        UNIT_ASSERT(full.front().Success && full.front().TableSummary.Data);
        UNIT_ASSERT_VALUES_EQUAL(full.front().TableSummary.Data->GetRowCount(), 10);
    }

    Y_UNIT_TEST(SinglePassStreamsGroupedRanges) {
        TTestEnv env(1, 1, false);
        auto& runtime = *env.GetServer().GetRuntime();
        CreateDatabase(env, "Database");
        const auto table = PrepareSamplingRowTable(env);
        size_t selects = 0;
        size_t hiveRequests = 0;
        size_t publications = 0;
        size_t resultBatches = 0;
        size_t partialReads = 0;
        THashSet<ui64> scannedShards;
        std::optional<ui64> seed;
        auto queries = runtime.AddObserver<NKqp::TEvKqp::TEvQueryRequest>([&](auto& ev) {
            if (!IsAnalyzeScan(ev)) {
                return;
            }
            ++selects;
            const auto& query = ev->Get()->GetQuery();
            UNIT_ASSERT_STRING_CONTAINS(query, "sampling_rate");
            UNIT_ASSERT_STRING_CONTAINS(query, "sampling_seed");
            UNIT_ASSERT_C(!query.Contains("HLL"), query);
        });
        auto hive = runtime.AddObserver<TEvHive::TEvRequestTabletDistribution>([&](auto&) {
            ++hiveRequests;
        });
        auto describes = runtime.AddObserver<NSchemeShard::TEvSchemeShard::TEvDescribeScheme>([](auto& ev) {
            UNIT_ASSERT(!ev->Get()->Record.GetOptions().GetReturnPartitionStats());
        });
        auto reads = runtime.AddObserver<TEvPipeCache::TEvForward>([&](auto& ev) {
            if (ev->Get()->Ev->Type() != TEvDataShard::TEvRead::EventType
                    || std::find(table.ShardIds.begin(), table.ShardIds.end(), ev->Get()->TabletId) == table.ShardIds.end()) {
                return;
            }
            auto& record = static_cast<TEvDataShard::TEvRead*>(ev->Get()->Ev.Get())->Record;
            UNIT_ASSERT(record.HasSampling());
            if (!seed) {
                seed = record.GetSampling().GetSeed();
            }
            UNIT_ASSERT_VALUES_EQUAL(record.GetSampling().GetSeed(), *seed);
            record.SetMaxRowsInResult(2);
            record.SetMaxRows(2);
            scannedShards.insert(ev->Get()->TabletId);
        });
        auto acks = runtime.AddObserver<TEvDataShard::TEvReadAck>([](auto& ev) {
            ev->Get()->Record.SetMaxRows(2);
        });
        auto readResults = runtime.AddObserver<TEvDataShard::TEvReadResult>([&](auto& ev) {
            if (ev->Get()->Record.HasSamplingStats() && !ev->Get()->Record.GetFinished()) {
                ++partialReads;
            }
        });
        TActorId aggregator;
        auto results = runtime.AddObserver<TEvStatistics::TEvAnalyzeActorResult>([&](auto& ev) {
            ++resultBatches;
            UNIT_ASSERT(ev->Get()->Final);
            aggregator = ev->GetRecipientRewrite();
        });
        auto saves = runtime.AddObserver<TEvStatistics::TEvSaveStatisticsQueryResponse>([&](auto& ev) {
            publications += ev->GetRecipientRewrite() == aggregator;
        });
        const double rate = std::nextafter(1.0, 0.0);
        const auto result = Sample(runtime, table, "single-pass", rate);
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NKikimrStat::TEvAnalyzeResponse::STATUS_SUCCESS, result.DebugString());
        reads.Remove();
        UNIT_ASSERT_VALUES_EQUAL(selects, (table.ShardIds.size() + SampledShardsPerScan - 1) / SampledShardsPerScan);
        UNIT_ASSERT_VALUES_EQUAL(scannedShards.size(), table.ShardIds.size());
        UNIT_ASSERT_VALUES_EQUAL(hiveRequests, 0);
        UNIT_ASSERT_VALUES_EQUAL(resultBatches, 1);
        UNIT_ASSERT_VALUES_EQUAL(publications, 1);
        UNIT_ASSERT_GT(partialReads, 0);
        const auto sample = ReadSample(runtime, table.PathId, EStatType::TABLE_SUMMARY);
        UNIT_ASSERT(sample.Success && sample.Sampling && sample.TableSummary.Data);
        UNIT_ASSERT(sample.Sampling->GetMethod() == NKikimrStat::TSamplingStatistics::PK_UNIT_BERNOULLI);
        UNIT_ASSERT_VALUES_EQUAL(sample.Sampling->GetRequestedRate(), rate);
        UNIT_ASSERT_VALUES_EQUAL(sample.Sampling->GetEffectiveRate(), rate);
        UNIT_ASSERT(seed);
        UNIT_ASSERT_VALUES_EQUAL(sample.Sampling->GetSeed(), *seed);
        UNIT_ASSERT(!sample.Sampling->HasEligibleUnits());
        UNIT_ASSERT(!sample.Sampling->HasSelectedUnits());
        UNIT_ASSERT_VALUES_EQUAL(sample.Sampling->GetSampleRows(), 130);
        UNIT_ASSERT_VALUES_EQUAL(sample.TableSummary.Data->GetRowCount(), 130);
        const auto simple = ReadSample(runtime, table.PathId, EStatType::SIMPLE_COLUMN, TColumnTags(2u));
        UNIT_ASSERT(simple.Success && simple.SimpleColumn.Data);
        UNIT_ASSERT_VALUES_EQUAL(simple.SimpleColumn.Data->GetCount(), 130);
        UNIT_ASSERT(!simple.SimpleColumn.Data->HasCountDistinct());
        const auto cms = ReadSample(runtime, table.PathId, EStatType::COUNT_MIN_SKETCH, TColumnTags(2u));
        UNIT_ASSERT(cms.Success && cms.CountMinSketch.CountMin);
        for (ui32 value = 0; value < 10; ++value) {
            const TString key = ToString(value);
            UNIT_ASSERT_VALUES_EQUAL(cms.CountMinSketch.CountMin->Probe(key.data(), key.size()), 13);
        }
    }

    Y_UNIT_TEST(SplitAtFullConcurrency) {
        constexpr ui32 maxSelects = 2;
        constexpr ui32 maxReaders = maxSelects * SampledShardsPerScan;
        TTestEnv env(1, 1, false, [](Tests::TServerSettings& settings) {
            settings.AppConfig->MutableStatisticsConfig()->SetAnalyzeMaxTotalScanActorsInFlight(maxSelects);
            settings.AppConfig->MutableStatisticsConfig()->SetAnalyzeMaxPerNodeScanActorsInFlight(1);
        });
        auto& runtime = *env.GetServer().GetRuntime();
        CreateDatabase(env, "Database");
        const auto table = PrepareSamplingRowTable(env, maxReaders + 1);
        for (ui32 node = 0; node < runtime.GetNodeCount(); ++node) {
            TControlBoard::SetValue(-1, runtime.GetAppData(node).Icb->SchemeShardControls.SplitMergePartCountLimit);
        }
        TBlockEvents<TEvDataShard::TEvReadAck> blockedAcks(runtime);
        THashMap<TActorId, THashMap<ui64, ui64>> readers;
        ui32 inFlight = 0;
        ui32 peakInFlight = 0;
        ui64 shardToSplit = 0;
        size_t selects = 0;
        size_t publications = 0;
        auto queries = runtime.AddObserver<NKqp::TEvKqp::TEvQueryRequest>([&](auto& ev) {
            if (IsAnalyzeScan(ev)) {
                ++selects;
            }
        });
        auto requests = runtime.AddObserver<TEvPipeCache::TEvForward>([&](auto& ev) {
            if (ev->Get()->Ev->Type() != TEvDataShard::TEvRead::EventType) {
                return;
            }
            auto& read = static_cast<TEvDataShard::TEvRead*>(ev->Get()->Ev.Get())->Record;
            if (!read.HasSampling()) {
                return;
            }
            read.SetMaxRowsInResult(2);
            read.SetMaxRows(2);
            UNIT_ASSERT(readers[ev->Sender].emplace(read.GetReadId(), ev->Get()->TabletId).second);
            peakInFlight = Max(peakInFlight, ++inFlight);
            UNIT_ASSERT_C(inFlight <= maxReaders, "Split exceeded the ANALYZE sampling concurrency limit");
            if (!shardToSplit) {
                shardToSplit = ev->Get()->TabletId;
            }
        });
        auto results = runtime.AddObserver<TEvDataShard::TEvReadResult>([&](auto& ev) {
            const auto& record = ev->Get()->Record;
            if (record.GetFinished() || record.GetStatus().GetCode() != Ydb::StatusIds::SUCCESS) {
                auto it = readers.find(ev->GetRecipientRewrite());
                if (it != readers.end() && it->second.erase(record.GetReadId())) {
                    --inFlight;
                }
            }
        });
        TActorId aggregator;
        auto statistics = runtime.AddObserver<TEvStatistics::TEvAnalyzeActorResult>([&](auto& ev) {
            aggregator = ev->GetRecipientRewrite();
        });
        auto saves = runtime.AddObserver<TEvStatistics::TEvSaveStatisticsQueryResponse>([&](auto& ev) {
            publications += ev->GetRecipientRewrite() == aggregator;
        });
        const auto sender = StartSample(runtime, table, "split", std::nextafter(1.0, 0.0));
        runtime.WaitFor("all sampled SELECTs and shard readers in flight", [&] {
            return blockedAcks.size() == maxReaders;
        });
        UNIT_ASSERT_VALUES_EQUAL(selects, maxSelects);
        UNIT_ASSERT_VALUES_EQUAL(inFlight, maxReaders);
        const auto shard = std::find(table.ShardIds.begin(), table.ShardIds.end(), shardToSplit);
        UNIT_ASSERT(shard != table.ShardIds.end());
        const ui64 boundary = (shard - table.ShardIds.begin()) * 10 + 5;
        const auto splitStatus = env.RunInThreadPool([&] {
            return env.GetClient().SplitTable(table.Path, shardToSplit, boundary);
        });
        UNIT_ASSERT_VALUES_EQUAL(splitStatus, NMsgBusProxy::MSTATUS_OK);
        blockedAcks.Stop().Unblock();
        const auto response = runtime.GrabEdgeEventRethrow<TEvStatistics::TEvAnalyzeResponse>(sender);
        UNIT_ASSERT_VALUES_EQUAL_C(response->Get()->Record.GetStatus(), NKikimrStat::TEvAnalyzeResponse::STATUS_SUCCESS,
            response->Get()->Record.DebugString());
        UNIT_ASSERT_VALUES_EQUAL(selects, 3);
        UNIT_ASSERT_VALUES_EQUAL(peakInFlight, maxReaders);
        UNIT_ASSERT_VALUES_EQUAL(inFlight, 0);
        UNIT_ASSERT_VALUES_EQUAL(publications, 1);
        const auto summary = ReadSample(runtime, table.PathId, EStatType::TABLE_SUMMARY);
        UNIT_ASSERT(summary.Success && summary.TableSummary.Data);
        UNIT_ASSERT_VALUES_EQUAL(summary.TableSummary.Data->GetRowCount(), (maxReaders + 1) * 10);
    }

    Y_UNIT_TEST(FinalQueryFailureDiscardsPartialResults) {
        TTestEnv env(1, 1, false);
        auto& runtime = *env.GetServer().GetRuntime();
        CreateDatabase(env, "Database");
        const auto table = PrepareSamplingRowTable(env);
        UNIT_ASSERT_VALUES_EQUAL(Sample(runtime, table, "previous", 0.5).GetStatus(),
            NKikimrStat::TEvAnalyzeResponse::STATUS_SUCCESS);
        const auto previous = ReadSample(runtime, table.PathId, EStatType::TABLE_SUMMARY);
        UNIT_ASSERT(previous.Success && previous.Sampling && previous.TableSummary.Data);
        THashSet<TActorId> scans;
        size_t finalResponses = 0;
        size_t streamedResponses = 0;
        size_t publications = 0;
        auto queries = runtime.AddObserver<NKqp::TEvKqp::TEvQueryRequest>([&](auto& ev) {
            if (IsAnalyzeScan(ev)) {
                scans.insert(ev->Sender);
            }
        });
        auto streams = runtime.AddObserver<NKqp::TEvKqpExecuter::TEvStreamData>([&](auto& ev) {
            if (scans.contains(ev->GetRecipientRewrite())) {
                ++streamedResponses;
            }
        });
        auto responses = runtime.AddObserver<NKqp::TEvKqp::TEvQueryResponse>([&](auto& ev) {
            if (scans.contains(ev->GetRecipientRewrite()) && ++finalResponses == 2) {
                UNIT_ASSERT_GT(streamedResponses, 0);
                ev->Get()->Record.SetYdbStatus(Ydb::StatusIds::TIMEOUT);
                ev->Get()->Record.MutableResponse()->AddQueryIssues()->set_message("injected query deadline");
            }
        });
        auto saves = runtime.AddObserver<TEvStatistics::TEvSaveStatisticsQueryResponse>([&](auto&) {
            ++publications;
        });
        const auto result = Sample(runtime, table, "failed", 0.25);
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NKikimrStat::TEvAnalyzeResponse::STATUS_ERROR, result.DebugString());
        UNIT_ASSERT_STRING_CONTAINS(result.DebugString(), "injected query deadline");
        UNIT_ASSERT_VALUES_EQUAL(finalResponses, 2);
        UNIT_ASSERT_VALUES_EQUAL(publications, 0);
        const auto current = ReadSample(runtime, table.PathId, EStatType::TABLE_SUMMARY);
        UNIT_ASSERT(current.Success && current.Sampling && current.TableSummary.Data);
        UNIT_ASSERT_VALUES_EQUAL(current.Sampling->SerializeAsString(), previous.Sampling->SerializeAsString());
        UNIT_ASSERT_VALUES_EQUAL(current.TableSummary.Data->SerializeAsString(), previous.TableSummary.Data->SerializeAsString());
    }

    Y_UNIT_TEST(WideTupleKeyClearsPreviousHistogram) {
        TTestEnv env(1, 1, false);
        auto& runtime = *env.GetServer().GetRuntime();
        CreateDatabase(env, "Database");
        const auto table = PrepareSamplingRowTable(env);
        const double rate = std::nextafter(1.0, 0.0);
        UNIT_ASSERT_VALUES_EQUAL(Sample(runtime, table, "narrow", rate).GetStatus(),
            NKikimrStat::TEvAnalyzeResponse::STATUS_SUCCESS);
        const TColumnTags tuple(std::vector<ui32>{2, 3});
        UNIT_ASSERT(ReadSample(runtime, table.PathId, EStatType::EQ_HEIGHT_HISTOGRAM, tuple).Success);
        ExecuteYqlScript(env, TStringBuilder()
            << "UPSERT INTO `/Root/Database/Table` (Key, Value1, Value2) VALUES (125ul, '"
            << TString(256, 'a') << "', '" << TString(256, 'b') << "');");
        UNIT_ASSERT_VALUES_EQUAL(Sample(runtime, table, "wide", rate).GetStatus(),
            NKikimrStat::TEvAnalyzeResponse::STATUS_SUCCESS);
        UNIT_ASSERT(!ReadSample(runtime, table.PathId, EStatType::EQ_HEIGHT_HISTOGRAM, tuple).Success);
        UNIT_ASSERT(!ReadSample(runtime, table.PathId, EStatType::EQ_HEIGHT_HISTOGRAM, TColumnTags(2u)).Success);
        UNIT_ASSERT(ReadSample(runtime, table.PathId, EStatType::SIMPLE_COLUMN, TColumnTags(2u)).Success);
        UNIT_ASSERT(ReadSample(runtime, table.PathId, EStatType::COUNT_MIN_SKETCH, TColumnTags(2u)).Success);
        const auto summary = ReadSample(runtime, table.PathId, EStatType::TABLE_SUMMARY);
        UNIT_ASSERT(summary.Success && summary.TableSummary.Data);
        UNIT_ASSERT_VALUES_EQUAL(summary.TableSummary.Data->GetRowCount(), 130);
    }

    Y_UNIT_TEST(TupleReservationsRejectBeforeScan) {
        TTestEnv env(1, 1, false);
        auto& runtime = *env.GetServer().GetRuntime();
        CreateDatabase(env, "Database");
        TStringBuilder create;
        create << "CREATE TABLE `/Root/Database/Table` (Key Uint64 NOT NULL";
        for (ui32 column = 1; column < 50; ++column) {
            create << ", Value" << column << " String";
        }
        create << ", PRIMARY KEY (Key));";
        ExecuteYqlScript(env, create);
        TTableInfo table;
        table.Path = "/Root/Database/Table";
        table.PathId = ResolvePathId(runtime, table.Path, &table.DomainKey, &table.SaTabletId);
        bool injected = false;
        auto declarations = runtime.AddObserver<TEvTxProxySchemeCache::TEvNavigateKeySetResult>([&](auto& ev) {
            for (auto& entry : ev->Get()->Request->ResultSet) {
                if (!entry.SyncVersion || entry.TableId.PathId != table.PathId) {
                    continue;
                }
                entry.MultiColumnStatistics.clear();
                for (ui32 first = 1; first <= 50; ++first) {
                    for (ui32 second = first + 1; second <= 50; ++second) {
                        auto& stat = entry.MultiColumnStatistics.emplace_back();
                        stat.SetName(TStringBuilder() << "tuple_" << first << '_' << second);
                        stat.AddColumnIds(first);
                        stat.AddColumnIds(second);
                        stat.AddTypes(NKikimrSchemeOp::EMultiColumnStatisticsType::EQ_HEIGHT_HISTOGRAM);
                    }
                }
                injected = true;
            }
        });
        size_t scans = 0;
        auto queries = runtime.AddObserver<NKqp::TEvKqp::TEvQueryRequest>([&](auto& ev) {
            if (IsAnalyzeScan(ev)) {
                ++scans;
            }
        });
        const auto result = Sample(runtime, table, "too-many-tuples", 0.5, {1});
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NKikimrStat::TEvAnalyzeResponse::STATUS_ERROR, result.DebugString());
        UNIT_ASSERT_STRING_CONTAINS(result.DebugString(),
            "ANALYZE SAMPLE result exceeds the single-publication limit; restrict the column list or drop declared tuple statistics.");
        UNIT_ASSERT(injected);
        UNIT_ASSERT_VALUES_EQUAL(scans, 0);
    }

    Y_UNIT_TEST(DeadlineDiscardsCompletedGroups) {
        TTestEnv env(1, 1, false, [](Tests::TServerSettings& settings) {
            settings.AppConfig->MutableStatisticsConfig()->SetAnalyzeMaxTotalScanActorsInFlight(1);
        });
        auto& runtime = *env.GetServer().GetRuntime();
        CreateDatabase(env, "Database");
        const auto table = PrepareSamplingRowTable(env, 13);
        TSaveStatisticsObserver saves(runtime, table.PathId);
        ui32 selects = 0;
        TBlockEvents<NKqp::TEvKqp::TEvQueryRequest> lastGroup(runtime, [&](const auto& ev) {
            if (!IsAnalyzeScan(ev)) {
                return false;
            }
            return ++selects == 2;
        });
        const auto sender = StartSample(runtime, table, "sample", std::nextafter(1.0, 0.0));
        // With one SELECT slot, the second group starts only after the first
        // group has succeeded and its intermediate statistics have been merged.
        runtime.WaitFor("second sample group", [&] { return !lastGroup.empty(); });
        UNIT_ASSERT_VALUES_EQUAL(selects, 2);
        runtime.AdvanceCurrentTime(TDuration::Days(1) + TDuration::Seconds(1));
        const auto result = runtime.GrabEdgeEventRethrow<TEvStatistics::TEvAnalyzeResponse>(sender);
        UNIT_ASSERT_VALUES_EQUAL(result->Get()->Record.GetStatus(), NKikimrStat::TEvAnalyzeResponse::STATUS_ERROR);
        UNIT_ASSERT_STRING_CONTAINS(result->Get()->Record.DebugString(), "deadline exceeded");
        UNIT_ASSERT_VALUES_EQUAL(saves.GetSaveCount(), 0);
        UNIT_ASSERT_VALUES_EQUAL(
            TestGetAnalyzeOp(runtime, table.SaTabletId, "/Root/Database", "sample")
                .GetAnalyzeOperation().GetState(), Ydb::Table::AnalyzeState::STATE_FAILED);

        lastGroup.Stop().Unblock();
        runtime.SimulateSleep(TDuration::Seconds(1));
        UNIT_ASSERT_VALUES_EQUAL(saves.GetSaveCount(), 0);
        UNIT_ASSERT(!ReadSample(runtime, table.PathId, EStatType::TABLE_SUMMARY).Success);
    }

    Y_UNIT_TEST(PublicationFinishesAcrossDeadline) {
        TTestEnv env(1, 1, false);
        auto& runtime = *env.GetServer().GetRuntime();
        CreateDatabase(env, "Database");
        const auto table = PrepareSamplingRowTable(env, 1);
        TActorId aggregator;
        auto results = runtime.AddObserver<TEvStatistics::TEvAnalyzeActorResult>([&](auto& ev) {
            aggregator = ev->GetRecipientRewrite();
        });
        TBlockEvents<TEvStatistics::TEvSaveStatisticsQueryResponse> commitResult(runtime, [&](const auto& ev) {
            return aggregator && ev->GetRecipientRewrite() == aggregator;
        });
        const auto sender = StartSample(runtime, table, "sample", std::nextafter(1.0, 0.0));
        runtime.WaitFor("atomic sample publication", [&] { return !commitResult.empty(); });
        UNIT_ASSERT(ReadSample(runtime, table.PathId, EStatType::TABLE_SUMMARY).Success);
        runtime.AdvanceCurrentTime(TDuration::Days(1) + TDuration::Seconds(1));
        runtime.SimulateSleep(TDuration::Seconds(2));
        TestCancelAnalyzeOp(runtime, table.SaTabletId, "/Root/Database", "sample",
            Ydb::StatusIds::PRECONDITION_FAILED);
        UNIT_ASSERT_VALUES_EQUAL(
            TestGetAnalyzeOp(runtime, table.SaTabletId, "/Root/Database", "sample")
                .GetAnalyzeOperation().GetState(), Ydb::Table::AnalyzeState::STATE_IN_PROGRESS);
        commitResult.Stop().Unblock();
        const auto result = runtime.GrabEdgeEventRethrow<TEvStatistics::TEvAnalyzeResponse>(sender);
        UNIT_ASSERT_VALUES_EQUAL(result->Get()->Record.GetStatus(), NKikimrStat::TEvAnalyzeResponse::STATUS_SUCCESS);
    }

    Y_UNIT_TEST(LostReaderAfterCompactionPublishesNothing) {
        TTestEnv env(1, 1, false);
        auto& runtime = *env.GetServer().GetRuntime();
        CreateDatabase(env, "Database");
        const auto table = PrepareSamplingRowTable(env, 1);
        TSaveStatisticsObserver saves(runtime, table.PathId);
        TActorId reader;
        ui32 reads = 0;
        auto requests = runtime.AddObserver<TEvPipeCache::TEvForward>([&](auto& ev) {
            if (ev->Get()->TabletId != table.ShardIds.front()
                    || ev->Get()->Ev->Type() != TEvDataShard::TEvRead::EventType) {
                return;
            }
            auto& record = static_cast<TEvDataShard::TEvRead*>(ev->Get()->Ev.Get())->Record;
            UNIT_ASSERT(record.HasSampling());
            record.SetMaxRowsInResult(2);
            record.SetMaxRows(2);
            reader = ev->Sender;
            ++reads;
        });
        TBlockEvents<TEvDataShard::TEvReadAck> acks(runtime);
        const auto sender = StartSample(runtime, table, "sample", std::nextafter(1.0, 0.0));
        runtime.WaitFor("sample reader yield", [&] { return !acks.empty(); });
        UNIT_ASSERT(reader);
        const auto compactor = runtime.AllocateEdgeActor();
        auto compact = std::make_unique<TEvDataShard::TEvCompactTable>(table.PathId);
        compact->Record.SetCompactSinglePartedShards(true);
        runtime.SendToPipe(table.ShardIds.front(), compactor, compact.release());
        const auto compacted = runtime.GrabEdgeEventRethrow<TEvDataShard::TEvCompactTableResult>(compactor);
        UNIT_ASSERT_VALUES_EQUAL(compacted->Get()->Record.GetStatus(), NKikimrTxDataShard::TEvCompactTableResult::OK);
        runtime.Send(reader, sender, new TEvPipeCache::TEvDeliveryProblem(table.ShardIds.front(), true),
            reader.NodeId() - runtime.GetNodeId(0));
        const auto result = runtime.GrabEdgeEventRethrow<TEvStatistics::TEvAnalyzeResponse>(sender);
        UNIT_ASSERT_VALUES_EQUAL(result->Get()->Record.GetStatus(), NKikimrStat::TEvAnalyzeResponse::STATUS_ERROR);
        UNIT_ASSERT_VALUES_EQUAL(reads, 1);
        UNIT_ASSERT_VALUES_EQUAL(saves.GetSaveCount(), 0);
        acks.Stop().Unblock();
        runtime.SimulateSleep(TDuration::Seconds(1));
        UNIT_ASSERT_VALUES_EQUAL(reads, 1);
        UNIT_ASSERT_VALUES_EQUAL(saves.GetSaveCount(), 0);
        UNIT_ASSERT(!ReadSample(runtime, table.PathId, EStatType::TABLE_SUMMARY).Success);
    }
}

} // namespace NKikimr::NStat
