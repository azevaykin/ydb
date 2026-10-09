#include "kqp_column_fulltext_fetch.h"

#include <ydb/core/base/tablet_pipecache.h>
#include <ydb/core/formats/arrow/arrow_batch_builder.h>
#include <ydb/core/formats/arrow/converter.h>
#include <ydb/core/kqp/runtime/kqp_read_iterator_common.h>
#include <ydb/core/scheme/scheme_types_proto.h>
#include <ydb/core/tx/datashard/datashard.h>
#include <ydb/core/tx/datashard/range_ops.h>
#include <ydb/core/tx/scheme_cache/scheme_cache.h>
#include <ydb/core/tx/schemeshard/olap/schema/schema.h>
#include <ydb/core/tx/sharding/sharding.h>
#include <ydb/library/actors/core/actor.h>
#include <ydb/library/actors/core/events.h>
#include <ydb/library/formats/arrow/arrow_helpers.h>
#include <ydb/library/formats/arrow/validation/validation.h>
#include <yql/essentials/public/issue/yql_issue_message.h>

#include <util/string/builder.h>

namespace NKikimr::NKqp {

namespace {

constexpr ui32 MaxInFlightScans = 4;
constexpr size_t MaxKeysPerScan = 128;

NScheme::TTypeInfo ColumnType(const NKikimrKqp::TKqpColumnMetadataProto& column) {
    return NScheme::TypeInfoFromProto(column.GetTypeId(), column.GetTypeInfo());
}

}

TColumnShardPkFetch::~TColumnShardPkFetch() = default;

TColumnShardPkFetch::TColumnShardPkFetch(
        TConfig config,
        const NActors::TActorId& selfId,
        TRowsCallback onRows,
        TErrorCallback onError,
        TLocksCallback onLocks)
    : Config(std::move(config))
    , SelfId(selfId)
    , PipeCacheId(MakePipePerNodeCacheID(false))
    , OnRows(std::move(onRows))
    , OnError(std::move(onError))
    , OnLocks(std::move(onLocks))
{}

TString TColumnShardPkFetch::KeyToken(TConstArrayRef<TCell> key) const {
    return TSerializedCellVec::Serialize(key);
}

void TColumnShardPkFetch::Fail(NYql::NDqProto::StatusIds::StatusCode status, const TString& message) {
    if (Failed || Cancelled) {
        return;
    }
    Failed = true;
    Scans.clear();
    PendingKeys.clear();
    InFlight = 0;
    OnError(status, message);
}

void TColumnShardPkFetch::Cancel() {
    Cancelled = true;
    Scans.clear();
    PendingKeys.clear();
    InFlight = 0;
}

void TColumnShardPkFetch::Submit(std::vector<TOwnedCellVec> keys) {
    if (Failed || Cancelled || keys.empty()) {
        return;
    }
    if (!Sharding) {
        PendingKeys.insert(PendingKeys.end(), std::make_move_iterator(keys.begin()), std::make_move_iterator(keys.end()));
        if (!Resolving) {
            Resolve();
        }
        return;
    }
    StartScans(std::move(keys));
}

void TColumnShardPkFetch::Resolve() {
    Resolving = true;
    ++ResolveAttempts;
    auto request = std::make_unique<NSchemeCache::TSchemeCacheNavigate>();
    request->DatabaseName = Config.Database;
    NSchemeCache::TSchemeCacheNavigate::TEntry entry;
    entry.TableId = Config.TableId;
    entry.RequestType = NSchemeCache::TSchemeCacheNavigate::TEntry::ERequestType::ByTableId;
    entry.Operation = NSchemeCache::TSchemeCacheNavigate::OpTable;
    entry.SyncVersion = false;
    entry.ShowPrivatePath = true;
    request->ResultSet.emplace_back(std::move(entry));
    TActivationContext::Send(new IEventHandle(
        MakeSchemeCacheID(), SelfId, new TEvTxProxySchemeCache::TEvNavigateKeySet(request.release())));
}

void TColumnShardPkFetch::HandleNavigate(TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr& ev) {
    if (Failed || Cancelled) {
        return;
    }
    Resolving = false;
    auto* response = ev->Get()->Request.Get();
    if (response->ErrorCount > 0 || response->ResultSet.size() != 1) {
        Fail(NYql::NDqProto::StatusIds::SCHEME_ERROR,
            TStringBuilder() << "Failed to resolve column table `" << Config.TablePath << "`.");
        return;
    }
    const auto& entry = response->ResultSet[0];
    if (entry.Kind != NSchemeCache::TSchemeCacheNavigate::KindColumnTable || !entry.ColumnTableInfo) {
        Fail(NYql::NDqProto::StatusIds::SCHEME_ERROR,
            TStringBuilder() << "Table `" << Config.TablePath << "` is not a column table.");
        return;
    }
    if (entry.TableId.SchemaVersion != 0 && Config.TableId.SchemaVersion != 0
        && entry.TableId.SchemaVersion != Config.TableId.SchemaVersion)
    {
        Fail(NYql::NDqProto::StatusIds::SCHEME_ERROR,
            TStringBuilder() << "Column table `" << Config.TablePath << "` schema changed.");
        return;
    }
    const auto& description = entry.ColumnTableInfo->Description;
    if (!description.HasSchema() || !description.HasSharding()) {
        Fail(NYql::NDqProto::StatusIds::SCHEME_ERROR,
            TStringBuilder() << "Column table `" << Config.TablePath << "` has no sharding.");
        return;
    }
    NSchemeShard::TOlapSchema olapSchema;
    olapSchema.ParseFromLocalDB(description.GetSchema());
    auto sharding = NSharding::IShardingBase::BuildFromProto(olapSchema, description.GetSharding());
    if (sharding.IsFail()) {
        Fail(NYql::NDqProto::StatusIds::SCHEME_ERROR,
            TStringBuilder() << "Failed to build column sharding for `" << Config.TablePath << "`: " << sharding.GetErrorMessage());
        return;
    }
    Sharding = sharding.DetachResult();
    if (!Sharding) {
        Fail(NYql::NDqProto::StatusIds::SCHEME_ERROR,
            TStringBuilder() << "Column table `" << Config.TablePath << "` has empty sharding.");
        return;
    }
    auto pending = std::move(PendingKeys);
    PendingKeys.clear();
    if (!pending.empty()) {
        StartScans(std::move(pending));
    }
}

void TColumnShardPkFetch::StartScans(std::vector<TOwnedCellVec> keys) {
    if (Failed || Cancelled || keys.empty()) {
        return;
    }
    if (!Sharding) {
        PendingKeys.insert(PendingKeys.end(), std::make_move_iterator(keys.begin()), std::make_move_iterator(keys.end()));
        if (!Resolving) {
            Resolve();
        }
        return;
    }

    std::vector<std::pair<TString, NScheme::TTypeInfo>> keySchema;
    std::set<std::string> notNull;
    keySchema.reserve(Config.KeyColumnCount);
    for (ui32 i = 0; i < Config.KeyColumnCount; ++i) {
        keySchema.emplace_back(Config.Columns[i].GetName(), ColumnType(Config.Columns[i]));
        notNull.insert(Config.Columns[i].GetName());
    }
    NArrow::TArrowBatchBuilder builder(arrow::Compression::UNCOMPRESSED, notNull, arrow::default_memory_pool());
    TString error;
    if (!builder.Start(keySchema, 0, 0, error)) {
        Fail(NYql::NDqProto::StatusIds::INTERNAL_ERROR, error);
        return;
    }
    for (const auto& key : keys) {
        builder.AddRow(TConstArrayRef<TCell>(key));
    }
    auto keyBatch = builder.FlushBatch(true, true);
    auto shards = Sharding->MakeSharding(keyBatch);
    std::vector<bool> assigned(keys.size(), false);
    for (const auto& [shardId, indexes] : shards) {
        std::vector<TOwnedCellVec> shardKeys;
        shardKeys.reserve(indexes.size());
        for (const ui32 index : indexes) {
            if (index >= keys.size() || assigned[index]) {
                Fail(NYql::NDqProto::StatusIds::INTERNAL_ERROR, "Column sharding returned an unexpected key.");
                return;
            }
            assigned[index] = true;
            shardKeys.push_back(keys[index]);
        }
        while (!shardKeys.empty() && InFlight < MaxInFlightScans) {
            const size_t count = std::min(shardKeys.size(), MaxKeysPerScan);
            std::vector<TOwnedCellVec> slice;
            slice.insert(slice.end(), std::make_move_iterator(shardKeys.end() - count), std::make_move_iterator(shardKeys.end()));
            shardKeys.erase(shardKeys.end() - count, shardKeys.end());
            SendScan(shardId, std::move(slice));
        }
        if (!shardKeys.empty()) {
            PendingKeys.insert(PendingKeys.end(), std::make_move_iterator(shardKeys.begin()), std::make_move_iterator(shardKeys.end()));
        }
    }
    for (const bool seen : assigned) {
        if (!seen) {
            Fail(NYql::NDqProto::StatusIds::INTERNAL_ERROR,
                TStringBuilder() << "Column sharding did not cover a primary key of `" << Config.TablePath << "`.");
            return;
        }
    }
}

void TColumnShardPkFetch::SendScan(ui64 shardId, std::vector<TOwnedCellVec> keys) {
    const ui64 scanId = ++NextScanId;
    auto ev = std::make_unique<TEvDataShard::TEvKqpScan>();
    ev->Record.SetLocalPathId(Config.TableId.PathId.LocalPathId);
    ev->Record.SetTablePath(Config.TablePath);
    ev->Record.SetSchemaVersion(Config.TableId.SchemaVersion);
    ev->Record.SetScanId(scanId);
    ev->Record.SetGeneration(1);
    ev->Record.SetTxId(Config.LockTxId ? Config.LockTxId : Config.Snapshot.TxId);
    if (Config.LockTxId) {
        ev->Record.SetLockTxId(Config.LockTxId);
        ev->Record.SetLockNodeId(Config.LockNodeId);
        ev->Record.SetLockMode(Config.LockMode);
    }
    if (Config.Snapshot.IsValid()) {
        ev->Record.MutableSnapshot()->SetStep(Config.Snapshot.Step);
        ev->Record.MutableSnapshot()->SetTxId(Config.Snapshot.TxId);
    }
    ev->Record.SetDataFormat(NKikimrDataEvents::FORMAT_ARROW);
    ev->Record.SetReverse(false);
    for (const auto& column : Config.Columns) {
        ev->Record.AddColumnTags(column.GetId());
        ev->Record.AddColumnTypes(column.GetTypeId());
        if (column.HasTypeInfo()) {
            *ev->Record.AddColumnTypeInfos() = column.GetTypeInfo();
        } else {
            *ev->Record.AddColumnTypeInfos() = NKikimrProto::TTypeInfo();
        }
    }
    auto* ranges = ev->Record.MutableRanges();
    TScan scan;
    scan.ShardId = shardId;
    scan.Keys = keys;
    scan.KeyTokens.reserve(keys.size());
    for (const auto& key : keys) {
        scan.KeyTokens.push_back(KeyToken(key));
        TSerializedTableRange range(key, true, key, true);
        range.Point = true;
        range.Serialize(*ranges->Add());
    }
    Scans.emplace(scanId, std::move(scan));
    ++InFlight;
    TActivationContext::Send(new IEventHandle(
        PipeCacheId, SelfId, new TEvPipeCache::TEvForward(ev.release(), shardId, true),
        IEventHandle::FlagTrackDelivery, scanId));
}

void TColumnShardPkFetch::HandleScanInit(TEvKqpCompute::TEvScanInitActor::TPtr& ev) {
    if (Failed || Cancelled) {
        return;
    }
    auto it = Scans.find(ev->Get()->Record.GetScanId());
    if (it == Scans.end() || it->second.Finished) {
        return;
    }
    it->second.Generation = ev->Get()->Record.GetGeneration();
    it->second.ScanActor = ActorIdFromProto(ev->Get()->Record.GetScanActorId());
    TActivationContext::Send(new IEventHandle(
        it->second.ScanActor, SelfId, new TEvKqpCompute::TEvScanDataAck(8ull << 20, it->second.Generation)));
}

void TColumnShardPkFetch::HandleScanData(TEvKqpCompute::TEvScanData::TPtr& ev) {
    if (Failed || Cancelled) {
        return;
    }
    auto it = Scans.find(ev->Get()->ScanId);
    if (it == Scans.end() || it->second.Finished) {
        return;
    }
    auto& scan = it->second;
    if (scan.ScanActor && ev->Get()->Generation != scan.Generation) {
        return;
    }
    if (OnLocks && (!ev->Get()->LocksInfo.Locks.empty() || !ev->Get()->LocksInfo.BrokenLocks.empty())) {
        OnLocks(ev->Get()->LocksInfo.Locks, ev->Get()->LocksInfo.BrokenLocks);
        if (Failed) {
            return;
        }
    }

    auto consume = [&](TConstArrayRef<TCell> cells) {
        if (cells.size() != Config.Columns.size()) {
            Fail(NYql::NDqProto::StatusIds::INTERNAL_ERROR, "Column fetch returned an unexpected column count.");
            return;
        }
        const auto key = cells.subspan(0, Config.KeyColumnCount);
        const TString token = KeyToken(key);
        if (!scan.Returned.insert(token).second) {
            Fail(NYql::NDqProto::StatusIds::INTERNAL_ERROR, "Column fetch returned a duplicate primary key.");
            return;
        }
        bool expected = false;
        for (const auto& keyToken : scan.KeyTokens) {
            if (keyToken == token) {
                expected = true;
                break;
            }
        }
        if (!expected) {
            Fail(NYql::NDqProto::StatusIds::INTERNAL_ERROR, "Column fetch returned an unexpected primary key.");
            return;
        }
        scan.Rows.push_back(TRow{TOwnedCellVec(cells)});
    };

    if (!ev->Get()->Rows.empty()) {
        for (const auto& row : ev->Get()->Rows) {
            consume(row);
            if (Failed) {
                return;
            }
        }
    } else if (ev->Get()->ArrowBatch && ev->Get()->ArrowBatch->num_rows() > 0) {
        std::vector<std::pair<TString, NScheme::TTypeInfo>> schema;
        schema.reserve(Config.Columns.size());
        for (const auto& column : Config.Columns) {
            schema.emplace_back(column.GetName(), ColumnType(column));
        }
        auto fields = NArrow::TStatusValidator::GetValid(NArrow::MakeArrowFields(schema, {}));
        auto renamedSchema = std::make_shared<arrow::Schema>(std::move(fields));
        for (const auto& batch : NArrow::SliceToRecordBatches(ev->Get()->ArrowBatch)) {
            auto renamed = arrow::RecordBatch::Make(renamedSchema, batch->num_rows(), batch->columns());
            struct TCopyRows : NArrow::IRowWriter {
                explicit TCopyRows(std::function<void(TConstArrayRef<TCell>)> consume)
                    : Consume(std::move(consume))
                {
                }

                std::function<void(TConstArrayRef<TCell>)> Consume;
                void AddRow(const TConstArrayRef<TCell>& cells) override {
                    Consume(cells);
                }
            } writer(consume);
            NArrow::TArrowToYdbConverter converter(schema, writer, false, false);
            TString error;
            if (!converter.Process(*renamed, error)) {
                Fail(NYql::NDqProto::StatusIds::INTERNAL_ERROR, TStringBuilder() << "Cannot read column batch: " << error);
                return;
            }
            if (Failed) {
                return;
            }
        }
    }

    if (!ev->Get()->Finished) {
        const auto actor = scan.ScanActor ? scan.ScanActor : ev->Sender;
        TActivationContext::Send(new IEventHandle(
            actor, SelfId, new TEvKqpCompute::TEvScanDataAck(8ull << 20, scan.Generation)));
        return;
    }
    FinishScan(it->first);
}

void TColumnShardPkFetch::FinishScan(ui64 scanId) {
    auto it = Scans.find(scanId);
    if (it == Scans.end()) {
        return;
    }
    auto scan = std::move(it->second);
    Scans.erase(it);
    if (InFlight > 0) {
        --InFlight;
    }
    if (scan.Returned.size() != scan.KeyTokens.size()) {
        Fail(NYql::NDqProto::StatusIds::ABORTED,
            TStringBuilder() << "Indexed document is missing from the column table `" << Config.TablePath << "`.");
        return;
    }
    ShardAttempts[scan.ShardId] = 0;
    OnRows(std::move(scan.Rows));
    if (Failed || Cancelled) {
        return;
    }
    if (InFlight < MaxInFlightScans && !PendingKeys.empty() && Sharding) {
        auto pending = std::move(PendingKeys);
        PendingKeys.clear();
        StartScans(std::move(pending));
    }
}

void TColumnShardPkFetch::ScheduleRetry(ui64 scanId, bool allowInstant) {
    auto it = Scans.find(scanId);
    if (it == Scans.end()) {
        return;
    }
    const ui64 shardId = it->second.ShardId;
    auto keys = std::move(it->second.Keys);
    Scans.erase(it);
    if (InFlight > 0) {
        --InFlight;
    }
    const ui32 attempts = ++ShardAttempts[shardId];
    if (attempts > MaxShardRetries()) {
        Fail(NYql::NDqProto::StatusIds::UNAVAILABLE,
            TStringBuilder() << "ColumnShard " << shardId << " is unavailable for `" << Config.TablePath << "`.");
        return;
    }
    PendingKeys.insert(PendingKeys.end(), std::make_move_iterator(keys.begin()), std::make_move_iterator(keys.end()));
    const TDuration delay = CalcDelay(attempts, allowInstant);
    if (delay <= TDuration::Zero()) {
        RetryScan(0);
        return;
    }
    TActivationContext::Schedule(delay, new IEventHandle(SelfId, SelfId, new TEvents::TEvWakeup()));
}

void TColumnShardPkFetch::RetryScan(ui64 scanId) {
    Y_UNUSED(scanId);
    if (Failed || Cancelled || Resolving || PendingKeys.empty()) {
        return;
    }
    if (!Sharding) {
        Resolve();
        return;
    }
    if (InFlight >= MaxInFlightScans) {
        return;
    }
    auto pending = std::move(PendingKeys);
    PendingKeys.clear();
    StartScans(std::move(pending));
}

void TColumnShardPkFetch::HandleScanError(TEvKqpCompute::TEvScanError::TPtr& ev) {
    if (Failed || Cancelled) {
        return;
    }
    const auto status = ev->Get()->Record.GetStatus();
    NYql::TIssues issues;
    NYql::IssuesFromMessage(ev->Get()->Record.GetIssues(), issues);
    const TString message = issues.ToOneLineString();
    ui64 scanId = 0;
    for (const auto& [id, scan] : Scans) {
        if (ev->Get()->Record.GetTabletId() == 0 || scan.ShardId == ev->Get()->Record.GetTabletId()) {
            scanId = id;
            break;
        }
    }
    switch (status) {
        case Ydb::StatusIds::OVERLOADED:
        case Ydb::StatusIds::UNAVAILABLE:
        case Ydb::StatusIds::TIMEOUT:
        case Ydb::StatusIds::INTERNAL_ERROR:
        case Ydb::StatusIds::NOT_FOUND:
        case Ydb::StatusIds::UNDETERMINED:
            if (status == Ydb::StatusIds::NOT_FOUND) {
                if (ResolveAttempts >= MaxShardResolves()) {
                    Fail(NYql::NDqProto::StatusIds::UNAVAILABLE,
                        TStringBuilder() << "ColumnShard read failed for `" << Config.TablePath << "`. " << message);
                    return;
                }
                Sharding.reset();
            }
            if (scanId != 0) {
                ScheduleRetry(scanId, status != Ydb::StatusIds::OVERLOADED);
            }
            return;
        case Ydb::StatusIds::SCHEME_ERROR:
        case Ydb::StatusIds::PRECONDITION_FAILED:
            Fail(NYql::NDqProto::StatusIds::SCHEME_ERROR,
                TStringBuilder() << "Column table `" << Config.TablePath << "` schema or shard changed. " << message);
            return;
        default:
            Fail(NYql::NDqProto::StatusIds::ABORTED,
                TStringBuilder() << "ColumnShard read failed for `" << Config.TablePath << "`. " << message);
            return;
    }
}

bool TColumnShardPkFetch::HandleDeliveryProblem(ui64 tabletId) {
    if (Failed || Cancelled) {
        return false;
    }
    std::vector<ui64> scanIds;
    for (const auto& [scanId, scan] : Scans) {
        if (scan.ShardId == tabletId) {
            scanIds.push_back(scanId);
        }
    }
    if (scanIds.empty()) {
        return false;
    }
    for (const ui64 scanId : scanIds) {
        ScheduleRetry(scanId, true);
    }
    return true;
}

}
