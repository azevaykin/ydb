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
    return typeId == arrow::Type::STRING || typeId == arrow::Type::BINARY || typeId == arrow::Type::LARGE_STRING || typeId == arrow::Type::LARGE_BINARY;
}

TConclusion<std::optional<TString>> TextFromScalar(const std::shared_ptr<arrow::Scalar>& scalar) {
    if (!scalar || !scalar->is_valid) {
        return std::optional<TString>();
    }
    if (!IsTextType(scalar->type->id())) {
        return TConclusionStatus::Fail("fulltext column value is not String or Utf8");
    }
    const auto& binary = static_cast<const arrow::BaseBinaryScalar&>(*scalar);
    if (!binary.value) {
        return std::optional<TString>(TString());
    }
    return std::optional<TString>(TString(reinterpret_cast<const char*>(binary.value->data()), binary.value->size()));
}

TConclusion<std::vector<std::optional<TString>>> ReadDocuments(const THashMap<ui32, std::vector<std::shared_ptr<IPortionDataChunk>>>& data,
    const ui32 recordsCount, const NArrow::NAccessor::TColumnLoader& loader, const ui32 columnId) {
    if (!loader.GetField() || !loader.GetField()->type() || !IsTextType(loader.GetField()->type()->id())) {
        return TConclusionStatus::Fail("fulltext index column is not String or Utf8");
    }
    std::vector<std::optional<TString>> documents;
    documents.reserve(recordsCount);
    const auto appendArray = [&](const std::shared_ptr<NArrow::NAccessor::IChunkedArray>& array) -> TConclusionStatus {
        if (array->GetRecordsCount() > Max<ui32>()) {
            return TConclusionStatus::Fail("fulltext column chunk has too many rows");
        }
        for (ui32 i = 0; i < static_cast<ui32>(array->GetRecordsCount()); ++i) {
            auto text = TextFromScalar(array->GetScalar(i));
            if (text.IsFail()) {
                return TConclusionStatus::Fail(text.GetErrorMessage());
            }
            documents.push_back(std::move(text.DetachResult()));
        }
        return TConclusionStatus::Success();
    };

    const auto it = data.find(columnId);
    if (it == data.end() || it->second.empty()) {
        // Same accessor ISnapshotSchema::NormalizeBatch uses when the source batch has no physical column.
        // A missing blob is the column default, which is null only when the schema default is null.
        auto conclusion = appendArray(loader.BuildDefaultAccessor(recordsCount));
        if (conclusion.IsFail()) {
            return conclusion;
        }
    } else {
        auto chunks = it->second;
        std::stable_sort(chunks.begin(), chunks.end(), [](const std::shared_ptr<IPortionDataChunk>& left, const std::shared_ptr<IPortionDataChunk>& right) {
            return left->GetChunkIdxOptional().value_or(0) < right->GetChunkIdxOptional().value_or(0);
        });
        for (const auto& chunk : chunks) {
            if (chunk->GetEntityId() != columnId) {
                return TConclusionStatus::Fail("fulltext column chunk entity does not match the indexed column");
            }
            auto applied = loader.ApplyConclusion(chunk->GetData(), chunk->GetRecordsCountVerified(), std::nullopt, chunk->GetAdditionalAccessorDataOptional());
            if (applied.IsFail()) {
                return TConclusionStatus::Fail(applied.GetErrorMessage());
            }
            auto array = applied.DetachResult();
            if (array->GetRecordsCount() != chunk->GetRecordsCountVerified()) {
                return TConclusionStatus::Fail("fulltext column chunk record count does not match its blob");
            }
            auto conclusion = appendArray(array);
            if (conclusion.IsFail()) {
                return conclusion;
            }
        }
    }
    if (documents.size() != recordsCount) {
        return TConclusionStatus::Fail("fulltext column chunks do not cover the portion rows");
    }
    return documents;
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
    ui64 TokenBytes = 0;
    ui64 RowBytes = 0;
    ui64 DictionaryBytes = 0;
    ui64 PostingBytes = 0;

    ui64 DictMemory() const {
        return TokenBytes + RowBytes;
    }

    ui64 Serialized(const ui32 settingsBytes) const {
        return FulltextChunkSerializedBytes(settingsBytes, DictionaryBytes, PostingBytes);
    }
};

struct TPredicted {
    ui64 TokenBytes = 0;
    ui64 RowBytes = 0;
    ui64 DictionaryBytes = 0;
    ui64 PostingBytes = 0;

    ui64 DictMemory() const {
        return TokenBytes + RowBytes;
    }

    ui64 Serialized(const ui32 settingsBytes) const {
        return FulltextChunkSerializedBytes(settingsBytes, DictionaryBytes, PostingBytes);
    }
};

TPredicted Predict(const TOpenChunk& chunk, const TVector<TString>& tokens, const ui32 localRow) {
    TPredicted next{chunk.TokenBytes, chunk.RowBytes, chunk.DictionaryBytes, chunk.PostingBytes};
    for (const auto& token : tokens) {
        const auto it = chunk.Postings.find(token);
        if (it == chunk.Postings.end()) {
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
            chunk.Postings.emplace(token, std::vector<ui32>{localRow});
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
    if (dictMemory > budget || serialized > budget - dictMemory) {
        return false;
    }
    const ui64 live = dictMemory + serialized;
    return finalized <= budget - live;
}

TVector<TString> UniqueTokens(TVector<TString> tokens) {
    std::sort(tokens.begin(), tokens.end());
    tokens.erase(std::unique(tokens.begin(), tokens.end()), tokens.end());
    return tokens;
}

}

TConclusion<TIndexBuildOutcome> BuildFulltextFromDocuments(const std::vector<std::optional<TString>>& documents, const TFulltextIndexMeta& meta,
    const TIndexBuildContext& context) {
    Y_UNUSED(context.TargetTier);
    if (documents.empty()) {
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

    const auto close = [&]() -> TConclusionStatus {
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
        if (!MemoryFits(finalized, open.DictMemory(), serialized, context.ConstructionMemoryBudget)) {
            return TConclusionStatus::Fail(TString(SkipMemory));
        }
        TFulltextChunkPlain plain;
        plain.ColumnId = meta.GetColumnId();
        plain.IndexId = meta.GetIndexId();
        plain.Analyzers = analyzers;
        plain.ChunkRowCount = open.Rows;
        plain.Postings.reserve(open.Postings.size());
        for (const auto& [token, rows] : open.Postings) {
            plain.Postings.push_back(TFulltextPostingList{token, rows});
        }
        auto encoded = EncodeFulltextChunk(plain);
        if (encoded.IsFail()) {
            return TConclusionStatus::Fail(encoded.GetErrorMessage());
        }
        TString blob = encoded.DetachResult();
        if (blob.size() != serialized) {
            return TConclusionStatus::Fail("fulltext chunk size estimate diverged from the encoder");
        }
        const ui16 chunkIdx = static_cast<ui16>(chunks.size());
        chunks.push_back(std::make_shared<NChunks::TPortionIndexChunk>(TChunkAddress(meta.GetIndexId(), chunkIdx), open.Rows, blob.size(), blob));
        finalized += blob.size();
        open = TOpenChunk{};
        return TConclusionStatus::Success();
    };

    for (const auto& document : documents) {
        TVector<TString> tokens;
        if (document) {
            const auto analyzed = NKikimr::NFulltext::AnalyzeBounded(*document, analyzers, analyzerBudget);
            if (!analyzed.Ok()) {
                return TIndexBuildOutcome::Skipped(AnalyzerSkipReason(analyzed.Status));
            }
            tokens = UniqueTokens(analyzed.Tokens);
        }
        const auto predicted = Predict(open, tokens, open.Rows);
        const ui64 predictedBytes = predicted.Serialized(settingsBytes);
        const bool fits = ChunkBytesFit(predictedBytes, context.MaxChunkBytes) &&
            MemoryFits(finalized, predicted.DictMemory(), predictedBytes, context.ConstructionMemoryBudget);
        if (!fits) {
            if (open.Rows == 0) {
                if (!ChunkBytesFit(predictedBytes, context.MaxChunkBytes)) {
                    return TIndexBuildOutcome::Skipped(TString(SkipChunkBytes));
                }
                return TIndexBuildOutcome::Skipped(TString(SkipMemory));
            }
            auto closed = close();
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
            if (!MemoryFits(finalized, retry.DictMemory(), retryBytes, context.ConstructionMemoryBudget)) {
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
    if (covered != documents.size()) {
        return TConclusionStatus::Fail("fulltext chunks do not cover the portion");
    }
    return TIndexBuildOutcome::Built(std::move(chunks));
}

TConclusion<TIndexBuildOutcome> BuildFulltextIndex(const THashMap<ui32, std::vector<std::shared_ptr<IPortionDataChunk>>>& data, const ui32 recordsCount,
    const NArrow::NAccessor::TColumnLoader& loader, const TFulltextIndexMeta& meta, const TIndexBuildContext& context) {
    if (recordsCount == 0) {
        return TIndexBuildOutcome::Built({});
    }
    auto documents = ReadDocuments(data, recordsCount, loader, meta.GetColumnId());
    if (documents.IsFail()) {
        return TConclusionStatus::Fail(documents.GetErrorMessage());
    }
    return BuildFulltextFromDocuments(documents.DetachResult(), meta, context);
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

} // namespace NKikimr::NOlap::NIndexes::NFulltext
