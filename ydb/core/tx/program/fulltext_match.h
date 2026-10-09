#pragma once

#include "fulltext_source.h"

#include <ydb/core/formats/arrow/program/graph_optimization.h>
#include <ydb/library/formats/arrow/protos/ssa.pb.h>

#include <memory>
#include <optional>
#include <vector>

namespace arrow {
class RecordBatch;
class Scalar;
}

namespace NKikimr::NArrow::NAccessor {
class IChunkedArray;
}

namespace NKikimr::NArrow::NSSA {

// One entry per accessor row. Nullopt is SQL null. An empty string is present text.
TConclusion<std::vector<std::optional<TString>>> ReadFulltextTexts(const std::shared_ptr<NArrow::NAccessor::IChunkedArray>& text);

TConclusion<std::optional<TString>> ReadFulltextScalar(const arrow::Scalar& scalar);

// Null text is false. Empty text is analyzed.
TConclusion<std::vector<ui8>> EvaluateFulltextTexts(
    const NFulltext::TCompiledFulltextQuery& query, const std::vector<std::optional<TString>>& texts);

// tokenMask and texts are the same row space. A zero token bit stays zero.
// A one bit still has to pass MatchFulltextWildcardResidual on the original text.
TConclusion<std::vector<ui8>> ApplyFulltextWildcardResidual(const NFulltext::TCompiledFulltextQuery& query,
    const std::vector<ui8>& tokenMask, const std::vector<std::optional<TString>>& texts);

// The text column is not an input. Index bytes are fetched first; text is fetched
// only when the index is missing, incompatible, or a wildcard residual needs it.
TConclusionStatus AppendFulltextMatch(NGraph::NOptimization::TGraph::TBuilder& builder,
    const NKikimrSSA::TProgram::TAssignment::TFulltextMatch& match, ui32 outputColumnId,
    const std::shared_ptr<arrow::RecordBatch>& parameterValues);

}
