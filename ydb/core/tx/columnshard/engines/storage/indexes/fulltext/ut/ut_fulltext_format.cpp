#include <ydb/core/tx/columnshard/engines/storage/chunks/data.h>
#include <ydb/core/tx/columnshard/engines/storage/indexes/fulltext/format.h>
#include <ydb/core/tx/columnshard/engines/storage/indexes/fulltext/meta.h>

#include <ydb/core/formats/arrow/accessor/plain/accessor.h>
#include <ydb/core/formats/arrow/serializer/abstract.h>

#include <contrib/libs/apache/arrow/cpp/src/arrow/array/builder_binary.h>
#include <library/cpp/testing/common/env.h>
#include <library/cpp/testing/unittest/registar.h>

#include <util/stream/file.h>

namespace NKikimr::NOlap::NIndexes::NFulltext {
namespace {

using NKikimr::NFulltext::NormalizeAnalyzers;

Ydb::Table::FulltextIndexSettings::Analyzers Analyzers(Ydb::Table::FulltextIndexSettings::Tokenizer tokenizer) {
    Ydb::Table::FulltextIndexSettings::Analyzers analyzers;
    analyzers.set_tokenizer(tokenizer);
    return NormalizeAnalyzers(analyzers);
}

TFulltextIndexMeta Meta(Ydb::Table::FulltextIndexSettings::Tokenizer tokenizer, ui32 columnId = 7, ui32 indexId = 11) {
    return TFulltextIndexMeta(indexId, "ft", "default", true, columnId, Analyzers(tokenizer));
}

TIndexBuildContext Context(i64 maxChunkBytes = 1 << 20, ui64 memory = 64ull << 20) {
    TIndexBuildContext context;
    context.MaxChunkBytes = maxChunkBytes;
    context.TargetTier = "ut";
    context.ConstructionMemoryBudget = memory;
    context.AnalyzerMaxInputBytes = 1ull << 20;
    context.AnalyzerMaxGeneratedTokens = 100000;
    context.AnalyzerMaxRetainedBytes = 1ull << 20;
    return context;
}

std::vector<std::shared_ptr<NChunks::TPortionIndexChunk>> MustBuild(const std::vector<std::optional<TString>>& documents, const TFulltextIndexMeta& meta,
    const TIndexBuildContext& context) {
    auto outcome = BuildFulltextFromDocuments(documents, meta, context);
    if (outcome.IsFail()) {
        UNIT_ASSERT_C(false, outcome.GetErrorMessage());
    }
    UNIT_ASSERT_C(outcome->IsBuilt(), outcome->GetSkipReason());
    return outcome->DetachChunks();
}

TFulltextChunkPlain MustDecode(const TString& bytes) {
    auto decoded = DecodeFulltextChunk(bytes);
    if (decoded.IsFail()) {
        UNIT_ASSERT_C(false, decoded.GetErrorMessage());
    }
    return decoded.DetachResult();
}

ui32 ReadU32At(const TString& bytes, size_t offset) {
    const auto* p = reinterpret_cast<const unsigned char*>(bytes.data() + offset);
    return ui32(p[0]) | (ui32(p[1]) << 8) | (ui32(p[2]) << 16) | (ui32(p[3]) << 24);
}

void WriteU32At(TString& bytes, size_t offset, ui32 value) {
    bytes[offset] = static_cast<char>(value & 0xffu);
    bytes[offset + 1] = static_cast<char>((value >> 8) & 0xffu);
    bytes[offset + 2] = static_cast<char>((value >> 16) & 0xffu);
    bytes[offset + 3] = static_cast<char>((value >> 24) & 0xffu);
}

size_t DictionaryStart(const TString& bytes) {
    return FulltextChunkPrefixBytes + ReadU32At(bytes, 8);
}

TFulltextChunkPlain FixturePlain() {
    TFulltextChunkPlain plain;
    plain.ColumnId = 7;
    plain.IndexId = 11;
    plain.Analyzers = Analyzers(Ydb::Table::FulltextIndexSettings::KEYWORD);
    plain.ChunkRowCount = 4;
    plain.Postings.push_back(TFulltextPostingList{"b", {0}});
    plain.Postings.push_back(TFulltextPostingList{"a", {1, 2}});
    return plain;
}

std::shared_ptr<NArrow::NAccessor::TColumnLoader> MakeLoader(const std::shared_ptr<arrow::Scalar>& defaultValue, ui32 columnId) {
    auto field = std::make_shared<arrow::Field>("text", arrow::utf8());
    return std::make_shared<NArrow::NAccessor::TColumnLoader>(NArrow::NSerialization::TSerializerContainer::GetDefaultSerializer(),
        NArrow::NAccessor::TConstructorContainer::GetDefaultConstructor(), field, defaultValue, columnId);
}

}

Y_UNIT_TEST_SUITE(FulltextChunkFormat) {
    Y_UNIT_TEST(CanonicalSettingsStoreExplicitDefaults) {
        const auto analyzers = Analyzers(Ydb::Table::FulltextIndexSettings::KEYWORD);
        UNIT_ASSERT(!analyzers.has_filter_ngram_min_length());
        UNIT_ASSERT(!analyzers.has_filter_ngram_max_length());
        const TString canonical = CanonicalAnalyzerSettings(analyzers);
        UNIT_ASSERT_VALUES_EQUAL(canonical.size(), 31);
        UNIT_ASSERT_VALUES_EQUAL(static_cast<i32>(ReadU32At(canonical, 12)), FulltextCanonicalNgramMinDefault);
        UNIT_ASSERT_VALUES_EQUAL(static_cast<i32>(ReadU32At(canonical, 16)), FulltextCanonicalNgramMaxDefault);
        auto parsed = ParseCanonicalAnalyzerSettings(canonical);
        UNIT_ASSERT_C(parsed.IsSuccess(), parsed.IsFail() ? parsed.GetErrorMessage() : TString());
        UNIT_ASSERT_VALUES_EQUAL(CanonicalAnalyzerSettings(parsed.GetResult()), canonical);
        UNIT_ASSERT_VALUES_EQUAL(NKikimr::NFulltext::FulltextAnalyzerRevision(parsed.GetResult()), NKikimr::NFulltext::FulltextAnalyzerRevision(analyzers));
    }

    Y_UNIT_TEST(VersionOneFixtureMatchesEncoder) {
        auto encoded = EncodeFulltextChunk(FixturePlain());
        UNIT_ASSERT_C(encoded.IsSuccess(), encoded.IsFail() ? encoded.GetErrorMessage() : TString());
        const TString fixture = TFileInput(ArcadiaSourceRoot() + "/ydb/core/tx/columnshard/engines/storage/indexes/fulltext/ut/format_v1.bin").ReadAll();
        UNIT_ASSERT_VALUES_EQUAL(encoded.GetResult(), fixture);

        const auto decoded = MustDecode(fixture);
        UNIT_ASSERT_VALUES_EQUAL(decoded.ColumnId, 7);
        UNIT_ASSERT_VALUES_EQUAL(decoded.IndexId, 11);
        UNIT_ASSERT_VALUES_EQUAL(decoded.ChunkRowCount, 4);
        UNIT_ASSERT_VALUES_EQUAL(decoded.AnalyzerRevision, NKikimr::NFulltext::FulltextAnalyzerRevision(decoded.Analyzers));
        UNIT_ASSERT_VALUES_EQUAL(decoded.Postings.size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(decoded.Postings[0].Token, "a");
        UNIT_ASSERT_VALUES_EQUAL(decoded.Postings[0].Rows.size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(decoded.Postings[0].Rows[0], 1);
        UNIT_ASSERT_VALUES_EQUAL(decoded.Postings[0].Rows[1], 2);
        UNIT_ASSERT_VALUES_EQUAL(decoded.Postings[1].Token, "b");
        UNIT_ASSERT_VALUES_EQUAL(decoded.Postings[1].Rows.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(decoded.Postings[1].Rows[0], 0);
    }

    Y_UNIT_TEST(MalformedBytesAreErrors) {
        const TString valid = EncodeFulltextChunk(FixturePlain()).DetachResult();

        {
            TString bad = valid;
            bad[0] = 'X';
            UNIT_ASSERT(DecodeFulltextChunk(bad).IsFail());
        }
        {
            TString bad = valid;
            WriteU32At(bad, 4, 2);
            auto decoded = DecodeFulltextChunk(bad);
            UNIT_ASSERT(decoded.IsFail());
            UNIT_ASSERT(decoded.GetErrorMessage().Contains("unsupported fulltext chunk format"));
        }
        {
            TString bad = valid;
            const size_t dict = DictionaryStart(bad);
            const ui32 tokenSize = ReadU32At(bad, dict);
            const size_t offset = dict + 4 + tokenSize + 4;
            WriteU32At(bad, offset, ReadU32At(bad, offset) + 1);
            auto decoded = DecodeFulltextChunk(bad);
            UNIT_ASSERT(decoded.IsFail());
            UNIT_ASSERT(decoded.GetErrorMessage().Contains("offset"));
        }
        {
            TString bad = valid;
            const size_t dict = DictionaryStart(bad);
            const ui32 tokenSize = ReadU32At(bad, dict);
            WriteU32At(bad, dict + 4 + tokenSize, 9);
            auto decoded = DecodeFulltextChunk(bad);
            UNIT_ASSERT(decoded.IsFail());
            UNIT_ASSERT(decoded.GetErrorMessage().Contains("posting"));
        }
        {
            auto chunks = MustBuild({TString("a"), TString("a")}, Meta(Ydb::Table::FulltextIndexSettings::KEYWORD), Context());
            TString bad = chunks.front()->GetData();
            const size_t dict = DictionaryStart(bad);
            const ui32 tokenSize = ReadU32At(bad, dict);
            const ui32 postingOffset = ReadU32At(bad, dict + 4 + tokenSize + 4);
            bad[postingOffset + 1] = 0;
            auto decoded = DecodeFulltextChunk(bad);
            UNIT_ASSERT(decoded.IsFail());
            UNIT_ASSERT(decoded.GetErrorMessage().Contains("strictly increasing"));
        }
        {
            auto chunks = MustBuild({TString("a")}, Meta(Ydb::Table::FulltextIndexSettings::KEYWORD), Context());
            TString bad = chunks.front()->GetData();
            const size_t dict = DictionaryStart(bad);
            const ui32 tokenSize = ReadU32At(bad, dict);
            const size_t lengthOffset = dict + 4 + tokenSize + 8;
            const ui32 postingOffset = ReadU32At(bad, dict + 4 + tokenSize + 4);
            bad[postingOffset] = static_cast<char>(0x80);
            WriteU32At(bad, lengthOffset, 1);
            auto decoded = DecodeFulltextChunk(bad);
            UNIT_ASSERT(decoded.IsFail());
            UNIT_ASSERT(decoded.GetErrorMessage().Contains("varint"));
        }
        {
            TString bad = valid;
            bad.append('\0');
            UNIT_ASSERT(DecodeFulltextChunk(bad).IsFail());
        }
        {
            TString bad = valid;
            const size_t dict = DictionaryStart(bad);
            bad[dict + 4] = 'b';
            auto decoded = DecodeFulltextChunk(bad);
            UNIT_ASSERT(decoded.IsFail());
            UNIT_ASSERT(decoded.GetErrorMessage().Contains("strictly increasing"));
        }
    }
}

Y_UNIT_TEST_SUITE(FulltextChunkBuilder) {
    Y_UNIT_TEST(RowZeroAndBoundaryIds) {
        std::vector<std::optional<TString>> documents(201);
        documents[0] = "x";
        documents[200] = "x";
        const auto chunks = MustBuild(documents, Meta(Ydb::Table::FulltextIndexSettings::KEYWORD), Context());
        UNIT_ASSERT_VALUES_EQUAL(chunks.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(chunks[0]->GetRecordsCountVerified(), 201);
        const auto decoded = MustDecode(chunks[0]->GetData());
        UNIT_ASSERT_VALUES_EQUAL(decoded.Postings.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(decoded.Postings[0].Rows.size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(decoded.Postings[0].Rows[0], 0);
        UNIT_ASSERT_VALUES_EQUAL(decoded.Postings[0].Rows[1], 200);
        const size_t dict = DictionaryStart(chunks[0]->GetData());
        const ui32 tokenSize = ReadU32At(chunks[0]->GetData(), dict);
        UNIT_ASSERT_VALUES_EQUAL(ReadU32At(chunks[0]->GetData(), dict + 4 + tokenSize + 8), 3);
    }

    Y_UNIT_TEST(DuplicateTokensEmitOnePosting) {
        const auto chunks = MustBuild({TString("a a a")}, Meta(Ydb::Table::FulltextIndexSettings::WHITESPACE), Context());
        const auto decoded = MustDecode(chunks[0]->GetData());
        UNIT_ASSERT_VALUES_EQUAL(decoded.ChunkRowCount, 1);
        UNIT_ASSERT_VALUES_EQUAL(decoded.Postings.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(decoded.Postings[0].Token, "a");
        UNIT_ASSERT_VALUES_EQUAL(decoded.Postings[0].Rows.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(decoded.Postings[0].Rows[0], 0);
    }

    Y_UNIT_TEST(NullAndTokenlessChunksKeepCoverage) {
        const auto nulls = MustBuild({std::nullopt, std::nullopt, std::nullopt}, Meta(Ydb::Table::FulltextIndexSettings::KEYWORD), Context());
        const auto nullDecoded = MustDecode(nulls[0]->GetData());
        UNIT_ASSERT_VALUES_EQUAL(nullDecoded.ChunkRowCount, 3);
        UNIT_ASSERT(nullDecoded.Postings.empty());

        const auto empty = MustBuild({TString(), TString()}, Meta(Ydb::Table::FulltextIndexSettings::STANDARD), Context());
        const auto emptyDecoded = MustDecode(empty[0]->GetData());
        UNIT_ASSERT_VALUES_EQUAL(emptyDecoded.ChunkRowCount, 2);
        UNIT_ASSERT(emptyDecoded.Postings.empty());

        const auto keywordEmpty = MustBuild({TString()}, Meta(Ydb::Table::FulltextIndexSettings::KEYWORD), Context());
        const auto keywordDecoded = MustDecode(keywordEmpty[0]->GetData());
        UNIT_ASSERT_VALUES_EQUAL(keywordDecoded.Postings.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(keywordDecoded.Postings[0].Token, "");
        UNIT_ASSERT_VALUES_EQUAL(keywordDecoded.Postings[0].Rows[0], 0);
    }

    Y_UNIT_TEST(MultiChunkCoverageUsesLocalRowIds) {
        const auto meta = Meta(Ydb::Table::FulltextIndexSettings::KEYWORD);
        const auto one = MustBuild({TString("aa")}, meta, Context());
        const i64 limit = one[0]->GetData().size();
        const auto chunks = MustBuild({TString("aa"), TString("bb"), TString("cc")}, meta, Context(limit));
        UNIT_ASSERT_VALUES_EQUAL(chunks.size(), 3);
        ui32 covered = 0;
        const TVector<TString> tokens = {"aa", "bb", "cc"};
        for (size_t i = 0; i < chunks.size(); ++i) {
            const auto decoded = MustDecode(chunks[i]->GetData());
            UNIT_ASSERT_VALUES_EQUAL(decoded.ChunkRowCount, 1);
            UNIT_ASSERT_VALUES_EQUAL(decoded.Postings.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(decoded.Postings[0].Token, tokens[i]);
            UNIT_ASSERT_VALUES_EQUAL(decoded.Postings[0].Rows.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(decoded.Postings[0].Rows[0], 0);
            covered += chunks[i]->GetRecordsCountVerified();
        }
        UNIT_ASSERT_VALUES_EQUAL(covered, 3);
    }

    Y_UNIT_TEST(DefaultsOnOlderSchemasUseColumnAccessor) {
        const auto meta = Meta(Ydb::Table::FulltextIndexSettings::KEYWORD);
        const auto context = Context();
        auto withDefault = BuildFulltextIndex({}, 3, *MakeLoader(std::make_shared<arrow::StringScalar>("hello"), meta.GetColumnId()), meta, context);
        UNIT_ASSERT_C(withDefault.IsSuccess(), withDefault.IsFail() ? withDefault.GetErrorMessage() : TString());
        UNIT_ASSERT(withDefault->IsBuilt());
        const auto decodedDefault = MustDecode(withDefault->GetChunks()[0]->GetData());
        UNIT_ASSERT_VALUES_EQUAL(decodedDefault.ChunkRowCount, 3);
        UNIT_ASSERT_VALUES_EQUAL(decodedDefault.Postings.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(decodedDefault.Postings[0].Token, "hello");
        UNIT_ASSERT_VALUES_EQUAL(decodedDefault.Postings[0].Rows.size(), 3);

        auto withoutDefault = BuildFulltextIndex({}, 2, *MakeLoader(nullptr, meta.GetColumnId()), meta, context);
        UNIT_ASSERT_C(withoutDefault.IsSuccess(), withoutDefault.IsFail() ? withoutDefault.GetErrorMessage() : TString());
        const auto decodedNull = MustDecode(withoutDefault->GetChunks()[0]->GetData());
        UNIT_ASSERT_VALUES_EQUAL(decodedNull.ChunkRowCount, 2);
        UNIT_ASSERT(decodedNull.Postings.empty());

        arrow::StringBuilder builder;
        UNIT_ASSERT(builder.Append("hello").ok());
        auto array = builder.Finish().ValueOrDie();
        auto loader = MakeLoader(std::make_shared<arrow::StringScalar>("other"), meta.GetColumnId());
        auto trivial = std::make_shared<NArrow::NAccessor::TTrivialArray>(array);
        const auto blob = loader->GetAccessorConstructor().SerializeToBlobAndMeta(trivial, loader->BuildAccessorContext(1)).Blob;
        THashMap<ui32, std::vector<std::shared_ptr<IPortionDataChunk>>> data;
        data[meta.GetColumnId()] = {std::make_shared<NChunks::TPortionIndexChunk>(TChunkAddress(meta.GetColumnId(), 0), 1, blob.size(), blob)};
        auto physical = BuildFulltextIndex(data, 1, *loader, meta, context);
        UNIT_ASSERT_C(physical.IsSuccess(), physical.IsFail() ? physical.GetErrorMessage() : TString());
        const auto decodedPhysical = MustDecode(physical->GetChunks()[0]->GetData());
        UNIT_ASSERT_VALUES_EQUAL(decodedPhysical.Postings[0].Token, "hello");
    }

    Y_UNIT_TEST(OversizedOneRowSkipsTheWholeIndex) {
        const auto meta = Meta(Ydb::Table::FulltextIndexSettings::KEYWORD);
        const auto one = MustBuild({TString("a")}, meta, Context());
        const i64 limit = one[0]->GetData().size();
        const TString huge(static_cast<size_t>(limit), 'z');
        auto onlyHuge = BuildFulltextFromDocuments({huge}, meta, Context(limit));
        UNIT_ASSERT(onlyHuge.IsSuccess());
        UNIT_ASSERT(onlyHuge->IsSkipped());
        UNIT_ASSERT(onlyHuge->GetSkipReason().Contains("single row"));
        UNIT_ASSERT(onlyHuge->GetChunks().empty());

        auto prefixThenHuge = BuildFulltextFromDocuments({TString("a"), huge}, meta, Context(limit));
        UNIT_ASSERT(prefixThenHuge.IsSuccess());
        UNIT_ASSERT(prefixThenHuge->IsSkipped());
        UNIT_ASSERT(prefixThenHuge->GetChunks().empty());
    }

    Y_UNIT_TEST(ConstructionMemoryKeepsClosedChunks) {
        const auto meta = Meta(Ydb::Table::FulltextIndexSettings::KEYWORD);
        const auto one = MustBuild({TString("aa")}, meta, Context());
        const i64 limit = one[0]->GetData().size();
        const ui64 chunkBytes = one[0]->GetData().size();
        const auto wide = MustBuild({TString("aa"), TString("bb")}, meta, Context(limit, 8ull << 20));
        UNIT_ASSERT_VALUES_EQUAL(wide.size(), 2);
        auto tight = BuildFulltextFromDocuments({TString("aa"), TString("bb")}, meta, Context(limit, chunkBytes * 2));
        UNIT_ASSERT(tight.IsSuccess());
        UNIT_ASSERT(tight->IsSkipped());
        UNIT_ASSERT(tight->GetSkipReason().Contains("memory"));
        UNIT_ASSERT(tight->GetChunks().empty());
    }

    Y_UNIT_TEST(BoundedAnalysisDoesNotTruncateTokens) {
        auto context = Context();
        context.AnalyzerMaxGeneratedTokens = 1;
        auto skipped = BuildFulltextFromDocuments({TString("a b")}, Meta(Ydb::Table::FulltextIndexSettings::WHITESPACE), context);
        UNIT_ASSERT(skipped.IsSuccess());
        UNIT_ASSERT(skipped->IsSkipped());
        UNIT_ASSERT(skipped->GetSkipReason().Contains("token"));
        UNIT_ASSERT(skipped->GetChunks().empty());

        context.AnalyzerMaxGeneratedTokens = 100000;
        context.AnalyzerMaxInputBytes = 3;
        auto inputSkipped = BuildFulltextFromDocuments({TString("abcd")}, Meta(Ydb::Table::FulltextIndexSettings::KEYWORD), context);
        UNIT_ASSERT(inputSkipped->IsSkipped());
        UNIT_ASSERT(inputSkipped->GetSkipReason().Contains("input"));

        context.AnalyzerMaxInputBytes = 1ull << 20;
        context.AnalyzerMaxRetainedBytes = 1;
        auto retainedSkipped = BuildFulltextFromDocuments({TString("abcd")}, Meta(Ydb::Table::FulltextIndexSettings::KEYWORD), context);
        UNIT_ASSERT(retainedSkipped->IsSkipped());
        UNIT_ASSERT(retainedSkipped->GetSkipReason().Contains("retained"));

        Ydb::Table::FulltextIndexSettings::Analyzers analyzers;
        analyzers.set_tokenizer(Ydb::Table::FulltextIndexSettings::KEYWORD);
        analyzers.set_use_filter_ngram(true);
        analyzers.set_filter_ngram_min_length(1);
        analyzers.set_filter_ngram_max_length(3);
        const TFulltextIndexMeta ngramMeta(11, "ft", "default", true, 7, NormalizeAnalyzers(analyzers));
        context.AnalyzerMaxRetainedBytes = 1ull << 20;
        context.AnalyzerMaxGeneratedTokens = 2;
        auto ngramSkipped = BuildFulltextFromDocuments({TString("abcdef")}, ngramMeta, context);
        UNIT_ASSERT(ngramSkipped->IsSkipped());
        context.AnalyzerMaxGeneratedTokens = 100000;
        const auto ngramBuilt = MustBuild({TString("abcdef")}, ngramMeta, context);
        UNIT_ASSERT(MustDecode(ngramBuilt[0]->GetData()).Postings.size() > 2);

        NKikimr::NFulltext::TAnalyzeBudget budget;
        budget.MaxGeneratedTokens = 1;
        const auto analyzed = NKikimr::NFulltext::AnalyzeBounded("a b", Analyzers(Ydb::Table::FulltextIndexSettings::WHITESPACE), budget);
        UNIT_ASSERT(!analyzed.Ok());
        UNIT_ASSERT(analyzed.Tokens.empty());
    }

    Y_UNIT_TEST(CorruptColumnIsAnErrorNotASkip) {
        const auto meta = Meta(Ydb::Table::FulltextIndexSettings::KEYWORD);
        auto loader = MakeLoader(nullptr, meta.GetColumnId());
        THashMap<ui32, std::vector<std::shared_ptr<IPortionDataChunk>>> data;
        const TString junk = "this is not an arrow column";
        data[meta.GetColumnId()] = {std::make_shared<NChunks::TPortionIndexChunk>(TChunkAddress(meta.GetColumnId(), 0), 1, junk.size(), junk)};
        auto outcome = BuildFulltextIndex(data, 1, *loader, meta, Context());
        UNIT_ASSERT(outcome.IsFail());
        UNIT_ASSERT(!outcome.GetErrorMessage().empty());
    }
}

} // namespace NKikimr::NOlap::NIndexes::NFulltext
