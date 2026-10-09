#include "snapshot_scheme.h"
#include "versioned_index.h"

#include <ydb/core/tx/columnshard/engines/db_wrapper.h>
#include <ydb/core/tx/columnshard/engines/scheme/index_info.h>

#include <ydb/library/actors/core/log.h>

#define YDB_LOG_THIS_FILE_COMPONENT NKikimrServices::TX_COLUMNSHARD

namespace NKikimr::NOlap {

const TIndexInfo* TVersionedIndex::AddIndex(const TSnapshot& snapshot, TObjectCache<TSchemaVersionId, TIndexInfo>::TEntryGuard&& indexInfo) {
    if (SnapshotByVersion.empty()) {
        PrimaryKey = indexInfo->GetPrimaryKey();
    } else {
        Y_ABORT_UNLESS(PrimaryKey->Equals(indexInfo->GetPrimaryKey()));
    }

    const bool needActualization = indexInfo->GetSchemeNeedActualization();
    auto newVersion = indexInfo->GetVersion();
    auto itVersion =
        SnapshotByVersion.emplace(newVersion, TSchemaInfoByVersion(std::make_shared<TSnapshotSchema>(std::move(indexInfo), snapshot)));
    AFL_VERIFY(itVersion.second)("message", "duplication for registered version")("version", LastSchemaVersion);
    // Older schema objects stay in SnapshotByVersion, so an in-flight scan keeps the
    // index identity it started with. The scheme actualizer queues portions older than
    // this version; an empty queue is not a promise that every portion has postings.
    if (needActualization) {
        if (!SchemeVersionForActualization || *SchemeVersionForActualization < newVersion) {
            SchemeVersionForActualization = newVersion;
            SchemeForActualization = itVersion.first->second.GetSchema();
            YDB_LOG_INFO("",
                {"event", "scheme_actualization_scheduled"},
                {"version", newVersion},
                {"snapshot", snapshot.DebugString()});
        }
    }
    auto itSnap = Snapshots.emplace(snapshot, itVersion.first->second.GetSchema());
    Y_ABORT_UNLESS(itSnap.second);
    LastSchemaVersion = std::max(newVersion, LastSchemaVersion);
    return &itVersion.first->second->GetIndexInfo();
}

bool TVersionedIndex::LoadShardingInfo(IDbWrapper& db) {
    TConclusion<THashMap<TInternalPathId, std::map<TSnapshot, TGranuleShardingInfo>>> shardingLocal = db.LoadGranulesShardingInfo();
    if (shardingLocal.IsFail()) {
        return false;
    }
    ShardingInfo = std::move(shardingLocal.DetachResult());
    return true;
}

std::optional<NKikimr::NOlap::TGranuleShardingInfo> TVersionedIndex::GetShardingInfoActual(const TInternalPathId pathId) const {
    auto it = ShardingInfo.find(pathId);
    if (it == ShardingInfo.end() || it->second.empty()) {
        return std::nullopt;
    } else {
        return it->second.rbegin()->second;
    }
}

}   // namespace NKikimr::NOlap
