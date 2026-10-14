#include <ydb/core/kqp/runtime/kqp_column_fulltext_write.h>
#include <ydb/library/testlib/helpers.h>

#include <library/cpp/testing/unittest/registar.h>

namespace NKikimr::NKqp {
namespace {

using namespace NTableIndex::NFulltext;

TColumnFulltextMaintainer MakeMaintainer(const bool relevance = false, const bool seed = false, const bool ngram = false) {
    TColumnFulltextMaintainer result;
    result.SetLayout({"Id", "Message"}, {}, {"Id"});
    TColumnFulltextMaintainer::TIndex index;
    index.PostingId = TPathId(1, 2);
    index.StateId = TPathId(1, 3);
    index.DocsId = TPathId(1, 4);
    index.StatsId = TPathId(1, 5);
    index.DocIdType = NScheme::TTypeInfo(NScheme::NTypeIds::Uint64);
    index.Text = {"Message", NScheme::TTypeInfo(NScheme::NTypeIds::Utf8), false};
    index.Relevance = relevance;
    index.SeedOnly = seed;
    index.Ready = !seed;
    index.StateColumns = {
        {"Id", index.DocIdType, true},
        {TokensColumn, NScheme::TTypeInfo(NScheme::NTypeIds::String), true},
        {DocLengthColumn, NScheme::TTypeInfo(NScheme::NTypeIds::Uint32), true},
    };
    auto* column = index.Settings.add_columns();
    column->set_column("Message");
    auto* analyzers = column->mutable_analyzers();
    analyzers->set_tokenizer(Ydb::Table::FulltextIndexSettings::WHITESPACE);
    if (ngram) {
        analyzers->set_use_filter_ngram(true);
        analyzers->set_filter_ngram_min_length(2);
        analyzers->set_filter_ngram_max_length(32);
    }
    result.AddIndex(std::move(index));
    return result;
}

TString PrepareText(TColumnFulltextMaintainer& maintainer, const TString& text, const bool seed = false) {
    const TVector<TCell> row = {TCell::Make(ui64(1)), TCell(text.data(), text.size())};
    const std::vector<TColumnFulltextMaintainer::TInputRow> rows = {{row, false, true}};
    if (seed) {
        return maintainer.PrepareSeed(rows);
    }
    maintainer.SetRows(NKikimrKqp::TKqpTableSinkSettings::MODE_UPSERT, rows);
    return maintainer.Prepare();
}

} // namespace

Y_UNIT_TEST_SUITE(KqpColumnFulltextMaintainer) {
    Y_UNIT_TEST_TWIN(OversizedInputRejectedBeforeTokenization, seed) {
        auto maintainer = MakeMaintainer(false, seed);
        // Whitespace generates no retained tokens, but must still be bounded.
        const TString text(ColumnFulltextAnalyzerMaxInputBytes + 1, ' ');
        UNIT_ASSERT_STRING_CONTAINS(PrepareText(maintainer, text, seed), "input exceeds the byte limit");
        // A subsequent prepare must discard the failed attempt completely.
        UNIT_ASSERT_VALUES_EQUAL(PrepareText(maintainer, "alpha", seed), TString());
    }

    Y_UNIT_TEST_TWIN(NgramExpansionRejectedBeforeBuildingState, seed) {
        auto maintainer = MakeMaintainer(false, seed, true);
        const TString text(10000, 'a');
        UNIT_ASSERT_STRING_CONTAINS(PrepareText(maintainer, text, seed), "generated token limit");
        UNIT_ASSERT_VALUES_EQUAL(PrepareText(maintainer, "alpha", seed), TString());
    }

    Y_UNIT_TEST_TWIN(TokenDeduplicationPreservesStateAndLength, relevance) {
        auto maintainer = MakeMaintainer(relevance);
        UNIT_ASSERT_VALUES_EQUAL(PrepareText(maintainer, "beta alpha beta alpha beta"), TString());
        for (const auto& need : maintainer.Sequences()) {
            maintainer.AssignSequences(need.PathId, std::vector<ui64>(need.Count, 1));
        }
        std::vector<TColumnFulltextMaintainer::TWriteBatch> batches;
        TColumnFulltextMaintainer::TStats stats;
        const auto error = maintainer.Build(nullptr, batches, stats);
        UNIT_ASSERT_C(error.empty(), error);
        bool foundState = false;
        for (const auto& batch : batches) {
            if (batch.PathId != TPathId(1, 3)) {
                continue;
            }
            foundState = true;
            const auto rows = GetRows(batch.Batch);
            UNIT_ASSERT_VALUES_EQUAL(rows.size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(rows[0][2].AsValue<ui32>(), 5);
            TVector<NFulltext::TDocumentStateToken> tokens;
            TString decodeError;
            UNIT_ASSERT_C(NFulltext::DecodeDocumentState(rows[0][1].AsBuf(), relevance, tokens, decodeError), decodeError);
            UNIT_ASSERT_VALUES_EQUAL(tokens.size(), 2);
            UNIT_ASSERT_VALUES_EQUAL(tokens[0].Token, "alpha");
            UNIT_ASSERT_VALUES_EQUAL(tokens[0].Freq, relevance ? 2 : 1);
            UNIT_ASSERT_VALUES_EQUAL(tokens[1].Token, "beta");
            UNIT_ASSERT_VALUES_EQUAL(tokens[1].Freq, relevance ? 3 : 1);
        }
        UNIT_ASSERT(foundState);
    }
}

} // namespace NKikimr::NKqp
