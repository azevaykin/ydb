#include <ydb/core/formats/arrow/filter/filter.h>
#include <ydb/core/tx/columnshard/engines/storage/indexes/fulltext/format.h>
#include <ydb/core/tx/columnshard/engines/storage/indexes/fulltext/posting_match.h>

#include <library/cpp/testing/common/env.h>
#include <library/cpp/testing/unittest/registar.h>
#include <util/stream/file.h>
#include <util/string/builder.h>

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

}   // namespace

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
            plain.Postings.push_back(TFulltextPostingList{ "a", { 0, 1 } });
            plain.Postings.push_back(TFulltextPostingList{ "b", { 0, 2 } });
            return plain;
        }());
        const std::vector<TFulltextStoredChunk> chunks{ { bytes, 4 } };

        AssertBits(MustMask(chunks, 4, MustCompile("a b")), { 1, 0, 0, 0 });
        AssertBits(MustMask(chunks, 4, MustCompile("a z")), { 0, 0, 0, 0 });
        AssertBits(MustMask(chunks, 4, MustCompile("a z", "or")), { 1, 1, 0, 0 });
        AssertBits(MustMask(chunks, 4, MustCompile("z", "or")), { 0, 0, 0, 0 });
        AssertBits(MustMask(chunks, 4, MustCompile("+b", "or")), { 1, 0, 1, 0 });
    }

    Y_UNIT_TEST(ChunkLocalIdsAddPrecedingRowCounts) {
        const TString first = MustEncode([] {
            TFulltextChunkPlain plain;
            plain.ChunkRowCount = 2;
            plain.Postings.push_back(TFulltextPostingList{ "a", { 1 } });
            return plain;
        }());
        const TString second = MustEncode([] {
            TFulltextChunkPlain plain;
            plain.ChunkRowCount = 2;
            plain.Postings.push_back(TFulltextPostingList{ "a", { 0 } });
            return plain;
        }());
        const std::vector<TFulltextStoredChunk> chunks{ { first, 2 }, { second, 2 } };
        AssertBits(MustMask(chunks, 4, MustCompile("a")), { 0, 1, 1, 0 });
    }

    Y_UNIT_TEST(PartialCoverageIsAnError) {
        const TString bytes = MustEncode([] {
            TFulltextChunkPlain plain;
            plain.ChunkRowCount = 2;
            plain.Postings.push_back(TFulltextPostingList{ "a", { 0 } });
            return plain;
        }());
        const auto result = Mask({ { bytes, 2 } }, 4, MustCompile("a"));
        UNIT_ASSERT_EQUAL(result.Status, EFulltextPostingStatus::Error);
        UNIT_ASSERT_STRING_CONTAINS(result.Reason, "partial or gapped");
    }

    Y_UNIT_TEST(UnsupportedVersionFallsBackAndCorruptBytesFail) {
        TString bytes = MustEncode([] {
            TFulltextChunkPlain plain;
            plain.ChunkRowCount = 1;
            plain.Postings.push_back(TFulltextPostingList{ "a", { 0 } });
            return plain;
        }());
        bytes[4] = FulltextChunkFormatVersion + 1;
        const auto unsupported = Mask({ { bytes, 1 } }, 1, MustCompile("a"));
        UNIT_ASSERT_EQUAL(unsupported.Status, EFulltextPostingStatus::NeedTextFallback);

        bytes[4] = FulltextChunkFormatVersion;
        bytes[0] = 'X';
        const auto corrupt = Mask({ { bytes, 1 } }, 1, MustCompile("a"));
        UNIT_ASSERT_EQUAL(corrupt.Status, EFulltextPostingStatus::Error);
    }

    Y_UNIT_TEST(MixedFormatVersionsUseTheSameRowDomain) {
        const TString root = ArcadiaSourceRoot() + "/ydb/core/tx/columnshard/engines/storage/indexes/fulltext/ut/";
        const TString legacy = TFileInput(root + "format_v1.bin").ReadAll();
        const TString current = TFileInput(root + "format_v2.bin").ReadAll();
        auto analyzers = WhitespaceAnalyzers();
        analyzers.set_tokenizer(Ydb::Table::FulltextIndexSettings::KEYWORD);
        const auto query = CompileFulltextQuery("a", analyzers);
        UNIT_ASSERT(query);
        const auto result = BuildFulltextPostingMask({ { legacy, 4 }, { current, 4 } }, 8, 7, 11, *query.Compiled);
        UNIT_ASSERT_C(result.Status == EFulltextPostingStatus::TokenMask, result.Reason);
        AssertBits(result.PortionMask, { 0, 1, 1, 0, 0, 1, 1, 0 });

        auto upgradedQuery = *query.Compiled;
        ++upgradedQuery.AnalyzerSemanticRevision;
        const auto incompatible = BuildFulltextPostingMask({ { legacy, 4 } }, 4, 7, 11, upgradedQuery);
        UNIT_ASSERT_EQUAL(incompatible.Status, EFulltextPostingStatus::NeedTextFallback);
    }

    Y_UNIT_TEST(SemanticRevisionFallbackStillValidatesPostings) {
        TFulltextChunkPlain plain;
        plain.ChunkRowCount = 2;
        plain.Postings.push_back({ "a", { 0, 1 } });
        TString bytes = MustEncode(plain);
        // The v2 semantic revision occupies bytes [20, 28).
        bytes[20] = NKikimr::NFulltext::FulltextAnalyzerSemanticRevision + 1;
        const auto result = Mask({ { bytes, 2 } }, 2, MustCompile("a"));
        UNIT_ASSERT_EQUAL(result.Status, EFulltextPostingStatus::NeedTextFallback);
        UNIT_ASSERT_STRING_CONTAINS(result.Reason, "semantic revision");
        UNIT_ASSERT(result.PortionMask.empty());

        bytes.back() = 0;   // Duplicate row in an unqueried posting must still fail.
        UNIT_ASSERT_EQUAL(Mask({ { bytes, 2 } }, 2, MustCompile("absent")).Status, EFulltextPostingStatus::Error);
    }

    Y_UNIT_TEST(IncompatibleAnalyzerFallsBack) {
        const TString bytes = MustEncode([] {
            TFulltextChunkPlain plain;
            plain.ChunkRowCount = 1;
            plain.Postings.push_back(TFulltextPostingList{ "a", { 0 } });
            return plain;
        }());
        Ydb::Table::FulltextIndexSettings::Analyzers other;
        other.set_tokenizer(Ydb::Table::FulltextIndexSettings::STANDARD);
        other = NormalizeAnalyzers(other);
        TFulltextQueryOptions options;
        options.Checks = EFulltextQueryChecks::Column;
        const auto compiled = CompileFulltextQuery("a", other, options);
        UNIT_ASSERT_C(compiled, compiled.Error);
        const auto result = BuildFulltextPostingMask({ TFulltextStoredChunk{ bytes, 1 } }, 1, 3, 9, *compiled.Compiled);
        UNIT_ASSERT_EQUAL(result.Status, EFulltextPostingStatus::NeedTextFallback);
    }

    Y_UNIT_TEST(PostingCountsAgreeWithTextAcrossQuerySizes) {
        for (const ui32 terms : { 1, 16, 17, 64, 65, 129 }) {
            TFulltextChunkPlain plain;
            plain.ChunkRowCount = 19;
            std::vector<TString> documents(plain.ChunkRowCount);
            TString queryText;
            for (ui32 term = 0; term < terms; ++term) {
                const TString token = TStringBuilder() << "term" << term;
                queryText += (term % 5 == 0 ? " +" : " ") + token;
                TFulltextPostingList posting{ token, {} };
                for (ui32 row = 0; row < plain.ChunkRowCount; ++row) {
                    if (row == 0 || (row + term * 7) % 4 != 0) {
                        documents[row] += " " + token;
                        posting.Rows.push_back(row);
                    }
                }
                plain.Postings.push_back(std::move(posting));
            }
            const TString bytes = MustEncode(std::move(plain));
            for (const TString& op : { TString("and"), TString("or") }) {
                for (const TString& threshold : { TString(), TString("1"), TString("50%"), TString("-1"), TString("100%") }) {
                    if (op == "and" && threshold) {
                        continue;
                    }
                    TFulltextQueryOptions options;
                    options.DefaultOperator = op;
                    options.MinimumShouldMatch = threshold;
                    const auto compiled = CompileFulltextQuery(queryText, WhitespaceAnalyzers(), options);
                    UNIT_ASSERT_C(compiled, compiled.Error);
                    const auto actual = MustMask({ { bytes, static_cast<ui32>(documents.size()) } }, documents.size(), *compiled.Compiled);
                    for (ui32 row = 0; row < documents.size(); ++row) {
                        UNIT_ASSERT_VALUES_EQUAL_C(actual[row] != 0,
                            NKikimr::NFulltext::EvaluateFulltextText(*compiled.Compiled, TStringBuf(documents[row])),
                            TStringBuilder() << "terms=" << terms << " row=" << row << " op=" << op << " threshold=" << threshold);
                    }
                }
            }
        }
    }

    Y_UNIT_TEST(FallbackDoesNotHideInvalidCoverageOrCorruption) {
        TFulltextChunkPlain plain;
        plain.ChunkRowCount = 2;
        plain.Postings.push_back({ "a", { 0, 1 } });
        TString future = MustEncode(plain);
        future[4] = FulltextChunkFormatVersion + 1;
        UNIT_ASSERT_EQUAL(Mask({ { future, 2 } }, 3, MustCompile("a")).Status, EFulltextPostingStatus::Error);
        UNIT_ASSERT_EQUAL(Mask({ { future, 0 } }, 0, MustCompile("a")).Status, EFulltextPostingStatus::Error);

        TString corrupt = MustEncode(plain);
        corrupt.back() = 0;   // Duplicate row in a token not present in the query.
        UNIT_ASSERT_EQUAL(Mask({ { corrupt, 2 } }, 2, MustCompile("absent")).Status, EFulltextPostingStatus::Error);
        UNIT_ASSERT_EQUAL(Mask({ { future, 2 }, { corrupt, 2 } }, 4, MustCompile("a")).Status, EFulltextPostingStatus::Error);

        auto other = WhitespaceAnalyzers();
        other.set_use_filter_lowercase(true);
        const auto query = CompileFulltextQuery("a", other);
        UNIT_ASSERT(query);
        const TString valid = MustEncode(plain);
        // Matching total coverage does not make an incorrect per-chunk row count valid.
        UNIT_ASSERT_EQUAL(Mask({ { valid, 3 } }, 3, *query.Compiled).Status, EFulltextPostingStatus::Error);
    }

    Y_UNIT_TEST(FilteredPositionIsNotThePostingId) {
        const std::vector<ui8> portionMask{ 1, 0, 1, 0 };
        NArrow::TColumnFilter shrinking = NArrow::TColumnFilter::BuildAllowFilter();
        shrinking.Add(false);
        shrinking.Add(true);
        shrinking.Add(true);
        shrinking.Add(false);
        auto program = MapFulltextMaskToProgramRows(portionMask, &shrinking, 2);
        UNIT_ASSERT_C(!program.IsFail(), program.GetErrorMessage());
        AssertBits(program.GetResult(), { 0, 1 });
    }
}

}   // namespace NKikimr::NOlap::NIndexes::NFulltext
