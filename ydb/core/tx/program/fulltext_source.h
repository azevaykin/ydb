#pragma once

#include <ydb/core/base/fulltext_query.h>
#include <ydb/core/formats/arrow/program/execution.h>
#include <ydb/library/conclusion/result.h>

#include <memory>

namespace NKikimr::NArrow::NSSA {

// One compiled FulltextMatch for a scan program. Text is not an input of the node:
// the reader fetches it only for fallback or a wildcard residual.
struct TFulltextMatchRequest {
    ui32 IndexId = 0;
    ui32 ColumnId = 0;
    NFulltext::TCompiledFulltextQuery Query;
};

// Implemented by the column scan source. SIMPLE and TRIVIAL share one service.
class IFulltextMatchSource {
public:
    virtual ~IFulltextMatchSource() = default;

    virtual TConclusion<TExecutionResult> ReserveFulltextMatch(
        const TProcessorContext& context, const TFulltextMatchRequest& request) = 0;
    virtual TConclusion<TExecutionResult> FetchFulltextIndex(
        const TProcessorContext& context, const TFulltextMatchRequest& request) = 0;
    virtual TConclusion<TExecutionResult> DecideFulltextMatch(
        const TProcessorContext& context, const TFulltextMatchRequest& request) = 0;
    virtual TConclusion<TExecutionResult> ReserveFulltextText(
        const TProcessorContext& context, const TFulltextMatchRequest& request) = 0;
    virtual TConclusion<TExecutionResult> FetchFulltextText(
        const TProcessorContext& context, const TFulltextMatchRequest& request) = 0;
    virtual TConclusion<TExecutionResult> EmitFulltextMatch(
        const TProcessorContext& context, const TFulltextMatchRequest& request, ui32 outputColumnId) = 0;
};

}
