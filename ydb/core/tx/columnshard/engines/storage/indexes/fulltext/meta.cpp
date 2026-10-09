#include "meta.h"

namespace NKikimr::NOlap::NIndexes::NFulltext {

bool TFulltextIndexMeta::DoDeserializeFromProto(const NKikimrSchemeOp::TOlapIndexDescription& proto) {
    if (!proto.HasColumnFulltextIndex() || !proto.GetColumnFulltextIndex().HasColumnId()) {
        return false;
    }
    ColumnId = proto.GetColumnFulltextIndex().GetColumnId();
    Analyzers = NKikimr::NFulltext::NormalizeAnalyzers(proto.GetColumnFulltextIndex().GetAnalyzers());
    return ColumnId != 0 && Analyzers.has_tokenizer();
}

void TFulltextIndexMeta::DoSerializeToProto(NKikimrSchemeOp::TOlapIndexDescription& proto) const {
    auto* section = proto.MutableColumnFulltextIndex();
    section->SetColumnId(ColumnId);
    *section->MutableAnalyzers() = Analyzers;
}

TConclusionStatus TFulltextIndexMeta::DoCheckModificationCompatibility(const IIndexMeta& newMeta) const {
    const auto* other = dynamic_cast<const TFulltextIndexMeta*>(&newMeta);
    if (!other) {
        return TConclusionStatus::Fail(TString(NKikimr::NFulltext::LocalFulltextIndexAlterRejected));
    }
    if (other->ColumnId != ColumnId
        || NKikimr::NFulltext::FulltextAnalyzerIdentity(other->Analyzers) != NKikimr::NFulltext::FulltextAnalyzerIdentity(Analyzers))
    {
        return TConclusionStatus::Fail(TString(NKikimr::NFulltext::LocalFulltextIndexAlterRejected));
    }
    return TConclusionStatus::Success();
}

} // namespace NKikimr::NOlap::NIndexes::NFulltext
