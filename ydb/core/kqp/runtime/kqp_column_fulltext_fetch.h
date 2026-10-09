#pragma once

#include <ydb/core/kqp/compute_actor/kqp_compute_events.h>
#include <ydb/core/kqp/gateway/kqp_gateway.h>
#include <ydb/core/protos/kqp.pb.h>
#include <ydb/core/scheme/scheme_tabledefs.h>
#include <ydb/library/yql/dq/actors/protos/dq_status_codes.pb.h>

#include <functional>
#include <memory>
#include <vector>

#include <util/generic/hash.h>
#include <util/generic/hash_set.h>

namespace NKikimr::NSharding {
class IShardingBase;
}

namespace NKikimr::NKqp {

// Exact full-primary-key reads of a column table. Rows are correlated by the
// complete typed primary key. A scan is acknowledged to the caller only after
// every requested key of that scan has arrived, so a retry cannot emit twice.
class TColumnShardPkFetch {
public:
    struct TRow {
        TOwnedCellVec Cells;
    };

    struct TConfig {
        TTableId TableId;
        TString TablePath;
        TString Database;
        IKqpGateway::TKqpSnapshot Snapshot;
        ui64 LockTxId = 0;
        ui32 LockNodeId = 0;
        NKikimrDataEvents::ELockMode LockMode = NKikimrDataEvents::OPTIMISTIC;
        std::vector<NKikimrKqp::TKqpColumnMetadataProto> Columns;
        ui32 KeyColumnCount = 0;
    };

    using TRowsCallback = std::function<void(std::vector<TRow>)>;
    using TErrorCallback = std::function<void(NYql::NDqProto::StatusIds::StatusCode, const TString&)>;
    using TLocksCallback = std::function<void(const TVector<NKikimrDataEvents::TLock>&, const TVector<NKikimrDataEvents::TLock>&)>;

    TColumnShardPkFetch(TConfig config, const NActors::TActorId& selfId, TRowsCallback onRows, TErrorCallback onError, TLocksCallback onLocks);
    ~TColumnShardPkFetch();

    // One wave of point reads. The caller waits until InFlightScans() == 0
    // before submitting the next wave.
    void Submit(std::vector<TOwnedCellVec> keys);
    ui32 InFlightScans() const {
        return InFlight;
    }
    bool Idle() const {
        return InFlight == 0 && !Resolving && PendingKeys.empty();
    }
    void Cancel();

    void HandleNavigate(TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr& ev);
    void HandleScanInit(TEvKqpCompute::TEvScanInitActor::TPtr& ev);
    void HandleScanData(TEvKqpCompute::TEvScanData::TPtr& ev);
    void HandleScanError(TEvKqpCompute::TEvScanError::TPtr& ev);
    bool HandleDeliveryProblem(ui64 tabletId);
    void RetryScan(ui64 scanId);

private:
    struct TScan {
        ui64 ShardId = 0;
        ui32 Generation = 0;
        ui32 Attempts = 0;
        NActors::TActorId ScanActor;
        bool Finished = false;
        std::vector<TOwnedCellVec> Keys;
        std::vector<TString> KeyTokens;
        THashSet<TString> Returned;
        std::vector<TRow> Rows;
    };

    void Fail(NYql::NDqProto::StatusIds::StatusCode status, const TString& message);
    void Resolve();
    void StartScans(std::vector<TOwnedCellVec> keys);
    void SendScan(ui64 shardId, std::vector<TOwnedCellVec> keys);
    void FinishScan(ui64 scanId);
    void ScheduleRetry(ui64 scanId, bool allowInstant);
    TString KeyToken(TConstArrayRef<TCell> key) const;

    TConfig Config;
    const NActors::TActorId SelfId;
    const NActors::TActorId PipeCacheId;
    TRowsCallback OnRows;
    TErrorCallback OnError;
    TLocksCallback OnLocks;

    std::unique_ptr<NSharding::IShardingBase> Sharding;
    bool Resolving = false;
    bool Failed = false;
    bool Cancelled = false;
    ui32 ResolveAttempts = 0;
    ui32 InFlight = 0;
    THashMap<ui64, ui32> ShardAttempts;
    ui64 NextScanId = 0;
    std::vector<TOwnedCellVec> PendingKeys;
    THashMap<ui64, TScan> Scans;
};

}
