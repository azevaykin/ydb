#include "posting_match.h"

#include "format.h"

#include <ydb/core/formats/arrow/filter/filter.h>

#include <util/generic/hash_set.h>

#include <algorithm>

namespace NKikimr::NOlap::NIndexes::NFulltext {

namespace {

enum class EPrefix {
    Version1,
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
    if (version != FulltextChunkFormatVersion) {
        reason = "unsupported fulltext chunk format version";
        return EPrefix::Unsupported;
    }
    return EPrefix::Version1;
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

const TFulltextPostingList* FindToken(const std::vector<TFulltextPostingList>& postings, const TString& token) {
    const auto it = std::lower_bound(postings.begin(), postings.end(), token,
        [](const TFulltextPostingList& posting, const TString& key) {
            return posting.Token < key;
        });
    if (it == postings.end() || it->Token != token) {
        return nullptr;
    }
    return &*it;
}

std::vector<ui8> MembershipFromBits(const NKikimr::NFulltext::TCompiledFulltextQuery& query, const std::vector<ui64>& bits) {
    const ui32 termCount = query.Terms.size();
    std::vector<ui8> pattern;
    if (termCount <= 16) {
        pattern.resize(size_t(1) << termCount);
        for (ui32 mask = 0; mask < pattern.size(); ++mask) {
            THashSet<TString> tokens;
            for (ui32 term = 0; term < termCount; ++term) {
                if (mask & (ui32(1) << term)) {
                    tokens.insert(query.Terms[term].Token);
                }
            }
            pattern[mask] = NKikimr::NFulltext::EvaluateFulltextMembership(query, tokens) ? 1 : 0;
        }
    }
    std::vector<ui8> result(bits.size());
    for (ui32 row = 0; row < bits.size(); ++row) {
        if (!pattern.empty()) {
            result[row] = pattern[bits[row]];
            continue;
        }
        THashSet<TString> tokens;
        for (ui32 term = 0; term < termCount; ++term) {
            if (bits[row] & (ui64(1) << term)) {
                tokens.insert(query.Terms[term].Token);
            }
        }
        result[row] = NKikimr::NFulltext::EvaluateFulltextMembership(query, tokens) ? 1 : 0;
    }
    return result;
}

std::vector<ui8> MembershipFromSets(const NKikimr::NFulltext::TCompiledFulltextQuery& query, const std::vector<THashSet<TString>>& rowTokens) {
    std::vector<ui8> result(rowTokens.size());
    for (ui32 row = 0; row < rowTokens.size(); ++row) {
        result[row] = NKikimr::NFulltext::EvaluateFulltextMembership(query, rowTokens[row]) ? 1 : 0;
    }
    return result;
}

}

TFulltextPostingResult BuildFulltextPostingMask(const std::vector<TFulltextStoredChunk>& chunks, const ui32 portionRows, const ui32 columnId,
    const ui32 indexId, const NKikimr::NFulltext::TCompiledFulltextQuery& query) {
    if (query.Terms.empty()) {
        return Fail("fulltext query has no terms");
    }
    if (chunks.empty()) {
        return Fallback("fulltext index has no chunks");
    }

    const TString expectedSettings = CanonicalAnalyzerSettings(query.Analyzers);
    struct TKeptChunk {
        TFulltextChunkPlain Plain;
        ui32 Rows = 0;
    };
    std::vector<TKeptChunk> kept;
    kept.reserve(chunks.size());
    bool unsupported = false;
    bool incompatible = false;
    TString fallbackReason;
    ui64 covered = 0;

    for (const TFulltextStoredChunk& chunk : chunks) {
        TString reason;
        const EPrefix prefix = InspectPrefix(chunk.Bytes, reason);
        if (prefix == EPrefix::Corrupt) {
            return Fail(std::move(reason));
        }
        if (prefix == EPrefix::Unsupported) {
            unsupported = true;
            fallbackReason = std::move(reason);
            continue;
        }
        auto decoded = DecodeFulltextChunk(chunk.Bytes);
        if (decoded.IsFail()) {
            return Fail(decoded.GetErrorMessage());
        }
        TFulltextChunkPlain plain = decoded.DetachResult();
        if (plain.ColumnId != columnId || plain.IndexId != indexId) {
            return Fail("fulltext chunk column or index identity does not match the scan");
        }
        if (plain.AnalyzerRevision != query.AnalyzerRevision) {
            incompatible = true;
            fallbackReason = "fulltext index analyzer settings are incompatible";
            continue;
        }
        if (plain.CanonicalSettings != expectedSettings) {
            return Fail("fulltext chunk analyzer settings do not match its revision");
        }
        if (chunk.MetadataRows == 0 || plain.ChunkRowCount != chunk.MetadataRows) {
            return Fail("fulltext chunk row count does not match the portion chunk");
        }
        covered += chunk.MetadataRows;
        kept.push_back(TKeptChunk{std::move(plain), chunk.MetadataRows});
    }

    if (unsupported || incompatible) {
        return Fallback(fallbackReason ? fallbackReason : "fulltext index cannot be used");
    }
    if (covered != portionRows) {
        return Fail("fulltext index chunk coverage is partial or gapped");
    }

    ui64 preceding = 0;
    if (query.Terms.size() <= 64) {
        std::vector<ui64> bits(portionRows);
        for (const TKeptChunk& chunk : kept) {
            for (ui32 term = 0; term < query.Terms.size(); ++term) {
                const TFulltextPostingList* posting = FindToken(chunk.Plain.Postings, query.Terms[term].Token);
                if (!posting) {
                    continue;
                }
                for (const ui32 localRow : posting->Rows) {
                    const ui64 row = preceding + localRow;
                    if (localRow >= chunk.Rows || row >= portionRows) {
                        return Fail("fulltext posting row is outside the portion");
                    }
                    bits[static_cast<ui32>(row)] |= ui64(1) << term;
                }
            }
            preceding += chunk.Rows;
        }
        TFulltextPostingResult result;
        result.Status = EFulltextPostingStatus::TokenMask;
        result.PortionMask = MembershipFromBits(query, bits);
        return result;
    }

    std::vector<THashSet<TString>> rowTokens(portionRows);
    for (const TKeptChunk& chunk : kept) {
        for (const auto& term : query.Terms) {
            const TFulltextPostingList* posting = FindToken(chunk.Plain.Postings, term.Token);
            if (!posting) {
                continue;
            }
            for (const ui32 localRow : posting->Rows) {
                const ui64 row = preceding + localRow;
                if (localRow >= chunk.Rows || row >= portionRows) {
                    return Fail("fulltext posting row is outside the portion");
                }
                rowTokens[static_cast<ui32>(row)].insert(term.Token);
            }
        }
        preceding += chunk.Rows;
    }
    TFulltextPostingResult result;
    result.Status = EFulltextPostingStatus::TokenMask;
    result.PortionMask = MembershipFromSets(query, rowTokens);
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

}
