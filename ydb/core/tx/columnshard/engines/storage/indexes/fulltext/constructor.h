#pragma once

#include <ydb/core/base/fulltext.h>
#include <ydb/core/tx/columnshard/engines/scheme/indexes/abstract/constructor.h>

namespace NKikimr::NOlap::NIndexes::NFulltext {

class TIndexConstructor: public IIndexMetaConstructor {
public:
    static TString GetClassNameStatic() {
        return TString(NKikimr::NFulltext::LocalFulltextClassName);
    }

private:
    TString ColumnName;
    Ydb::Table::FulltextIndexSettings::Analyzers Analyzers;
    static inline auto Registrator = TFactory::TRegistrator<TIndexConstructor>(GetClassNameStatic());

protected:
    std::shared_ptr<IIndexMeta> DoCreateIndexMeta(const ui32 indexId, const TString& indexName,
        const NSchemeShard::TOlapSchema& currentSchema, NSchemeShard::IErrorCollector& errors) const override;

    TConclusionStatus DoDeserializeFromJson(const NJson::TJsonValue& jsonInfo) override;
    TConclusionStatus DoDeserializeFromProto(const NKikimrSchemeOp::TOlapIndexRequested& proto) override;
    void DoSerializeToProto(NKikimrSchemeOp::TOlapIndexRequested& proto) const override;

public:
    TIndexConstructor() = default;

    TString GetClassName() const override {
        return GetClassNameStatic();
    }
};

} // namespace NKikimr::NOlap::NIndexes::NFulltext
