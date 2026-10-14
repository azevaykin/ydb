#include "format.h"
#include "meta.h"

#include <ydb/core/formats/arrow/accessor/abstract/accessor.h>
#include <ydb/core/formats/arrow/save_load/loader.h>
#include <ydb/core/tx/columnshard/engines/scheme/index_info.h>
#include <ydb/core/tx/columnshard/engines/storage/chunks/data.h>

#include <contrib/libs/apache/arrow/cpp/src/arrow/scalar.h>
#include <contrib/libs/apache/arrow/cpp/src/arrow/type.h>
#include <util/generic/ylimits.h>

#include <algorithm>
#include <map>

namespace NKikimr::NOlap::NIndexes::NFulltext {

namespace {

constexpr TStringBuf SkipChunkBytes = "single row exceeds the fulltext chunk byte limit";
constexpr TStringBuf SkipMemory = "fulltext construction memory budget exceeded";
constexpr TStringBuf SkipAnalyzerInput = "fulltext analyzer input budget exceeded";
constexpr TStringBuf SkipAnalyzerTokens = "fulltext analyzer generated-token budget exceeded";
constexpr TStringBuf SkipAnalyzerRetained = "fulltext analyzer retained-byte budget exceeded";
constexpr TStringBuf SkipChunkAddress = "fulltext chunk address space is exhausted";

bool IsTextType(const arrow::Type::type typeId) {
    return typeId == arrow::Type::STRING || typeId == arrow::Type::BINARY || typeId == arrow::Type::LARGE_STRING ||
           typeId == arrow::Type::LARGE_BINARY;
}

TConclusion<std::optional<TStringBuf>> TextFromScalar(const std::shared_ptr<arrow::Scalar>& scalar) {
    if (!scalar || !scalar->is_valid) {
        return std::optional<TStringBuf>();
    }
    if (!IsTextType(scalar->type->id())) {
        return TConclusionStatus::Fail("fulltext column value is not String or Utf8");
    }
    const auto& binary = static_cast<const arrow::BaseBinaryScalar&>(*scalar);
    return std::optional<TStringBuf>(
        binary.value ? TStringBuf(reinterpret_cast<const char*>(binary.value->data()), binary.value->size()) : TStringBuf());
}

TString AnalyzerSkipReason(const NKikimr::NFulltext::EAnalyzeBudgetStatus status) {
    switch (status) {
        case NKikimr::NFulltext::EAnalyzeBudgetStatus::InputExceeded:
            return TString(SkipAnalyzerInput);
        case NKikimr::NFulltext::EAnalyzeBudgetStatus::GeneratedTokensExceeded:
            return TString(SkipAnalyzerTokens);
        case NKikimr::NFulltext::EAnalyzeBudgetStatus::RetainedBytesExceeded:
            return TString(SkipAnalyzerRetained);
        case NKikimr::NFulltext::EAnalyzeBudgetStatus::Ok:
            return {};
    }
    return TString(SkipAnalyzerTokens);
}

struct TOpenChunk {
    ui32 Rows = 0;
    std::map<TString, std::vector<ui32>> Postings;
    ui64 TokenCount = 0;
    ui64 TokenBytes = 0;
    ui64 RowBytes = 0;
    ui64 DictionaryBytes = 0;
    ui64 PostingBytes = 0;

    ui64 DictMemory() const {
        // Include map nodes, vector capacity growth, and the temporary encoder
        // descriptors. TString may retain capacity beyond its logical length.
        return 2 * TokenBytes + 2 * RowBytes +
               TokenCount * (sizeof(std::pair<const TString, std::vector<ui32>>) + 4 * sizeof(void*) + sizeof(TFulltextPostingList));
    }

    ui64 Serialized(const ui32 settingsBytes) const {
        return FulltextChunkSerializedBytes(settingsBytes, DictionaryBytes, PostingBytes);
    }
};

struct TPredicted {
    ui64 TokenCount = 0;
    ui64 TokenBytes = 0;
    ui64 RowBytes = 0;
    ui64 DictionaryBytes = 0;
    ui64 PostingBytes = 0;

    ui64 DictMemory() const {
        // Include map nodes, vector capacity growth, and the temporary encoder
        // descriptors. TString may retain capacity beyond its logical length.
        return 2 * TokenBytes + 2 * RowBytes +
               TokenCount * (sizeof(std::pair<const TString, std::vector<ui32>>) + 4 * sizeof(void*) + sizeof(TFulltextPostingList));
    }

    ui64 Serialized(const ui32 settingsBytes) const {
        return FulltextChunkSerializedBytes(settingsBytes, DictionaryBytes, PostingBytes);
    }
};

TPredicted Predict(const TOpenChunk& chunk, const TVector<TString>& tokens, const ui32 localRow) {
    TPredicted next{ chunk.TokenCount, chunk.TokenBytes, chunk.RowBytes, chunk.DictionaryBytes, chunk.PostingBytes };
    for (const auto& token : tokens) {
        const auto it = chunk.Postings.find(token);
        if (it == chunk.Postings.end()) {
            ++next.TokenCount;
            next.TokenBytes += token.size();
            next.RowBytes += sizeof(ui32);
            next.DictionaryBytes += 16 + token.size();
            next.PostingBytes += FulltextVarintSize(localRow);
        } else {
            const ui32 previous = it->second.back();
            next.RowBytes += sizeof(ui32);
            next.PostingBytes += FulltextVarintSize(static_cast<ui64>(localRow) - previous);
        }
    }
    return next;
}

void Commit(TOpenChunk& chunk, const TVector<TString>& tokens, const ui32 localRow) {
    for (const auto& token : tokens) {
        const auto it = chunk.Postings.find(token);
        if (it == chunk.Postings.end()) {
            chunk.Postings.emplace(token, std::vector<ui32>{ localRow });
            ++chunk.TokenCount;
            chunk.TokenBytes += token.size();
            chunk.RowBytes += sizeof(ui32);
            chunk.DictionaryBytes += 16 + token.size();
            chunk.PostingBytes += FulltextVarintSize(localRow);
        } else {
            const ui32 previous = it->second.back();
            it->second.push_back(localRow);
            chunk.RowBytes += sizeof(ui32);
            chunk.PostingBytes += FulltextVarintSize(static_cast<ui64>(localRow) - previous);
        }
    }
    ++chunk.Rows;
}

bool ChunkBytesFit(const ui64 serialized, const i64 maxChunkBytes) {
    return maxChunkBytes > 0 && serialized <= static_cast<ui64>(maxChunkBytes);
}

bool MemoryFits(const ui64 finalized, const ui64 dictMemory, const ui64 serialized, const ui64 budget) {
    if (dictMemory > budget || serialized > (budget - dictMemory) / 2) {
        return false;
    }
    const ui64 live = dictMemory + 2 * serialized;
    return finalized <= budget - live;
}

TVector<TString> UniqueTokens(TVector<TString> tokens) {
    std::sort(tokens.begin(), tokens.end());
    tokens.erase(std::unique(tokens.begin(), tokens.end()), tokens.end());
    return tokens;
}

}   // namespace

template <class TReadDocument>
TConclusion<TIndexBuildOutcome> BuildFulltextFromReader(
    const ui32 recordsCount, TReadDocument&& readDocument, const TFulltextIndexMeta& meta, const TIndexBuildContext& context) {
    Y_UNUSED(context.TargetTier);
    if (!recordsCount) {
        return TIndexBuildOutcome::Built({});
    }
    if (context.MaxChunkBytes <= 0) {
        return TConclusionStatus::Fail("fulltext build context is missing a positive chunk byte limit");
    }

    const auto analyzers = NKikimr::NFulltext::NormalizeAnalyzers(meta.GetAnalyzers());
    const ui32 settingsBytes = static_cast<ui32>(CanonicalAnalyzerSettings(analyzers).size());
    NKikimr::NFulltext::TAnalyzeBudget analyzerBudget;
    analyzerBudget.MaxInputBytes = context.AnalyzerMaxInputBytes;
    analyzerBudget.MaxGeneratedTokens = context.AnalyzerMaxGeneratedTokens;
    analyzerBudget.MaxRetainedBytes = context.AnalyzerMaxRetainedBytes;

    TOpenChunk open;
    ui64 finalized = 0;
    std::vector<std::shared_ptr<NChunks::TPortionIndexChunk>> chunks;

    const auto close = [&](const ui64 scratch = 0) -> TConclusionStatus {
        if (open.Rows == 0) {
            return TConclusionStatus::Success();
        }
        if (chunks.size() > Max<ui16>()) {
            return TConclusionStatus::Fail(TString(SkipChunkAddress));
        }
        const ui64 serialized = open.Serialized(settingsBytes);
        if (!ChunkBytesFit(serialized, context.MaxChunkBytes)) {
            return TConclusionStatus::Fail(TString(SkipChunkBytes));
        }
        if (scratch > context.ConstructionMemoryBudget ||
            !MemoryFits(finalized, open.DictMemory(), serialized, context.ConstructionMemoryBudget - scratch)) {
            return TConclusionStatus::Fail(TString(SkipMemory));
        }
        TFulltextChunkPlain plain;
        plain.ColumnId = meta.GetColumnId();
        plain.IndexId = meta.GetIndexId();
        plain.Analyzers = analyzers;
        plain.ChunkRowCount = open.Rows;
        plain.Postings.reserve(open.Postings.size());
        for (auto& [token, rows] : open.Postings) {
            plain.Postings.push_back(TFulltextPostingList{ token, std::move(rows) });
        }
        auto encoded = EncodeFulltextChunk(std::move(plain));
        if (encoded.IsFail()) {
            return TConclusionStatus::Fail(encoded.GetErrorMessage());
        }
        TString blob = encoded.DetachResult();
        if (blob.size() != serialized) {
            return TConclusionStatus::Fail("fulltext chunk size estimate diverged from the encoder");
        }
        const ui16 chunkIdx = static_cast<ui16>(chunks.size());
        chunks.push_back(
            std::make_shared<NChunks::TPortionIndexChunk>(TChunkAddress(meta.GetIndexId(), chunkIdx), open.Rows, blob.size(), blob));
        finalized += blob.size() + 2 * sizeof(std::shared_ptr<NChunks::TPortionIndexChunk>) + sizeof(NChunks::TPortionIndexChunk);
        open = TOpenChunk{};
        return TConclusionStatus::Success();
    };

    for (ui32 row = 0; row < recordsCount; ++row) {
        auto document = readDocument(row);
        if (document.IsFail()) {
            return TConclusionStatus::Fail(document.GetErrorMessage());
        }
        TVector<TString> tokens;
        if (document.GetResult()) {
            if (finalized > context.ConstructionMemoryBudget || open.DictMemory() > context.ConstructionMemoryBudget - finalized) {
                return TIndexBuildOutcome::Skipped(TString(SkipMemory));
            }
            const ui64 remaining = context.ConstructionMemoryBudget - finalized - open.DictMemory();
            auto rowBudget = analyzerBudget;
            rowBudget.MaxRetainedBytes = std::min(rowBudget.MaxRetainedBytes, remaining / 2);
            rowBudget.MaxGeneratedTokens = std::min(rowBudget.MaxGeneratedTokens, remaining / (2 * sizeof(TString)));
            auto analyzed = NKikimr::NFulltext::AnalyzeBounded(*document.GetResult(), analyzers, rowBudget);
            if (!analyzed.Ok()) {
                return TIndexBuildOutcome::Skipped(AnalyzerSkipReason(analyzed.Status));
            }
            tokens = UniqueTokens(std::move(analyzed.Tokens));
        }
        ui64 scratch = tokens.capacity() * sizeof(TString);
        for (const auto& token : tokens) {
            scratch += token.capacity();
        }
        if (scratch > context.ConstructionMemoryBudget) {
            return TIndexBuildOutcome::Skipped(TString(SkipMemory));
        }
        const ui64 available = context.ConstructionMemoryBudget - scratch;
        const auto predicted = Predict(open, tokens, open.Rows);
        const ui64 predictedBytes = predicted.Serialized(settingsBytes);
        const bool fits =
            ChunkBytesFit(predictedBytes, context.MaxChunkBytes) && MemoryFits(finalized, predicted.DictMemory(), predictedBytes, available);
        if (!fits) {
            if (open.Rows == 0) {
                if (!ChunkBytesFit(predictedBytes, context.MaxChunkBytes)) {
                    return TIndexBuildOutcome::Skipped(TString(SkipChunkBytes));
                }
                return TIndexBuildOutcome::Skipped(TString(SkipMemory));
            }
            auto closed = close(scratch);
            if (closed.IsFail()) {
                const TString reason = closed.GetErrorMessage();
                if (reason == SkipMemory || reason == SkipChunkBytes || reason == SkipChunkAddress) {
                    return TIndexBuildOutcome::Skipped(reason);
                }
                return closed;
            }
            const auto retry = Predict(open, tokens, open.Rows);
            const ui64 retryBytes = retry.Serialized(settingsBytes);
            if (!ChunkBytesFit(retryBytes, context.MaxChunkBytes)) {
                return TIndexBuildOutcome::Skipped(TString(SkipChunkBytes));
            }
            if (!MemoryFits(finalized, retry.DictMemory(), retryBytes, available)) {
                return TIndexBuildOutcome::Skipped(TString(SkipMemory));
            }
        }
        Commit(open, tokens, open.Rows);
    }

    auto closed = close();
    if (closed.IsFail()) {
        const TString reason = closed.GetErrorMessage();
        if (reason == SkipMemory || reason == SkipChunkBytes || reason == SkipChunkAddress) {
            return TIndexBuildOutcome::Skipped(reason);
        }
        return closed;
    }

    ui32 covered = 0;
    for (const auto& chunk : chunks) {
        covered += chunk->GetRecordsCountVerified();
    }
    if (covered != recordsCount) {
        return TConclusionStatus::Fail("fulltext chunks do not cover the portion");
    }
    return TIndexBuildOutcome::Built(std::move(chunks));
}

TConclusion<TIndexBuildOutcome> BuildFulltextFromDocuments(
    const std::vector<std::optional<TString>>& documents, const TFulltextIndexMeta& meta, const TIndexBuildContext& context) {
    if (documents.size() > Max<ui32>()) {
        return TConclusionStatus::Fail("fulltext column has too many rows");
    }
    return BuildFulltextFromReader(documents.size(), [&](ui32 row) -> TConclusion<std::optional<TStringBuf>> {
        return documents[row] ? std::optional<TStringBuf>(*documents[row]) : std::nullopt;
    }, meta, context);
}

TConclusion<TIndexBuildOutcome> BuildFulltextIndex(const THashMap<ui32, std::vector<std::shared_ptr<IPortionDataChunk>>>& data,
    const ui32 recordsCount, const NArrow::NAccessor::TColumnLoader& loader, const TFulltextIndexMeta& meta, const TIndexBuildContext& context) {
    if (!recordsCount) {
        return TIndexBuildOutcome::Built({});
    }
    if (!loader.GetField() || !loader.GetField()->type() || !IsTextType(loader.GetField()->type()->id())) {
        return TConclusionStatus::Fail("fulltext index column is not String or Utf8");
    }
    std::vector<std::shared_ptr<IPortionDataChunk>> chunks;
    if (auto it = data.find(meta.GetColumnId()); it != data.end()) {
        chunks = it->second;
    }
    std::stable_sort(chunks.begin(), chunks.end(), [](const auto& left, const auto& right) {
        return left->GetChunkIdxOptional().value_or(0) < right->GetChunkIdxOptional().value_or(0);
    });
    ui64 covered = 0;
    ui64 inputBytes = 0;
    for (const auto& chunk : chunks) {
        if (chunk->GetEntityId() != meta.GetColumnId() || !chunk->GetRecordsCountVerified()) {
            return TConclusionStatus::Fail("fulltext column chunk identity or record count is invalid");
        }
        covered += chunk->GetRecordsCountVerified();
        inputBytes = std::max(inputBytes, chunk->GetRawBytesVerified());
    }
    if (!chunks.empty() && covered != recordsCount) {
        return TConclusionStatus::Fail("fulltext column chunks do not cover the portion rows");
    }
    std::shared_ptr<NArrow::NAccessor::IChunkedArray> array;
    if (chunks.empty()) {
        array = loader.BuildDefaultAccessor(recordsCount);
        inputBytes = array->GetRawSize().value_or(static_cast<ui64>(array->GetValueRawBytes()) * recordsCount);
    }
    const ui64 descriptors = chunks.capacity() * sizeof(std::shared_ptr<IPortionDataChunk>);
    if (inputBytes > context.ConstructionMemoryBudget || descriptors > context.ConstructionMemoryBudget - inputBytes) {
        return TIndexBuildOutcome::Skipped(TString(SkipMemory));
    }
    auto buildContext = context;
    buildContext.ConstructionMemoryBudget -= inputBytes + descriptors;
    std::shared_ptr<arrow::Scalar> scalar;
    size_t nextChunk = 0;
    ui32 chunkStart = 0;
    // Retain only the current decoded input chunk. Scalar bytes are borrowed
    // until the next call; the builder finishes each document synchronously.
    return BuildFulltextFromReader(recordsCount, [&](ui32 row) -> TConclusion<std::optional<TStringBuf>> {
        if (!array || row - chunkStart == array->GetRecordsCount()) {
            if (array) {
                chunkStart += array->GetRecordsCount();
                scalar.reset();
                array.reset();
            }
            if (chunks.empty()) {
                array = loader.BuildDefaultAccessor(recordsCount);
            } else {
                const auto& chunk = chunks.at(nextChunk++);
                auto applied = loader.ApplyConclusion(
                    chunk->GetData(), chunk->GetRecordsCountVerified(), std::nullopt, chunk->GetAdditionalAccessorDataOptional());
                if (applied.IsFail()) {
                    return TConclusionStatus::Fail(applied.GetErrorMessage());
                }
                array = applied.DetachResult();
                if (array->GetRecordsCount() != chunk->GetRecordsCountVerified()) {
                    return TConclusionStatus::Fail("fulltext column chunk record count does not match its blob");
                }
            }
        }
        scalar = array->GetScalar(row - chunkStart);
        return TextFromScalar(scalar);
    }, meta, buildContext);
}

TConclusion<TIndexBuildOutcome> TFulltextIndexMeta::DoBuildIndexOptional(
    const THashMap<ui32, std::vector<std::shared_ptr<IPortionDataChunk>>>& data, const ui32 recordsCount, const TIndexInfo& indexInfo,
    const TIndexBuildContext& context) const {
    const auto& loader = indexInfo.GetColumnLoaderOptional(ColumnId);
    if (!loader) {
        return TConclusionStatus::Fail("fulltext index column is missing from the portion schema");
    }
    return BuildFulltextIndex(data, recordsCount, *loader, *this, context);
}

}   // namespace NKikimr::NOlap::NIndexes::NFulltext
