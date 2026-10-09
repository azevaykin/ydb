#pragma once
#include "abstract.h"
#include "checker.h"
#include "collection.h"

#include <ydb/core/protos/flat_scheme_op.pb.h>
#include <ydb/core/tx/columnshard/engines/storage/chunks/data.h>
#include <ydb/core/tx/columnshard/splitter/chunks.h>

#include <ydb/library/conclusion/status.h>
#include <ydb/services/bg_tasks/abstract/interface.h>

#include <library/cpp/object_factory/object_factory.h>

#include <limits>

namespace NYql::NNodes {
class TExprBase;
}

namespace NKikimr::NOlap {
struct TIndexInfo;
class TIndexChunk;

namespace NReader::NCommon {
class IKernelFetchLogic;
}
}   // namespace NKikimr::NOlap

namespace NKikimr::NSchemeShard {
class TOlapSchema;
}

namespace NKikimr::NOlap::NIndexes {

// Resolved by AppendIndex before an optional index builder runs.
// Chunk byte limit is the target storage blob limit. The construction budget counts memory the
// builder still holds after a chunk is closed.
struct TIndexBuildContext {
    i64 MaxChunkBytes = 0;
    TString TargetTier;
    ui64 ConstructionMemoryBudget = std::numeric_limits<ui64>::max();
    ui64 AnalyzerMaxInputBytes = std::numeric_limits<ui64>::max();
    ui64 AnalyzerMaxGeneratedTokens = std::numeric_limits<ui64>::max();
    ui64 AnalyzerMaxRetainedBytes = std::numeric_limits<ui64>::max();
};

// Built carries chunks whose record counts cover the portion.
// Skipped omits the index and keeps the portion. An empty chunk vector is not a skip:
// callers still require Built chunks to cover every portion row.
class TIndexBuildOutcome {
public:
    static TIndexBuildOutcome Built(std::vector<std::shared_ptr<NChunks::TPortionIndexChunk>> chunks) {
        TIndexBuildOutcome result;
        result.Skipped_ = false;
        result.Chunks = std::move(chunks);
        return result;
    }

    static TIndexBuildOutcome Skipped(TString reason) {
        TIndexBuildOutcome result;
        result.Skipped_ = true;
        result.Reason = std::move(reason);
        return result;
    }

    bool IsSkipped() const {
        return Skipped_;
    }

    bool IsBuilt() const {
        return !Skipped_;
    }

    const TString& GetSkipReason() const {
        return Reason;
    }

    const std::vector<std::shared_ptr<NChunks::TPortionIndexChunk>>& GetChunks() const {
        return Chunks;
    }

    std::vector<std::shared_ptr<NChunks::TPortionIndexChunk>> DetachChunks() {
        return std::move(Chunks);
    }

private:
    bool Skipped_ = false;
    std::vector<std::shared_ptr<NChunks::TPortionIndexChunk>> Chunks;
    TString Reason;
};

namespace NRequest {
class TLikePart {
public:
    enum class EOperation {
        StartsWith,
        EndsWith,
        Contains,
        Equals
    };
};
}   // namespace NRequest

class IIndexMeta {
private:
    YDB_READONLY_DEF(TString, IndexName);
    YDB_READONLY(ui32, IndexId, 0);
    YDB_READONLY(TString, DefaultStorageId, IStoragesManager::DefaultStorageId);
    YDB_READONLY_DEF(bool, InheritPortionStorage);

    virtual std::shared_ptr<NReader::NCommon::IKernelFetchLogic> DoBuildFetchTask(const THashSet<NRequest::TOriginalDataAddress>& dataAddresses,
        const std::shared_ptr<IIndexMeta>& selfPtr, const std::shared_ptr<IStoragesManager>& storagesManager) const;

    virtual TConclusion<std::shared_ptr<IIndexHeader>> DoBuildHeader(const TChunkOriginalData& data) const {
        return std::make_shared<TDefaultHeader>(data.GetSize());
    }

    virtual std::optional<ui64> DoCalcCategory(const NArrow::NAccessor::NSubColumns::TCanonicalSubColumnName& /*subColumnName*/) const {
        return std::nullopt;
    }

protected:
    virtual TConclusion<TIndexBuildOutcome> DoBuildIndexOptional(
        const THashMap<ui32, std::vector<std::shared_ptr<IPortionDataChunk>>>& data, const ui32 recordsCount,
        const TIndexInfo& indexInfo, const TIndexBuildContext& context) const = 0;
    virtual bool DoDeserializeFromProto(const NKikimrSchemeOp::TOlapIndexDescription& proto) = 0;
    virtual void DoSerializeToProto(NKikimrSchemeOp::TOlapIndexDescription& proto) const = 0;
    virtual TConclusionStatus DoCheckModificationCompatibility(const IIndexMeta& newMeta) const = 0;

    virtual NJson::TJsonValue DoSerializeDataToJson(const TString& /*data*/, const TIndexInfo& /*indexInfo*/) const {
        return "NO_IMPLEMENTED";
    }

public:
    using TFactory = NObjectFactory::TObjectFactory<IIndexMeta, TString>;
    using TProto = NKikimrSchemeOp::TOlapIndexDescription;

    virtual bool IsSkipIndex() const {
        return false;
    }

    virtual std::optional<ui32> GetSingleColumnId() const {
        return std::nullopt;
    }

    std::optional<ui64> CalcCategory(const NArrow::NAccessor::NSubColumns::TCanonicalSubColumnName& subColumnName) const;

    TConclusion<std::shared_ptr<IIndexHeader>> BuildHeader(const TChunkOriginalData& data) const {
        return DoBuildHeader(data);
    }

    std::shared_ptr<NReader::NCommon::IKernelFetchLogic> BuildFetchTask(const THashSet<NRequest::TOriginalDataAddress>& dataAddresses,
        const std::shared_ptr<IIndexMeta>& meta, const std::shared_ptr<IStoragesManager>& storagesManager) const {
        return DoBuildFetchTask(dataAddresses, meta, storagesManager);
    }

    bool IsInplaceData(const TString& specialTier) const {
        if (InheritPortionStorage && specialTier && specialTier != NBlobOperations::TGlobal::DefaultStorageId) {
            return false;
        }
        return DefaultStorageId == NBlobOperations::TGlobal::LocalMetadataStorageId;
    }

    IIndexMeta() = default;

    IIndexMeta(const ui32 indexId, const TString& indexName, const TString& storageId, const bool inheritPortionStorage)
        : IndexName(indexName)
        , IndexId(indexId)
        , DefaultStorageId(storageId)
        , InheritPortionStorage(inheritPortionStorage)
    {
    }

    NJson::TJsonValue SerializeDataToJson(const TString& iChunk, const TIndexInfo& indexInfo) const;

    TConclusionStatus CheckModificationCompatibility(const std::shared_ptr<IIndexMeta>& newMeta) const {
        if (!newMeta) {
            return TConclusionStatus::Fail("new meta cannot be absent");
        }
        if (newMeta->GetClassName() != GetClassName()) {
            return TConclusionStatus::Fail(
                "new meta have to be same index class (" + GetClassName() + "), but new class name: " + newMeta->GetClassName());
        }
        return DoCheckModificationCompatibility(*newMeta);
    }

    virtual ~IIndexMeta() = default;

    TConclusion<TIndexBuildOutcome> BuildIndexOptional(const THashMap<ui32, std::vector<std::shared_ptr<IPortionDataChunk>>>& data,
        const ui32 recordsCount, const TIndexInfo& indexInfo, const TIndexBuildContext& context) const;

    bool DeserializeFromProto(const NKikimrSchemeOp::TOlapIndexDescription& proto);
    void SerializeToProto(NKikimrSchemeOp::TOlapIndexDescription& proto) const;

    virtual TString GetClassName() const = 0;
};

class TIndexMetaContainer: public NBackgroundTasks::TInterfaceProtoContainer<IIndexMeta> {
private:
    using TBase = NBackgroundTasks::TInterfaceProtoContainer<IIndexMeta>;

public:
    TIndexMetaContainer() = default;

    TIndexMetaContainer(const std::shared_ptr<IIndexMeta>& object)
        : TBase(object)
    {
        AFL_VERIFY(Object);
    }
};

}   // namespace NKikimr::NOlap::NIndexes
