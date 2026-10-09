#pragma once

#include <ydb/core/base/fulltext.h>
#include <ydb/core/formats/arrow/save_load/loader.h>
#include <ydb/core/tx/columnshard/engines/scheme/indexes/abstract/meta.h>

#include <optional>
#include <vector>

namespace NKikimr::NOlap::NIndexes::NFulltext {

class TFulltextIndexMeta: public IIndexMeta {
public:
    static TString GetClassNameStatic() {
        return TString(NKikimr::NFulltext::LocalFulltextClassName);
    }

    TFulltextIndexMeta() = default;

    TFulltextIndexMeta(const ui32 indexId, const TString& indexName, const TString& storageId, const bool inheritPortionStorage,
        const ui32 columnId, Ydb::Table::FulltextIndexSettings::Analyzers analyzers)
        : IIndexMeta(indexId, indexName, storageId, inheritPortionStorage)
        , ColumnId(columnId)
        , Analyzers(std::move(analyzers))
    {
    }

    ui32 GetColumnId() const {
        return ColumnId;
    }

    const Ydb::Table::FulltextIndexSettings::Analyzers& GetAnalyzers() const {
        return Analyzers;
    }

    std::optional<ui32> GetSingleColumnId() const override {
        return ColumnId == 0 ? std::nullopt : std::optional<ui32>(ColumnId);
    }

    TString GetClassName() const override {
        return GetClassNameStatic();
    }

protected:
    TConclusion<TIndexBuildOutcome> DoBuildIndexOptional(const THashMap<ui32, std::vector<std::shared_ptr<IPortionDataChunk>>>& data,
        const ui32 recordsCount, const TIndexInfo& indexInfo, const TIndexBuildContext& context) const override;

    bool DoDeserializeFromProto(const NKikimrSchemeOp::TOlapIndexDescription& proto) override;
    void DoSerializeToProto(NKikimrSchemeOp::TOlapIndexDescription& proto) const override;
    TConclusionStatus DoCheckModificationCompatibility(const IIndexMeta& newMeta) const override;

private:
    static inline auto Registrator = TFactory::TRegistrator<TFulltextIndexMeta>(GetClassNameStatic());
    ui32 ColumnId = 0;
    Ydb::Table::FulltextIndexSettings::Analyzers Analyzers;
};

// nullopt is SQL null. An empty string is analyzed, including the empty KEYWORD token.
TConclusion<TIndexBuildOutcome> BuildFulltextFromDocuments(const std::vector<std::optional<TString>>& documents, const TFulltextIndexMeta& meta,
    const TIndexBuildContext& context);

// Absent column chunks use loader.BuildDefaultAccessor. That is the accessor NormalizeBatch installs
// for a column missing from the source schema; it is not an implicit SQL null.
TConclusion<TIndexBuildOutcome> BuildFulltextIndex(const THashMap<ui32, std::vector<std::shared_ptr<IPortionDataChunk>>>& data, const ui32 recordsCount,
    const NArrow::NAccessor::TColumnLoader& loader, const TFulltextIndexMeta& meta, const TIndexBuildContext& context);

} // namespace NKikimr::NOlap::NIndexes::NFulltext
