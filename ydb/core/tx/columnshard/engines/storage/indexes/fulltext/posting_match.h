#pragma once

#include <ydb/core/base/fulltext_query.h>
#include <ydb/library/conclusion/result.h>

#include <vector>

namespace NKikimr::NArrow {
class TColumnFilter;
}

namespace NKikimr::NOlap::NIndexes::NFulltext {

// One stored portion chunk. MetadataRows is the portion chunk's record count.
// Posting ids inside Bytes are local to that chunk.
struct TFulltextStoredChunk {
    TStringBuf Bytes;
    ui32 MetadataRows = 0;
};

enum class EFulltextPostingStatus {
    TokenMask,
    NeedTextFallback,
    Error
};

// TokenMask: PortionMask is the token condition in portion-row coordinates.
// A query token missing from a validated dictionary is an empty posting list.
// NeedTextFallback: no chunks, an unsupported format, or analyzer settings that
// do not match the request. Error: corrupt bytes or partial/gapped coverage.
struct TFulltextPostingResult {
    EFulltextPostingStatus Status = EFulltextPostingStatus::Error;
    TString Reason;
    std::vector<ui8> PortionMask;
};

TFulltextPostingResult BuildFulltextPostingMask(const std::vector<TFulltextStoredChunk>& chunks, ui32 portionRows, ui32 columnId, ui32 indexId,
    const NKikimr::NFulltext::TCompiledFulltextQuery& query);

// Portion-row bits -> the row space of the scan program.
// shrinkingFilter is set only when the scan has already compacted arrays.
// It is the same ApplyFilterFrom transform the duplicate filter uses.
TConclusion<std::vector<ui8>> MapFulltextMaskToProgramRows(
    const std::vector<ui8>& portionMask, const NArrow::TColumnFilter* shrinkingFilter, ui32 programRows);

}
