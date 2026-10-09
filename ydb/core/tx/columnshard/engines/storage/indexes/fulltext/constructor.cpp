#include "constructor.h"
#include "meta.h"

#include <ydb/core/tx/schemeshard/olap/schema/schema.h>
#include <ydb/public/lib/scheme_types/scheme_type_id.h>

namespace NKikimr::NOlap::NIndexes::NFulltext {

namespace {

bool IsTextColumn(const NScheme::TTypeInfo type) {
    const auto typeId = type.GetTypeId();
    return typeId == NKikimr::NScheme::NTypeIds::String || typeId == NKikimr::NScheme::NTypeIds::Utf8;
}

}

std::shared_ptr<IIndexMeta> TIndexConstructor::DoCreateIndexMeta(const ui32 indexId, const TString& indexName,
    const NSchemeShard::TOlapSchema& currentSchema, NSchemeShard::IErrorCollector& errors) const {
    const auto* columnInfo = currentSchema.GetColumns().GetByName(ColumnName);
    if (!columnInfo) {
        errors.AddError(TStringBuilder() << "Local fulltext index '" << indexName << "' refers to unknown column '" << ColumnName << "'");
        return nullptr;
    }
    if (!IsTextColumn(columnInfo->GetType())) {
        errors.AddError(TString(NKikimr::NFulltext::LocalFulltextIndexOneColumn));
        return nullptr;
    }
    TString validationError;
    Ydb::Table::FulltextIndexSettings settings;
    auto* column = settings.add_columns();
    column->set_column(ColumnName);
    *column->mutable_analyzers() = Analyzers;
    if (!NKikimr::NFulltext::ValidateSettings(settings, validationError)) {
        errors.AddError(validationError);
        return nullptr;
    }
    const auto normalized = NKikimr::NFulltext::NormalizeAnalyzers(Analyzers);
    const TString storageId = GetStorageId().value_or(IStoragesManager::DefaultStorageId);
    const bool inherit = GetInheritPortionStorage().value_or(true);
    return std::make_shared<TFulltextIndexMeta>(indexId, indexName, storageId, inherit, columnInfo->GetId(), normalized);
}

TConclusionStatus TIndexConstructor::DoDeserializeFromJson(const NJson::TJsonValue&) {
    return TConclusionStatus::Fail("JSON description is not supported for local fulltext indexes");
}

TConclusionStatus TIndexConstructor::DoDeserializeFromProto(const NKikimrSchemeOp::TOlapIndexRequested& proto) {
    if (!proto.HasColumnFulltextIndex()) {
        return TConclusionStatus::Fail("ColumnFulltextIndex section is missing");
    }
    const auto& section = proto.GetColumnFulltextIndex();
    if (section.GetColumnName().empty()) {
        return TConclusionStatus::Fail("Local fulltext index requires a column name");
    }
    ColumnName = section.GetColumnName();
    Analyzers = NKikimr::NFulltext::NormalizeAnalyzers(section.GetAnalyzers());
    return TConclusionStatus::Success();
}

void TIndexConstructor::DoSerializeToProto(NKikimrSchemeOp::TOlapIndexRequested& proto) const {
    auto* section = proto.MutableColumnFulltextIndex();
    section->SetColumnName(ColumnName);
    *section->MutableAnalyzers() = Analyzers;
}

} // namespace NKikimr::NOlap::NIndexes::NFulltext
