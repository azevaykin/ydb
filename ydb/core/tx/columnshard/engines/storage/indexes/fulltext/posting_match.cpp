#include "format.h"
#include "posting_match.h"

#include <ydb/core/formats/arrow/filter/filter.h>

#include <algorithm>

namespace NKikimr::NOlap::NIndexes::NFulltext {

namespace {

enum class EPrefix {
    Supported,
    Unsupported,
    Corrupt
};

EPrefix InspectPrefix(TStringBuf bytes, TString& reason) {
    if (bytes.size() < FulltextChunkPrefixBytes) {
        reason = "truncated fulltext chunk prefix";
        return EPrefix::Corrupt;
    }
    if (bytes.Head(4) != TStringBuf(FulltextChunkMagic, 4)) {
        reason = "fulltext chunk magic is not YFTI";
        return EPrefix::Corrupt;
    }
    const auto* versionBytes = reinterpret_cast<const unsigned char*>(bytes.data() + 4);
    const ui32 version = ui32(versionBytes[0]) | (ui32(versionBytes[1]) << 8) | (ui32(versionBytes[2]) << 16) | (ui32(versionBytes[3]) << 24);
    if (version != FulltextChunkFormatVersion && version != FulltextChunkLegacyFormatVersion) {
        reason = "unsupported fulltext chunk format version";
        return EPrefix::Unsupported;
    }
    return EPrefix::Supported;
}

TFulltextPostingResult Fallback(TString reason) {
    TFulltextPostingResult result;
    result.Status = EFulltextPostingStatus::NeedTextFallback;
    result.Reason = std::move(reason);
    return result;
}

TFulltextPostingResult Fail(TString reason) {
    TFulltextPostingResult result;
    result.Status = EFulltextPostingStatus::Error;
    result.Reason = std::move(reason);
    return result;
}

}   // namespace

TFulltextPostingResult BuildFulltextPostingMask(const std::vector<TFulltextStoredChunk>& chunks, const ui32 portionRows, const ui32 columnId,
    const ui32 indexId, const NKikimr::NFulltext::TCompiledFulltextQuery& query) {
    if (query.Terms.empty()) {
        return Fail("fulltext query has no terms");
    }
    if (chunks.empty()) {
        return Fallback("fulltext index has no chunks");
    }

    // Validate metadata before deciding to fall back: a future format or stale
    // analyzer must not hide missing chunks or an invalid row domain.
    ui64 covered = 0;
    for (const auto& chunk : chunks) {
        if (!chunk.MetadataRows) {
            return Fail("fulltext chunk has no portion rows");
        }
        covered += chunk.MetadataRows;
        if (covered > portionRows) {
            return Fail("fulltext index chunk coverage is partial or gapped");
        }
    }
    if (covered != portionRows) {
        return Fail("fulltext index chunk coverage is partial or gapped");
    }

    const TString& expectedSettings = query.AnalyzerIdentity;
    TFulltextPostingResult result;
    result.Status = EFulltextPostingStatus::TokenMask;
    result.PortionMask.resize(portionRows);
    TString fallbackReason;
    ui32 preceding = 0;
    for (const auto& chunk : chunks) {
        TString reason;
        const EPrefix prefix = InspectPrefix(chunk.Bytes, reason);
        if (prefix == EPrefix::Corrupt) {
            return Fail(std::move(reason));
        }
        if (prefix == EPrefix::Unsupported) {
            fallbackReason = std::move(reason);
            preceding += chunk.MetadataRows;
            continue;
        }
        auto decoded = DecodeFulltextChunkView(chunk.Bytes);
        if (decoded.IsFail()) {
            return Fail(decoded.GetErrorMessage());
        }
        const auto& plain = decoded.GetResult();
        if (plain.ColumnId != columnId || plain.IndexId != indexId) {
            return Fail("fulltext chunk column or index identity does not match the scan");
        }
        if (plain.ChunkRowCount != chunk.MetadataRows) {
            return Fail("fulltext chunk row count does not match the portion chunk");
        }
        if (plain.AnalyzerSemanticRevision != query.AnalyzerSemanticRevision) {
            fallbackReason = "fulltext index analyzer semantic revision is incompatible";
        } else if (plain.CanonicalSettings != expectedSettings) {
            fallbackReason = "fulltext index analyzer settings are incompatible";
        }
        if (!fallbackReason.empty()) {
            preceding += chunk.MetadataRows;
            continue;
        }

        // Two counts per row work for any query size without exponential lookup
        // tables, bit-width boundaries, or per-row hash sets and string copies.
        std::vector<ui32> required(chunk.MetadataRows);
        std::vector<ui32> optional(chunk.MetadataRows);
        for (const auto& term : query.Terms) {
            const auto posting = std::lower_bound(
                plain.Postings.begin(), plain.Postings.end(), term.Token, [](const TFulltextPostingView& entry, const TString& token) {
                    return entry.Token < token;
                });
            if (posting == plain.Postings.end() || posting->Token != term.Token) {
                continue;
            }
            auto& counts = term.Required ? required : optional;
            posting->VisitRows([&](ui32 row) {
                ++counts[row];
            });
        }
        for (ui32 row = 0; row < chunk.MetadataRows; ++row) {
            result.PortionMask[preceding + row] = NKikimr::NFulltext::EvaluateFulltextCounts(query, required[row], optional[row]);
        }
        preceding += chunk.MetadataRows;
    }
    if (!fallbackReason.empty()) {
        return Fallback(std::move(fallbackReason));
    }
    return result;
}

TConclusion<std::vector<ui8>> MapFulltextMaskToProgramRows(
    const std::vector<ui8>& portionMask, const NArrow::TColumnFilter* shrinkingFilter, const ui32 programRows) {
    if (!shrinkingFilter || shrinkingFilter->IsTotalAllowFilter()) {
        if (portionMask.size() != programRows) {
            return TConclusionStatus::Fail("fulltext posting mask does not match the source row count");
        }
        return portionMask;
    }
    if (shrinkingFilter->IsTotalDenyFilter()) {
        if (programRows != 0) {
            return TConclusionStatus::Fail("fulltext posting mask does not match the filtered row count");
        }
        return std::vector<ui8>();
    }
    const std::optional<ui32> filterRows = shrinkingFilter->GetRecordsCount();
    if (!filterRows || *filterRows != portionMask.size()) {
        return TConclusionStatus::Fail("fulltext posting mask does not match the applied filter");
    }
    NArrow::TColumnFilter mask = NArrow::TColumnFilter::BuildAllowFilter();
    for (const ui8 value : portionMask) {
        mask.Add(value != 0, 1);
    }
    const NArrow::TColumnFilter mapped = mask.ApplyFilterFrom(*shrinkingFilter);
    const std::optional<ui32> mappedRows = mapped.GetRecordsCount();
    if (!mappedRows) {
        if (programRows == 0) {
            return std::vector<ui8>();
        }
        return TConclusionStatus::Fail("fulltext posting mask does not match the filtered row count");
    }
    if (*mappedRows != programRows) {
        return TConclusionStatus::Fail("fulltext posting mask does not match the filtered row count");
    }
    const std::vector<bool>& plain = mapped.BuildSimpleFilter();
    std::vector<ui8> result(plain.size());
    for (ui32 i = 0; i < plain.size(); ++i) {
        result[i] = plain[i] ? 1 : 0;
    }
    return result;
}

}   // namespace NKikimr::NOlap::NIndexes::NFulltext
