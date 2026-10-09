#include "constructor.h"
#include "fetch_steps.h"
#include "source.h"

#include <ydb/core/tx/columnshard/engines/portions/index_chunk.h>

#include <ydb/core/formats/arrow/accessor/plain/accessor.h>
#include <ydb/core/tx/columnshard/engines/scheme/index_info.h>
#include <ydb/core/tx/columnshard/engines/storage/indexes/fulltext/counters.h>
#include <ydb/core/tx/columnshard/engines/storage/indexes/fulltext/meta.h>
#include <ydb/core/tx/columnshard/engines/storage/indexes/fulltext/posting_match.h>
#include <ydb/core/tx/program/fulltext_match.h>

#include <ydb/library/actors/core/log.h>

#include <contrib/libs/apache/arrow/cpp/src/arrow/array/builder_primitive.h>

#include <ydb/core/tx/columnshard/engines/scheme/indexes/abstract/common.h>

#include <util/generic/hash_set.h>

#include <algorithm>
#include <set>

#define YDB_LOG_THIS_FILE_COMPONENT NKikimrServices::TX_COLUMNSHARD_SCAN

namespace NKikimr::NOlap::NReader::NCommon {

namespace {

using NIndexes::NFulltext::EFulltextPostingStatus;
using NIndexes::NFulltext::TFulltextStoredChunk;

TConclusion<NArrow::NSSA::TExecutionResult> Fail(const TString& reason) {
    return TConclusionStatus::Fail(reason);
}

struct TRowSpace {
    ui32 PortionRows = 0;
    ui32 ProgramRows = 0;
    const NArrow::TColumnFilter* Shrinking = nullptr;
};

// Shrinking is set only when arrays were already compacted. That is the same
// case in which fetching.cpp remaps a duplicate filter with ApplyFilterFrom.
TRowSpace DescribeRows(const NArrow::NAccessor::TAccessorsCollection& resources, const ui32 portionRows) {
    TRowSpace space;
    space.PortionRows = portionRows;
    space.ProgramRows = portionRows;
    if (!resources.GetFilterUsage()) {
        return space;
    }
    const std::shared_ptr<NArrow::TColumnFilter>& applied = resources.GetAppliedFilter();
    if (!applied || applied->IsTotalAllowFilter()) {
        return space;
    }
    space.Shrinking = applied.get();
    if (applied->IsTotalDenyFilter()) {
        space.ProgramRows = 0;
    } else {
        space.ProgramRows = applied->GetFilteredCountVerified();
    }
    return space;
}

std::shared_ptr<NIndexes::NFulltext::TFulltextIndexMeta> FindUsableIndex(
    const IDataSource& source, const NArrow::NSSA::TFulltextMatchRequest& request) {
    const std::shared_ptr<ISnapshotSchema>& schema = source.GetSourceSchemaOptional();
    if (!schema) {
        return nullptr;
    }
    const auto meta = schema->GetIndexInfo().GetIndexOptional(request.IndexId);
    if (!meta.HasObject()) {
        return nullptr;
    }
    auto fulltext = meta.GetObjectPtrOptionalAs<NIndexes::NFulltext::TFulltextIndexMeta>();
    if (!fulltext || fulltext->GetColumnId() != request.ColumnId) {
        return nullptr;
    }
    return fulltext;
}

bool AnyMatch(const std::vector<ui8>& mask) {
    return std::any_of(mask.begin(), mask.end(), [](const ui8 value) {
        return value != 0;
    });
}

TConclusion<NArrow::NSSA::TExecutionResult> AddBoolean(
    const NArrow::NSSA::TProcessorContext& context, const ui32 outputColumnId, const std::vector<ui8>& mask) {
    arrow::UInt8Builder builder;
    const auto reserved = builder.Reserve(mask.size());
    if (!reserved.ok()) {
        return Fail(TString(reserved.message()));
    }
    for (const ui8 value : mask) {
        const auto status = builder.Append(value);
        if (!status.ok()) {
            return Fail(TString(status.message()));
        }
    }
    std::shared_ptr<arrow::Array> array;
    const auto finished = builder.Finish(&array);
    if (!finished.ok()) {
        return Fail(TString(finished.message()));
    }
    // withFilter is false: the mask is already in program-row coordinates.
    context.MutableResources().AddVerified(outputColumnId, std::make_shared<NArrow::NAccessor::TTrivialArray>(array), false);
    return NArrow::NSSA::TExecutionResult::Done();
}

TConclusion<std::vector<std::optional<TString>>> ReadScalarTexts(
    const NArrow::NSSA::TProcessorContext& context, const ui32 columnId, const ui32 expectedRows) {
    const std::shared_ptr<arrow::Scalar>& scalar = context.GetResources().GetConstantScalarOptional(columnId);
    if (!scalar) {
        return TConclusionStatus::Fail("Fulltext text column is not available");
    }
    auto text = NArrow::NSSA::ReadFulltextScalar(*scalar);
    if (text.IsFail()) {
        return TConclusionStatus::Fail(text.GetErrorMessage());
    }
    return std::vector<std::optional<TString>>(expectedRows, text.GetResult());
}

bool TextIsPresent(const NArrow::NSSA::TProcessorContext& context, const ui32 columnId) {
    return context.GetResources().GetAccessorOptional(columnId) || context.GetResources().GetConstantScalarOptional(columnId);
}

TConclusion<NArrow::NSSA::TExecutionResult> StartTextFetch(
    IDataSource& source, const NArrow::NSSA::TProcessorContext& context, const NArrow::NSSA::TFulltextMatchRequest& request) {
    const TString columnName = source.GetContext()->GetCommonContext()->GetResolver()->GetColumnName(request.ColumnId, true);
    NArrow::NSSA::IDataSource::TDataAddress address(request.ColumnId, columnName, std::nullopt);
    auto logic = source.StartFetchData(context, address);
    if (logic.IsFail()) {
        return TConclusionStatus::Fail(logic.GetErrorMessage());
    }
    if (!logic.GetResult()) {
        return NArrow::NSSA::TExecutionResult::Done();
    }
    std::vector<std::shared_ptr<NArrow::NSSA::IFetchLogic>> fetchers;
    fetchers.push_back(logic.DetachResult());
    return source.StartFetch(context, fetchers);
}

void RememberFallback(TFetchedData::TFulltextMatchState& state, TString reason, const ui32 rows = 0, const ui64 bytes = 0) {
    state.IndexCollected = true;
    state.NeedText = true;
    state.HasTokenMask = false;
    state.PortionMask.clear();
    state.FallbackReason = std::move(reason);
    NIndexes::NFulltext::TFulltextBuildCounters::OnFallback(rows, bytes);
}

}

TConclusion<NArrow::NSSA::TExecutionResult> IDataSource::ReserveFulltextMatch(
    const NArrow::NSSA::TProcessorContext& context, const NArrow::NSSA::TFulltextMatchRequest& request) {
    if (!HasPortionAccessor() || context.GetResources().GetFilter().IsTotalDenyFilter()) {
        return NArrow::NSSA::TExecutionResult::Done();
    }
    const std::shared_ptr<NIndexes::NFulltext::TFulltextIndexMeta> fulltext = FindUsableIndex(*this, request);
    if (!fulltext) {
        return NArrow::NSSA::TExecutionResult::Done();
    }
    ui64 bytes = 0;
    for (const TIndexChunk* chunk : GetPortionAccessor().GetIndexChunksPointers(request.IndexId)) {
        bytes += chunk->GetDataSize();
    }
    if (bytes == 0) {
        return NArrow::NSSA::TExecutionResult::Done();
    }
    return StartProgramStepReserveMemory(*this, bytes, NArrow::NSSA::IMemoryCalculationPolicy::EStage::Filter);
}

TConclusion<NArrow::NSSA::TExecutionResult> IDataSource::FetchFulltextIndex(
    const NArrow::NSSA::TProcessorContext& context, const NArrow::NSSA::TFulltextMatchRequest& request) {
    if (!HasPortionAccessor()) {
        return Fail("fulltext match source has no portion");
    }
    auto& state = MutableStageData().MutableFulltextMatchState();
    if (context.GetResources().GetFilter().IsTotalDenyFilter()) {
        state.IndexCollected = true;
        state.NeedText = false;
        state.HasTokenMask = true;
        state.PortionMask.assign(GetRecordsCount(), 0);
        return NArrow::NSSA::TExecutionResult::Done();
    }
    const std::shared_ptr<NIndexes::NFulltext::TFulltextIndexMeta> fulltext = FindUsableIndex(*this, request);
    const std::vector<const TIndexChunk*> chunks =
        fulltext ? GetPortionAccessor().GetIndexChunksPointers(request.IndexId) : std::vector<const TIndexChunk*>();
    if (!fulltext || chunks.empty()) {
        RememberFallback(state,
            fulltext ? "fulltext index has no chunks" : "fulltext index is not available for this portion",
            GetRecordsCount());
        YDB_LOG_DEBUG("",
            {"event", "fulltext_fallback"},
            {"reason", state.FallbackReason},
            {"index", request.IndexId},
            {"column", request.ColumnId});
        return NArrow::NSSA::TExecutionResult::Done();
    }

    THashSet<NIndexes::NRequest::TOriginalDataAddress> addresses;
    addresses.insert(NIndexes::NRequest::TOriginalDataAddress(request.ColumnId));
    std::shared_ptr<NIndexes::IIndexMeta> meta = fulltext;
    std::vector<std::shared_ptr<NArrow::NSSA::IFetchLogic>> fetchers;
    fetchers.push_back(meta->BuildFetchTask(addresses, meta, GetContext()->GetCommonContext()->GetStoragesManager()));
    return StartFetch(context, fetchers);
}

TConclusion<NArrow::NSSA::TExecutionResult> IDataSource::DecideFulltextMatch(
    const NArrow::NSSA::TProcessorContext& context, const NArrow::NSSA::TFulltextMatchRequest& request) {
    if (!HasPortionAccessor()) {
        return Fail("fulltext match source has no portion");
    }
    auto& state = MutableStageData().MutableFulltextMatchState();
    if (!state.IndexCollected) {
        std::shared_ptr<IKernelFetchLogic> fetcher = MutableStageData().ExtractFetcherOptional(request.IndexId);
        if (!fetcher) {
            return Fail("fulltext index bytes were not fetched");
        }
        TFetchingResultContext fetchContext(context.MutableResources(), *GetStageData().GetIndexes(), *this);
        const TConclusionStatus collected = fetcher->OnDataCollected(fetchContext);
        if (collected.IsFail()) {
            return TConclusionStatus::Fail(collected.GetErrorMessage());
        }
        const NIndexes::TIndexColumnChunked* stored = GetStageData().GetIndexes()->GetIndexDataOptional(request.IndexId);
        if (!stored || stored->GetChunks().empty()) {
            return Fail("fulltext index fetch produced no chunks");
        }
        std::vector<TFulltextStoredChunk> chunks;
        chunks.reserve(stored->GetChunks().size());
        for (const auto& chunk : stored->GetChunks()) {
            const TString* bytes = chunk.GetDataOptional(std::nullopt);
            if (!bytes) {
                return Fail("fulltext index chunk bytes are missing");
            }
            chunks.push_back(TFulltextStoredChunk{*bytes, chunk.GetRecordsCount()});
        }
        const auto assessed = NIndexes::NFulltext::BuildFulltextPostingMask(
            chunks, GetRecordsCount(), request.ColumnId, request.IndexId, request.Query);
        if (assessed.Status == EFulltextPostingStatus::Error) {
            NIndexes::NFulltext::TFulltextBuildCounters::OnDecodeError();
            return Fail(assessed.Reason);
        }
        if (assessed.Status == EFulltextPostingStatus::NeedTextFallback) {
            RememberFallback(state, assessed.Reason, GetRecordsCount());
            YDB_LOG_DEBUG("",
                {"event", "fulltext_fallback"},
                {"reason", state.FallbackReason},
                {"index", request.IndexId});
        } else {
            state.IndexCollected = true;
            state.HasTokenMask = true;
            state.PortionMask = assessed.PortionMask;
            state.NeedText = request.Query.Mode == NFulltext::EFulltextQueryMode::Wildcard && AnyMatch(state.PortionMask);
            state.FallbackReason.clear();
            ui64 bytes = 0;
            for (const auto& chunk : chunks) {
                bytes += chunk.Bytes.size();
            }
            const ui32 matched = std::count_if(state.PortionMask.begin(), state.PortionMask.end(), [](ui8 v) {
                return v != 0;
            });
            NIndexes::NFulltext::TFulltextBuildCounters::OnIndexedRead(
                chunks.size(), bytes, state.PortionMask.size(), matched);
        }
    }
    return NArrow::NSSA::TExecutionResult::Done();
}

TConclusion<NArrow::NSSA::TExecutionResult> IDataSource::ReserveFulltextText(
    const NArrow::NSSA::TProcessorContext& context, const NArrow::NSSA::TFulltextMatchRequest& request) {
    const TFetchedData::TFulltextMatchState* state = GetStageData().GetFulltextMatchStateOptional();
    if (!state || !state->NeedText || TextIsPresent(context, request.ColumnId) || !HasPortionAccessor()) {
        return NArrow::NSSA::TExecutionResult::Done();
    }
    const ui64 bytes = GetColumnBlobBytes(std::set<ui32>{request.ColumnId});
    if (bytes == 0) {
        return NArrow::NSSA::TExecutionResult::Done();
    }
    return StartProgramStepReserveMemory(*this, bytes, NArrow::NSSA::IMemoryCalculationPolicy::EStage::Filter);
}

TConclusion<NArrow::NSSA::TExecutionResult> IDataSource::FetchFulltextText(
    const NArrow::NSSA::TProcessorContext& context, const NArrow::NSSA::TFulltextMatchRequest& request) {
    const TFetchedData::TFulltextMatchState* state = GetStageData().GetFulltextMatchStateOptional();
    if (!state || !state->NeedText || TextIsPresent(context, request.ColumnId)) {
        return NArrow::NSSA::TExecutionResult::Done();
    }
    if (!HasPortionAccessor()) {
        return Fail("fulltext match source has no portion");
    }
    return StartTextFetch(*this, context, request);
}

TConclusion<NArrow::NSSA::TExecutionResult> IDataSource::EmitFulltextMatch(
    const NArrow::NSSA::TProcessorContext& context, const NArrow::NSSA::TFulltextMatchRequest& request, const ui32 outputColumnId) {
    const TFetchedData::TFulltextMatchState* state = GetStageData().GetFulltextMatchStateOptional();
    if (!state || !state->IndexCollected) {
        return Fail("fulltext match did not finish index resolution");
    }
    if (std::shared_ptr<IKernelFetchLogic> fetcher = MutableStageData().ExtractFetcherOptional(request.ColumnId)) {
        TFetchingResultContext fetchContext(context.MutableResources(), *GetStageData().GetIndexes(), *this);
        const TConclusionStatus collected = fetcher->OnDataCollected(fetchContext);
        if (collected.IsFail()) {
            return TConclusionStatus::Fail(collected.GetErrorMessage());
        }
    }

    const ui32 portionRows = HasPortionAccessor() ? GetRecordsCount() : state->PortionMask.size();
    const TRowSpace space = DescribeRows(context.GetResources(), portionRows);
    if (!state->NeedText) {
        if (!state->HasTokenMask) {
            return Fail("fulltext match has no token mask");
        }
        auto program = NIndexes::NFulltext::MapFulltextMaskToProgramRows(state->PortionMask, space.Shrinking, space.ProgramRows);
        if (program.IsFail()) {
            return Fail(program.GetErrorMessage());
        }
        return AddBoolean(context, outputColumnId, program.GetResult());
    }

    std::vector<std::optional<TString>> texts;
    if (const std::shared_ptr<NArrow::NAccessor::IChunkedArray>& accessor = context.GetResources().GetAccessorOptional(request.ColumnId)) {
        const ui32 length = accessor->GetRecordsCount();
        if (length != space.PortionRows && length != space.ProgramRows) {
            return Fail("fulltext text column is not aligned with source rows");
        }
        auto read = NArrow::NSSA::ReadFulltextTexts(accessor);
        if (read.IsFail()) {
            return Fail(read.GetErrorMessage());
        }
        texts = read.DetachResult();
    } else if (context.GetResources().GetConstantScalarOptional(request.ColumnId)) {
        auto read = ReadScalarTexts(context, request.ColumnId, space.PortionRows);
        if (read.IsFail()) {
            return Fail(read.GetErrorMessage());
        }
        texts = read.DetachResult();
    } else {
        return Fail("Fulltext text column is not available");
    }

    if (state->HasTokenMask) {
        if (texts.size() == state->PortionMask.size()) {
            auto portion = NArrow::NSSA::ApplyFulltextWildcardResidual(request.Query, state->PortionMask, texts);
            if (portion.IsFail()) {
                return Fail(portion.GetErrorMessage());
            }
            auto program = NIndexes::NFulltext::MapFulltextMaskToProgramRows(portion.GetResult(), space.Shrinking, space.ProgramRows);
            if (program.IsFail()) {
                return Fail(program.GetErrorMessage());
            }
            return AddBoolean(context, outputColumnId, program.GetResult());
        }
        if (texts.size() == space.ProgramRows) {
            auto program = NIndexes::NFulltext::MapFulltextMaskToProgramRows(state->PortionMask, space.Shrinking, space.ProgramRows);
            if (program.IsFail()) {
                return Fail(program.GetErrorMessage());
            }
            auto matched = NArrow::NSSA::ApplyFulltextWildcardResidual(request.Query, program.GetResult(), texts);
            if (matched.IsFail()) {
                return Fail(matched.GetErrorMessage());
            }
            return AddBoolean(context, outputColumnId, matched.GetResult());
        }
        return Fail("fulltext text column is not aligned with source rows");
    }

    if (texts.size() == space.PortionRows) {
        auto portion = NArrow::NSSA::EvaluateFulltextTexts(request.Query, texts);
        if (portion.IsFail()) {
            return Fail(portion.GetErrorMessage());
        }
        auto program = NIndexes::NFulltext::MapFulltextMaskToProgramRows(portion.GetResult(), space.Shrinking, space.ProgramRows);
        if (program.IsFail()) {
            return Fail(program.GetErrorMessage());
        }
        return AddBoolean(context, outputColumnId, program.GetResult());
    }
    if (texts.size() == space.ProgramRows && space.Shrinking) {
        auto program = NArrow::NSSA::EvaluateFulltextTexts(request.Query, texts);
        if (program.IsFail()) {
            return Fail(program.GetErrorMessage());
        }
        if (program.GetResult().size() != space.ProgramRows) {
            return Fail("fulltext text column is not aligned with source rows");
        }
        return AddBoolean(context, outputColumnId, program.GetResult());
    }
    return Fail("fulltext text column is not aligned with source rows");
}

}
