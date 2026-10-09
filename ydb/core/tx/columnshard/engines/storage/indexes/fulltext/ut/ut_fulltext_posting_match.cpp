#include <ydb/core/formats/arrow/filter/filter.h>
#include <ydb/core/tx/columnshard/engines/storage/indexes/fulltext/format.h>
#include <ydb/core/tx/columnshard/engines/storage/indexes/fulltext/posting_match.h>

#include <library/cpp/testing/unittest/registar.h>

namespace NKikimr::NOlap::NIndexes::NFulltext {
namespace {

using NKikimr::NFulltext::CompileFulltextQuery;
using NKikimr::NFulltext::EFulltextQueryChecks;
using NKikimr::NFulltext::NormalizeAnalyzers;
using NKikimr::NFulltext::TCompiledFulltextQuery;
using NKikimr::NFulltext::TFulltextQueryOptions;

Ydb::Table::FulltextIndexSettings::Analyzers WhitespaceAnalyzers() {
    Ydb::Table::FulltextIndexSettings::Analyzers analyzers;
    analyzers.set_tokenizer(Ydb::Table::FulltextIndexSettings::WHITESPACE);
    return NormalizeAnalyzers(analyzers);
}

TCompiledFulltextQuery MustCompile(const TString& text, const TString& defaultOperator = "") {
    TFulltextQueryOptions options;
    options.Checks = EFulltextQueryChecks::Column;
    options.DefaultOperator = defaultOperator;
    const auto compiled = CompileFulltextQuery(text, WhitespaceAnalyzers(), options);
    UNIT_ASSERT_C(compiled, compiled.Error);
    return *compiled.Compiled;
}

TString MustEncode(TFulltextChunkPlain plain) {
    plain.ColumnId = 3;
    plain.IndexId = 9;
    plain.Analyzers = WhitespaceAnalyzers();
    auto encoded = EncodeFulltextChunk(plain);
    UNIT_ASSERT_C(!encoded.IsFail(), encoded.GetErrorMessage());
    return encoded.DetachResult();
}

TFulltextPostingResult Mask(const std::vector<TFulltextStoredChunk>& chunks, const ui32 portionRows, const TCompiledFulltextQuery& query) {
    return BuildFulltextPostingMask(chunks, portionRows, 3, 9, query);
}

std::vector<ui8> MustMask(const std::vector<TFulltextStoredChunk>& chunks, const ui32 portionRows, const TCompiledFulltextQuery& query) {
    const auto result = Mask(chunks, portionRows, query);
    UNIT_ASSERT_C(result.Status == EFulltextPostingStatus::TokenMask, result.Reason);
    return result.PortionMask;
}

void AssertBits(const std::vector<ui8>& actual, const std::vector<ui8>& expected) {
    UNIT_ASSERT_EQUAL(actual.size(), expected.size());
    for (size_t i = 0; i < actual.size(); ++i) {
        UNIT_ASSERT_EQUAL(ui32(actual[i]), ui32(expected[i]));
    }
}

}

Y_UNIT_TEST_SUITE(FulltextPostingMask) {

Y_UNIT_TEST(MissingChunksFallBackInsteadOfMatchingNothing) {
    const auto result = Mask({}, 4, MustCompile("a"));
    UNIT_ASSERT_EQUAL(result.Status, EFulltextPostingStatus::NeedTextFallback);
    UNIT_ASSERT(result.PortionMask.empty());
}

Y_UNIT_TEST(AbsentTokenIsAnEmptyPostingList) {
    const TString bytes = MustEncode([] {
        TFulltextChunkPlain plain;
        plain.ChunkRowCount = 4;
        plain.Postings.push_back(TFulltextPostingList{"a", {0, 1}});
        plain.Postings.push_back(TFulltextPostingList{"b", {0, 2}});
        return plain;
    }());
    const std::vector<TFulltextStoredChunk> chunks{{bytes, 4}};

    AssertBits(MustMask(chunks, 4, MustCompile("a b")), {1, 0, 0, 0});
    AssertBits(MustMask(chunks, 4, MustCompile("a z")), {0, 0, 0, 0});
    AssertBits(MustMask(chunks, 4, MustCompile("a z", "or")), {1, 1, 0, 0});
    AssertBits(MustMask(chunks, 4, MustCompile("z", "or")), {0, 0, 0, 0});
    AssertBits(MustMask(chunks, 4, MustCompile("+b", "or")), {1, 0, 1, 0});
}

Y_UNIT_TEST(ChunkLocalIdsAddPrecedingRowCounts) {
    const TString first = MustEncode([] {
        TFulltextChunkPlain plain;
        plain.ChunkRowCount = 2;
        plain.Postings.push_back(TFulltextPostingList{"a", {1}});
        return plain;
    }());
    const TString second = MustEncode([] {
        TFulltextChunkPlain plain;
        plain.ChunkRowCount = 2;
        plain.Postings.push_back(TFulltextPostingList{"a", {0}});
        return plain;
    }());
    const std::vector<TFulltextStoredChunk> chunks{{first, 2}, {second, 2}};
    AssertBits(MustMask(chunks, 4, MustCompile("a")), {0, 1, 1, 0});
}

Y_UNIT_TEST(PartialCoverageIsAnError) {
    const TString bytes = MustEncode([] {
        TFulltextChunkPlain plain;
        plain.ChunkRowCount = 2;
        plain.Postings.push_back(TFulltextPostingList{"a", {0}});
        return plain;
    }());
    const auto result = Mask({{bytes, 2}}, 4, MustCompile("a"));
    UNIT_ASSERT_EQUAL(result.Status, EFulltextPostingStatus::Error);
    UNIT_ASSERT_STRING_CONTAINS(result.Reason, "partial or gapped");
}

Y_UNIT_TEST(UnsupportedVersionFallsBackAndCorruptBytesFail) {
    TString bytes = MustEncode([] {
        TFulltextChunkPlain plain;
        plain.ChunkRowCount = 1;
        plain.Postings.push_back(TFulltextPostingList{"a", {0}});
        return plain;
    }());
    bytes[4] = 2;
    const auto unsupported = Mask({{bytes, 1}}, 1, MustCompile("a"));
    UNIT_ASSERT_EQUAL(unsupported.Status, EFulltextPostingStatus::NeedTextFallback);

    bytes[4] = 1;
    bytes[0] = 'X';
    const auto corrupt = Mask({{bytes, 1}}, 1, MustCompile("a"));
    UNIT_ASSERT_EQUAL(corrupt.Status, EFulltextPostingStatus::Error);
}

Y_UNIT_TEST(IncompatibleAnalyzerFallsBack) {
    const TString bytes = MustEncode([] {
        TFulltextChunkPlain plain;
        plain.ChunkRowCount = 1;
        plain.Postings.push_back(TFulltextPostingList{"a", {0}});
        return plain;
    }());
    Ydb::Table::FulltextIndexSettings::Analyzers other;
    other.set_tokenizer(Ydb::Table::FulltextIndexSettings::STANDARD);
    other = NormalizeAnalyzers(other);
    TFulltextQueryOptions options;
    options.Checks = EFulltextQueryChecks::Column;
    const auto compiled = CompileFulltextQuery("a", other, options);
    UNIT_ASSERT_C(compiled, compiled.Error);
    const auto result = BuildFulltextPostingMask({TFulltextStoredChunk{bytes, 1}}, 1, 3, 9, *compiled.Compiled);
    UNIT_ASSERT_EQUAL(result.Status, EFulltextPostingStatus::NeedTextFallback);
}

Y_UNIT_TEST(FilteredPositionIsNotThePostingId) {
    const std::vector<ui8> portionMask{1, 0, 1, 0};
    NArrow::TColumnFilter shrinking = NArrow::TColumnFilter::BuildAllowFilter();
    shrinking.Add(false);
    shrinking.Add(true);
    shrinking.Add(true);
    shrinking.Add(false);
    auto program = MapFulltextMaskToProgramRows(portionMask, &shrinking, 2);
    UNIT_ASSERT_C(!program.IsFail(), program.GetErrorMessage());
    AssertBits(program.GetResult(), {0, 1});
}

}

}
