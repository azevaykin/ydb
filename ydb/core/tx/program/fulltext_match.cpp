#include "fulltext_match.h"

#include <ydb/core/base/fulltext.h>
#include <ydb/library/formats/arrow/validation/validation.h>

#include <library/cpp/json/writer/json_value.h>
#include <util/string/builder.h>

#include <contrib/libs/apache/arrow/cpp/src/arrow/array/array_binary.h>
#include <contrib/libs/apache/arrow/cpp/src/arrow/type.h>
#include <contrib/libs/apache/arrow/cpp/src/arrow/type_traits.h>
#include <contrib/libs/apache/arrow/cpp/src/arrow/compute/cast.h>
#include <contrib/libs/apache/arrow/cpp/src/arrow/record_batch.h>
#include <contrib/libs/apache/arrow/cpp/src/arrow/scalar.h>

namespace NKikimr::NArrow::NSSA {

namespace {

enum class EFulltextPhase {
    Reserve,
    Fetch,
    Decide,
    ReserveText,
    FetchText,
    Emit
};

const char* PhaseName(const EFulltextPhase phase) {
    switch (phase) {
        case EFulltextPhase::Reserve:
            return "reserve";
        case EFulltextPhase::Fetch:
            return "fetch";
        case EFulltextPhase::Decide:
            return "decide";
        case EFulltextPhase::ReserveText:
            return "reserve_text";
        case EFulltextPhase::FetchText:
            return "fetch_text";
        case EFulltextPhase::Emit:
            return "emit";
    }
    return "fulltext";
}

class TFulltextMatchProcessor: public IResourceProcessor {
private:
    using TBase = IResourceProcessor;
    std::shared_ptr<const TFulltextMatchRequest> Request;
    EFulltextPhase Phase;

    TConclusion<TExecutionResult> Run(const TProcessorContext& context) const {
        auto* source = dynamic_cast<IFulltextMatchSource*>(&context.GetDataSource());
        if (!source) {
            return TConclusionStatus::Fail("fulltext match is not supported by this scan source");
        }
        switch (Phase) {
            case EFulltextPhase::Reserve:
                return source->ReserveFulltextMatch(context, *Request);
            case EFulltextPhase::Fetch:
                return source->FetchFulltextIndex(context, *Request);
            case EFulltextPhase::Decide:
                return source->DecideFulltextMatch(context, *Request);
            case EFulltextPhase::ReserveText:
                return source->ReserveFulltextText(context, *Request);
            case EFulltextPhase::FetchText:
                return source->FetchFulltextText(context, *Request);
            case EFulltextPhase::Emit:
                return source->EmitFulltextMatch(context, *Request, GetOutputColumnIdOnce());
        }
        return TConclusionStatus::Fail("fulltext match phase is not supported");
    }

    TConclusion<TExecutionResult> DoExecute(const TProcessorContext& context, const TExecutionNodeContext& /*nodeContext*/) const override {
        return Run(context);
    }

    bool IsAggregation() const override {
        return false;
    }

    ui64 DoGetWeight() const override {
        switch (Phase) {
            case EFulltextPhase::Reserve:
                return 1;
            case EFulltextPhase::Fetch:
                return 20;
            case EFulltextPhase::Decide:
            case EFulltextPhase::ReserveText:
            case EFulltextPhase::Emit:
                return 5;
            case EFulltextPhase::FetchText:
                return 20;
        }
        return 1;
    }

    TString DoGetSignalCategoryName() const override {
        return TStringBuilder() << "FulltextMatch::" << PhaseName(Phase);
    }

    NJson::TJsonValue DoDebugJson() const override {
        NJson::TJsonValue result = NJson::JSON_MAP;
        result.InsertValue("op", "fulltext_match");
        result.InsertValue("phase", PhaseName(Phase));
        result.InsertValue("exact", true);
        if (Request) {
            result.InsertValue("index_id", Request->IndexId);
            result.InsertValue("column_id", Request->ColumnId);
            result.InsertValue("analyzer_revision", Request->Query.AnalyzerRevision);
            result.InsertValue("mode", Request->Query.Mode == NFulltext::EFulltextQueryMode::Wildcard ? "wildcard" : "keywords");
        }
        return result;
    }

    static std::vector<TColumnChainInfo> Chain(const ui32 columnId) {
        if (!columnId) {
            return {};
        }
        return {TColumnChainInfo(columnId)};
    }

public:
    TFulltextMatchProcessor(std::shared_ptr<const TFulltextMatchRequest> request, const EFulltextPhase phase, const ui32 inputId, const ui32 outputId)
        : TBase(Chain(inputId), Chain(outputId), EProcessorType::FulltextMatch)
        , Request(std::move(request))
        , Phase(phase) {
    }
};

TConclusionStatus AppendTextChunk(std::vector<std::optional<TString>>& out, const arrow::Array& chunk, const int depth) {
    switch (chunk.type_id()) {
        case arrow::Type::NA:
            out.insert(out.end(), chunk.length(), std::nullopt);
            return TConclusionStatus::Success();
        case arrow::Type::STRING:
        case arrow::Type::BINARY:
        case arrow::Type::LARGE_STRING:
        case arrow::Type::LARGE_BINARY: {
            if (chunk.type_id() == arrow::Type::LARGE_STRING || chunk.type_id() == arrow::Type::LARGE_BINARY) {
                const auto& array = static_cast<const arrow::LargeBinaryArray&>(chunk);
                for (int64_t i = 0; i < array.length(); ++i) {
                    if (!array.IsValid(i)) {
                        out.emplace_back();
                        continue;
                    }
                    const auto view = array.GetView(i);
                    out.emplace_back(TString(view.data(), view.size()));
                }
                return TConclusionStatus::Success();
            }
            const auto& array = static_cast<const arrow::BinaryArray&>(chunk);
            for (int64_t i = 0; i < array.length(); ++i) {
                if (!array.IsValid(i)) {
                    out.emplace_back();
                    continue;
                }
                const auto view = array.GetView(i);
                out.emplace_back(TString(view.data(), view.size()));
            }
            return TConclusionStatus::Success();
        }
        case arrow::Type::DICTIONARY: {
            if (depth > 0) {
                return TConclusionStatus::Fail("Fulltext match column must be String or Utf8");
            }
            const auto& dictType = static_cast<const arrow::DictionaryType&>(*chunk.type());
            const auto decoded = arrow::compute::Cast(chunk, dictType.value_type());
            if (!decoded.ok()) {
                return TConclusionStatus::Fail(TString(decoded.status().message()));
            }
            const auto array = decoded->make_array();
            if (!array) {
                return TConclusionStatus::Fail("Fulltext match column must be String or Utf8");
            }
            return AppendTextChunk(out, *array, depth + 1);
        }
        default:
            return TConclusionStatus::Fail("Fulltext match column must be String or Utf8");
    }
}

TConclusion<TString> ReadBoundQuery(
    const NKikimrSSA::TProgram::TAssignment::TFulltextMatch& match, const std::shared_ptr<arrow::RecordBatch>& parameterValues) {
    if (match.HasQueryText()) {
        return match.GetQueryText();
    }
    if (!match.HasQueryParameter()) {
        return TConclusionStatus::Fail("Fulltext query is missing");
    }
    if (!parameterValues) {
        return TConclusionStatus::Fail("Fulltext query parameter is not bound");
    }
    const auto& name = match.GetQueryParameter();
    if (name.empty()) {
        return TConclusionStatus::Fail("Fulltext query parameter name is empty");
    }
    auto column = parameterValues->GetColumnByName(name);
    if (!column || column->length() != 1) {
        return TConclusionStatus::Fail("Fulltext query parameter is not bound: " + name);
    }
    const auto scalar = NArrow::TStatusValidator::GetValid(column->GetScalar(0));
    if (!scalar->is_valid) {
        return TConclusionStatus::Fail("Fulltext query must be a non-null String or Utf8 value");
    }
    switch (scalar->type->id()) {
        case arrow::Type::STRING:
        case arrow::Type::BINARY:
        case arrow::Type::LARGE_STRING:
        case arrow::Type::LARGE_BINARY: {
            const auto& binary = static_cast<const arrow::BaseBinaryScalar&>(*scalar);
            if (!binary.value || binary.value->size() == 0) {
                return TString();
            }
            return TString(reinterpret_cast<const char*>(binary.value->data()), binary.value->size());
        }
        default:
            return TConclusionStatus::Fail("Fulltext query must be a non-null String or Utf8 value");
    }
}

}

TConclusion<std::vector<std::optional<TString>>> ReadFulltextTexts(const std::shared_ptr<NArrow::NAccessor::IChunkedArray>& text) {
    if (!text) {
        return TConclusionStatus::Fail("Fulltext text column is not available");
    }
    if (text->GetDataType()->id() != arrow::Type::NA && !arrow::is_base_binary_like(text->GetDataType()->id()) &&
        text->GetDataType()->id() != arrow::Type::DICTIONARY) {
        return TConclusionStatus::Fail("Fulltext match column must be String or Utf8");
    }
    std::vector<std::optional<TString>> values;
    values.reserve(text->GetRecordsCount());
    const auto chunked = text->GetChunkedArray();
    ui64 seen = 0;
    for (const auto& chunk : chunked->chunks()) {
        auto status = AppendTextChunk(values, *chunk, 0);
        if (status.IsFail()) {
            return status;
        }
        seen += chunk->length();
    }
    if (seen != text->GetRecordsCount()) {
        return TConclusionStatus::Fail("Fulltext text column length does not match its chunks");
    }
    return values;
}

TConclusion<std::optional<TString>> ReadFulltextScalar(const arrow::Scalar& scalar) {
    if (!scalar.is_valid || scalar.type->id() == arrow::Type::NA) {
        return std::optional<TString>();
    }
    switch (scalar.type->id()) {
        case arrow::Type::STRING:
        case arrow::Type::BINARY:
        case arrow::Type::LARGE_STRING:
        case arrow::Type::LARGE_BINARY: {
            const auto& binary = static_cast<const arrow::BaseBinaryScalar&>(scalar);
            if (!binary.value || binary.value->size() == 0) {
                return std::optional<TString>(TString());
            }
            return std::optional<TString>(TString(reinterpret_cast<const char*>(binary.value->data()), binary.value->size()));
        }
        default:
            return TConclusionStatus::Fail("Fulltext match column must be String or Utf8");
    }
}

TConclusion<std::vector<ui8>> EvaluateFulltextTexts(
    const NFulltext::TCompiledFulltextQuery& query, const std::vector<std::optional<TString>>& texts) {
    std::vector<ui8> result;
    result.reserve(texts.size());
    for (const std::optional<TString>& text : texts) {
        std::optional<TStringBuf> view;
        if (text) {
            view = *text;
        }
        result.push_back(NFulltext::EvaluateFulltextText(query, view) ? 1 : 0);
    }
    return result;
}

TConclusion<std::vector<ui8>> ApplyFulltextWildcardResidual(const NFulltext::TCompiledFulltextQuery& query, const std::vector<ui8>& tokenMask,
    const std::vector<std::optional<TString>>& texts) {
    if (query.Mode != NFulltext::EFulltextQueryMode::Wildcard) {
        return TConclusionStatus::Fail("fulltext wildcard residual requires wildcard mode");
    }
    if (tokenMask.size() != texts.size()) {
        return TConclusionStatus::Fail("fulltext wildcard residual is not aligned with the text column");
    }
    std::vector<ui8> result(tokenMask.size());
    for (ui32 i = 0; i < tokenMask.size(); ++i) {
        if (!tokenMask[i] || !texts[i]) {
            result[i] = 0;
            continue;
        }
        result[i] = NFulltext::MatchFulltextWildcardResidual(query, *texts[i]) ? 1 : 0;
    }
    return result;
}

TConclusionStatus AppendFulltextMatch(NGraph::NOptimization::TGraph::TBuilder& builder,
    const NKikimrSSA::TProgram::TAssignment::TFulltextMatch& match, const ui32 outputColumnId,
    const std::shared_ptr<arrow::RecordBatch>& parameterValues) {
    if (outputColumnId == 0 || outputColumnId >= 0x10000000u) {
        return TConclusionStatus::Fail("fulltext match output column id is invalid");
    }
    const ui32 reserveId = 0x10000000u + outputColumnId;
    const ui32 fetchId = 0x20000000u + outputColumnId;
    const ui32 decideId = 0x30000000u + outputColumnId;
    const ui32 reserveTextId = 0x40000000u + outputColumnId;
    const ui32 fetchTextId = 0x50000000u + outputColumnId;
    if (!match.HasAnalyzerSettings()) {
        return TConclusionStatus::Fail("Fulltext analyzer settings are missing");
    }
    Ydb::Table::FulltextIndexSettings::Analyzers parsed;
    if (!parsed.ParseFromString(match.GetAnalyzerSettings())) {
        return TConclusionStatus::Fail("Cannot parse fulltext analyzer settings");
    }
    const auto analyzers = NFulltext::NormalizeAnalyzers(parsed);
    if (NFulltext::FulltextAnalyzerIdentity(analyzers) != match.GetAnalyzerSettings()) {
        return TConclusionStatus::Fail("Fulltext analyzer settings are not canonical");
    }
    if (NFulltext::FulltextAnalyzerRevision(analyzers) != match.GetAnalyzerRevision()) {
        return TConclusionStatus::Fail("Fulltext analyzer revision does not match the analyzer settings");
    }

    auto query = ReadBoundQuery(match, parameterValues);
    if (query.IsFail()) {
        return TConclusionStatus::Fail(query.GetErrorMessage());
    }

    NFulltext::TFulltextQueryOptions options;
    options.Checks = NFulltext::EFulltextQueryChecks::Column;
    options.Mode = match.GetMode();
    options.DefaultOperator = match.GetDefaultOperator();
    options.MinimumShouldMatch = match.GetMinimumShouldMatch();
    auto compiled = NFulltext::CompileFulltextQuery(query.DetachResult(), analyzers, options);
    if (!compiled) {
        return TConclusionStatus::Fail(compiled.Error);
    }
    if (compiled.Compiled->AnalyzerIdentity != match.GetAnalyzerSettings() ||
        compiled.Compiled->AnalyzerRevision != match.GetAnalyzerRevision()) {
        return TConclusionStatus::Fail("Fulltext analyzer identity does not match the scan program");
    }

    auto request = std::make_shared<TFulltextMatchRequest>();
    request->IndexId = match.GetIndexId();
    request->ColumnId = match.GetColumnId();
    request->Query = std::move(*compiled.Compiled);
    const auto shared = std::shared_ptr<const TFulltextMatchRequest>(std::move(request));

    // Text is not an input. Fetch, decide, then emit an exact boolean the filter can apply.
    builder.Add(std::make_shared<TFulltextMatchProcessor>(shared, EFulltextPhase::Reserve, 0, reserveId));
    builder.Add(std::make_shared<TFulltextMatchProcessor>(shared, EFulltextPhase::Fetch, reserveId, fetchId));
    builder.Add(std::make_shared<TFulltextMatchProcessor>(shared, EFulltextPhase::Decide, fetchId, decideId));
    builder.Add(std::make_shared<TFulltextMatchProcessor>(shared, EFulltextPhase::ReserveText, decideId, reserveTextId));
    builder.Add(std::make_shared<TFulltextMatchProcessor>(shared, EFulltextPhase::FetchText, reserveTextId, fetchTextId));
    builder.Add(std::make_shared<TFulltextMatchProcessor>(shared, EFulltextPhase::Emit, fetchTextId, outputColumnId));
    return TConclusionStatus::Success();
}

}
