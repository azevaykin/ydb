#include "fulltext.h"
#include "fulltext_query.h"
#include "superlemmer.h"
#include "table_index.h"

#include <util/digest/city.h>

#include <library/cpp/json/json_reader.h>
#include <library/cpp/resource/resource.h>
#include <library/cpp/testing/unittest/registar.h>
#include <util/generic/xrange.h>

namespace NKikimr::NFulltext {

    namespace {

        struct TDeltaItem {
            ui64 DocId;
            ui32 Freq;
        };

        struct TGeneration {
            bool Added;
            TVector<TDeltaItem> Items;
        };

        TVector<ui8> Encode(const TVector<TDeltaItem>& items, bool withFreq = true) {
            TDeltaWriter writer;
            writer.Reset(withFreq, false);
            for (const auto& item : items) {
                writer.Add(item.DocId, item.Freq);
            }
            return TVector<ui8>(writer.GetBuf().begin(), writer.GetBuf().end());
        }

        TVector<TDeltaItem> Merge(const TVector<TGeneration>& generations, bool withFreq = true) {
            TVector<TVector<ui8>> encoded;
            encoded.reserve(generations.size());

            TMultiDeltaReader reader;
            reader.Reset(withFreq, false);
            for (const auto& generation : generations) {
                encoded.push_back(Encode(generation.Items, withFreq));
                reader.Add(generation.Added, encoded.back());
            }
            reader.Start();

            TVector<TDeltaItem> result;
            ui64 docId = 0;
            ui32 freq = 0;
            while (reader.Read(docId, freq)) {
                result.push_back({docId, freq});
            }
            return result;
        }

        TVector<TDeltaItem> RoundTrip(const TVector<TDeltaItem>& items, bool withFreq, bool sign = false) {
            TDeltaWriter writer;
            writer.Reset(withFreq, sign);
            for (const auto& item : items) {
                writer.Add(item.DocId, item.Freq);
            }

            UNIT_ASSERT_VALUES_EQUAL(writer.GetCount(), items.size());
            UNIT_ASSERT_VALUES_EQUAL(writer.GetMaxId(), items.empty() ? 0 : items.back().DocId);

            TDeltaReader reader(writer.GetBuf(), withFreq, sign);
            TVector<TDeltaItem> result;
            ui64 docId = 0;
            ui32 freq = 0;
            while (reader.Read(docId, freq)) {
                result.push_back({docId, freq});
            }
            return result;
        }

        void AssertDeltaItemsEqual(const TVector<TDeltaItem>& actual, const TVector<TDeltaItem>& expected) {
            UNIT_ASSERT_VALUES_EQUAL(actual.size(), expected.size());
            for (size_t i = 0; i < expected.size(); ++i) {
                UNIT_ASSERT_VALUES_EQUAL_C(actual[i].DocId, expected[i].DocId, "item " << i);
                UNIT_ASSERT_VALUES_EQUAL_C(actual[i].Freq, expected[i].Freq, "item " << i);
            }
        }

        struct TSuperLemmerCallState {
            TString Languages;
            ui32 Calls = 0;
        } SuperLemmerCallState;

        bool IsTestSuperLemmerLanguageSupported(const TString& language) {
            return language == "english" || language == "russian";
        }

        void ApplyTestSuperLemmer(const TString& languages, TString&) {
            SuperLemmerCallState.Languages = languages;
            ++SuperLemmerCallState.Calls;
        }

        Ydb::Table::FulltextIndexSettings::Analyzers StandardAnalyzers(bool lowercase = false) {
            Ydb::Table::FulltextIndexSettings::Analyzers analyzers;
            analyzers.set_tokenizer(Ydb::Table::FulltextIndexSettings::STANDARD);
            if (lowercase) {
                analyzers.set_use_filter_lowercase(true);
            }
            return analyzers;
        }

        Ydb::Table::FulltextIndexSettings::Analyzers KeywordAnalyzers() {
            Ydb::Table::FulltextIndexSettings::Analyzers analyzers;
            analyzers.set_tokenizer(Ydb::Table::FulltextIndexSettings::KEYWORD);
            return analyzers;
        }

        Ydb::Table::FulltextIndexSettings::Analyzers NgramAnalyzers(i32 minLength, i32 maxLength, bool edge = false) {
            Ydb::Table::FulltextIndexSettings::Analyzers analyzers;
            analyzers.set_tokenizer(Ydb::Table::FulltextIndexSettings::WHITESPACE);
            analyzers.set_filter_ngram_min_length(minLength);
            analyzers.set_filter_ngram_max_length(maxLength);
            if (edge) {
                analyzers.set_use_filter_edge_ngram(true);
            } else {
                analyzers.set_use_filter_ngram(true);
            }
            return analyzers;
        }

        TFulltextQueryOptions Options(TString defaultOperator = {}, TString minimumShouldMatch = {}, TString mode = {}) {
            TFulltextQueryOptions options;
            options.DefaultOperator = std::move(defaultOperator);
            options.MinimumShouldMatch = std::move(minimumShouldMatch);
            options.Mode = std::move(mode);
            return options;
        }

        TCompiledFulltextQuery MustCompile(
            const TString& query,
            const Ydb::Table::FulltextIndexSettings::Analyzers& analyzers,
            TFulltextQueryOptions options = {})
        {
            const auto validation = CompileFulltextQuery(query, analyzers, options);
            UNIT_ASSERT_C(validation, validation.Error);
            return *validation.Compiled;
        }

        THashSet<TString> TokenSet(const TCompiledFulltextQuery& query, TStringBuf text) {
            THashSet<TString> tokens;
            for (const auto& token : TokenizeFulltextDocument(query, text)) {
                tokens.insert(token);
            }
            return tokens;
        }

    } // anonymous namespace

    Y_UNIT_TEST_SUITE(NFulltext) {

        // The compact rowid-mode doc-id layout: __ydb_row_id carries a dense seq in its low bits and a
        // bit-reversed spread bucket in its high bits. RowIdFromSeq must be a bijection (SeqFromRowId is its
        // left inverse) and consecutive seq values must spread across distinct high-bit buckets.
        Y_UNIT_TEST(RowIdSeqRoundTrip) {
            using namespace NKikimr::NTableIndex::NFulltext;

            // Round-trip across a range, around the bucket-cycle boundary, and for large/high-bit seq values.
            for (ui64 seq : xrange<ui64>(0, 5000)) {
                UNIT_ASSERT_VALUES_EQUAL(SeqFromRowId(RowIdFromSeq(seq)), seq);
            }
            for (ui64 seq : {ui64(0), ui64(1), ui64((1ull << RowIdSpreadBits) - 1), ui64(1ull << RowIdSpreadBits),
                             ui64(123456789), RowIdSeqMask - 1, RowIdSeqMask}) {
                ui64 rowId = RowIdFromSeq(seq);
                UNIT_ASSERT_VALUES_EQUAL_C(SeqFromRowId(rowId), seq, "seq=" << seq << " rowId=" << rowId);
                // seq lives strictly in the low (64 - RowIdSpreadBits) bits.
                UNIT_ASSERT_VALUES_EQUAL(seq & ~RowIdSeqMask, 0u);
            }

            // Injectivity + spread: a run of consecutive seq must map to distinct row ids, and the high-bit
            // bucket must take many different values (not a monotonic tail) over a full bucket cycle.
            THashSet<ui64> rowIds;
            THashSet<ui64> buckets;
            const ui64 cycle = 1ull << RowIdSpreadBits;
            for (ui64 seq : xrange<ui64>(0, cycle)) {
                ui64 rowId = RowIdFromSeq(seq);
                UNIT_ASSERT(rowIds.insert(rowId).second);
                buckets.insert(rowId >> (64 - RowIdSpreadBits));
            }
            // bit-reversal of the low RowIdSpreadBits is itself a bijection over [0, cycle), so every bucket appears.
            UNIT_ASSERT_VALUES_EQUAL(buckets.size(), cycle);
        }

        Y_UNIT_TEST(MultiDeltaReader1) {
            TDeltaWriter wr;
            wr.Reset(false, false);
            for (ui64 i = 1; i <= 100; i++) {
                wr.Add(i, 1);
            }
            TDeltaWriter wr2;
            for (ui64 i = 5; i <= 25; i += 2) {
                wr2.Add(i, 1);
            }
            TMultiDeltaReader rdr;
            rdr.Reset(false, false);
            rdr.Add(true, wr.GetBuf());
            rdr.Add(false, wr2.GetBuf());
            rdr.Start();
            ui64 docId;
            ui32 freq;
            for (ui64 i = 1; i <= 100; i++) {
                if (i >= 5 && i <= 25 && !((i - 5) % 2)) {
                    continue;
                }
                UNIT_ASSERT(rdr.Read(docId, freq));
                UNIT_ASSERT_VALUES_EQUAL(docId, i);
                UNIT_ASSERT_VALUES_EQUAL(freq, 1);
            }
            UNIT_ASSERT(!rdr.Read(docId, freq));
        }

        Y_UNIT_TEST(DeltaCodecEmptyAndSingle) {
            AssertDeltaItemsEqual(RoundTrip({}, false), {});
            AssertDeltaItemsEqual(RoundTrip({{0, 1}}, false), {{0, 1}});
            AssertDeltaItemsEqual(RoundTrip({{Max<ui64>(), 1}}, false), {{Max<ui64>(), 1}});

            // Frequency one is implicit in relevance segments, while larger values are stored explicitly.
            AssertDeltaItemsEqual(RoundTrip({{0, 1}}, true), {{0, 1}});
            AssertDeltaItemsEqual(RoundTrip({{Max<ui64>(), Max<ui32>()}}, true),
                                  {{Max<ui64>(), Max<ui32>()}});
        }

        Y_UNIT_TEST(DeltaCodecVarintBoundaries) {
            // Exercise one-byte/multi-byte transitions for regular delta varints (7 payload bits).
            const TVector<TDeltaItem> plain = {
                {0, 1},
                {127, 1},
                {255, 1},   // delta 128
                {16638, 1}, // delta 16383
                {33022, 1}, // delta 16384
                {Max<ui64>(), 1},
            };
            AssertDeltaItemsEqual(RoundTrip(plain, false), plain);

            // Relevance doc-id varints reserve a flag bit, so their first transition is 63 -> 64.
            // Frequencies independently cross the regular 127 -> 128 transition.
            const TVector<TDeltaItem> relevance = {
                {0, 1},
                {63, 2},
                {127, 127},           // delta 64
                {8254, 128},          // delta 8127
                {16446, Max<ui32>()}, // delta 8192
                {Max<ui64>(), 1},
            };
            AssertDeltaItemsEqual(RoundTrip(relevance, true), relevance);
        }

        Y_UNIT_TEST(DeltaCodecSignedExtremes) {
            const TVector<TDeltaItem> items = {
                {static_cast<ui64>(Min<i64>()), 1},
                {static_cast<ui64>(-1), 2},
                {0, 127},
                {static_cast<ui64>(Max<i64>()), Max<ui32>()},
            };
            AssertDeltaItemsEqual(RoundTrip(items, true, true), items);
        }

        Y_UNIT_TEST(DeltaCodecManyAndRandomizedRoundTrip) {
            TVector<TDeltaItem> many;
            many.reserve(10000);
            for (ui64 i = 0; i < 10000; ++i) {
                many.push_back({i * i + i, 1 + static_cast<ui32>(i % 257)});
            }
            AssertDeltaItemsEqual(RoundTrip(many, true), many);

            // Fixed-seed xorshift64 property test. Generate strictly increasing ids with deltas spanning
            // every varint width normally encountered by compact posting segments.
            ui64 random = 0x9e3779b97f4a7c15ULL;
            auto nextRandom = [&]() {
                random ^= random << 13;
                random ^= random >> 7;
                random ^= random << 17;
                return random;
            };

            for (ui32 iteration = 0; iteration < 100; ++iteration) {
                TVector<TDeltaItem> items;
                ui64 docId = nextRandom() & 0xFFFF;
                const size_t count = 1 + nextRandom() % 500;
                items.reserve(count);
                for (size_t i = 0; i < count; ++i) {
                    const ui32 shift = nextRandom() % 28;
                    const ui64 delta = 1 + (nextRandom() & ((1ULL << shift) - 1));
                    if (Max<ui64>() - docId < delta) {
                        break;
                    }
                    docId += delta;
                    const ui32 freq = 1 + static_cast<ui32>(nextRandom() % 100000);
                    items.push_back({docId, freq});
                }
                AssertDeltaItemsEqual(RoundTrip(items, true), items);
                AssertDeltaItemsEqual(RoundTrip(items, false), [&] {
                    TVector<TDeltaItem> result = items;
                    for (auto& item : result) {
                        item.Freq = 1;
                    }
                    return result;
                }());
            }
        }

        Y_UNIT_TEST(MultiDeltaReaderGenerationMerge) {
            auto encode = [](std::initializer_list<TDeltaItem> items) {
                TDeltaWriter writer;
                writer.Reset(true, false);
                for (const auto& item : items) {
                    writer.Add(item.DocId, item.Freq);
                }
                return TVector<ui8>(writer.GetBuf().begin(), writer.GetBuf().end());
            };

            const auto base = encode({{1, 2}, {2, 1}, {4, 5}, {8, 1}});
            const auto deleted = encode({{1, 1}, {2, 1}, {4, 7}, {6, 1}});
            const auto added = encode({{1, 3}, {3, 1}, {4, 2}, {6, 2}});

            TMultiDeltaReader reader;
            reader.Reset(true, false);
            reader.Add(true, base);
            reader.Add(false, deleted);
            reader.Add(true, added);
            reader.Start();

            TVector<TDeltaItem> actual;
            ui64 docId = 0;
            ui32 freq = 0;
            while (reader.Read(docId, freq)) {
                actual.push_back({docId, freq});
            }

            // Frequencies from add generations are summed, deletes are subtracted, and non-positive
            // totals disappear. A delete of an absent id is canceled if a later generation adds it.
            const TVector<TDeltaItem> expected = {{1, 4}, {3, 1}, {6, 1}, {8, 1}};
            AssertDeltaItemsEqual(actual, expected);
        }

        Y_UNIT_TEST(MultiDeltaReaderManyGenerationsDifferentialAndIdempotent) {
            // Treat a simple posting map as the legacy/reference representation and compare it with compact
            // add/delete segments over many generations. The fixed seed makes failures exactly reproducible.
            TVector<TGeneration> generations;
            TMap<ui64, i64> reference;
            ui64 random = 0xd1b54a32d192ed03ULL;
            auto nextRandom = [&]() {
                random ^= random << 13;
                random ^= random >> 7;
                random ^= random << 17;
                return random;
            };

            for (ui32 gen = 0; gen < 250; ++gen) {
                TGeneration generation{.Added = (nextRandom() % 3) != 0};
                TMap<ui64, ui32> uniqueItems;
                for (ui32 i = 0, count = 1 + nextRandom() % 40; i < count; ++i) {
                    // Mix a dense head with a sparse tail. Repeated ids occur across generations, while
                    // every individual segment remains strictly sorted as required by TDeltaWriter.
                    const ui64 docId = (nextRandom() & 1)
                                           ? nextRandom() % 128
                                           : (nextRandom() % 128) * (1ULL << (nextRandom() % 40));
                    uniqueItems[docId] = 1 + nextRandom() % 31;
                }
                for (const auto& [docId, freq] : uniqueItems) {
                    generation.Items.push_back({docId, freq});
                    reference[docId] += generation.Added ? static_cast<i64>(freq) : -static_cast<i64>(freq);
                }
                generations.push_back(std::move(generation));
            }

            TVector<TDeltaItem> expected;
            for (const auto& [docId, freq] : reference) {
                if (freq > 0) {
                    expected.push_back({docId, static_cast<ui32>(freq)});
                }
            }
            const auto compacted = Merge(generations);
            AssertDeltaItemsEqual(compacted, expected);

            // Compacting the canonical single add segment again is byte-for-byte and logically idempotent.
            const auto compactedAgain = Merge({TGeneration{.Added = true, .Items = compacted}});
            AssertDeltaItemsEqual(compactedAgain, compacted);
            UNIT_ASSERT(Encode(compactedAgain) == Encode(compacted));
        }

        Y_UNIT_TEST(MultiDeltaReaderAddDeleteAddFrequencyExtremes) {
            const ui32 maxFreq = Max<ui32>();
            const TVector<TGeneration> generations = {
                {true, {{0, maxFreq}, {1, 7}, {64, 100}, {Max<ui64>(), 3}}},
                {false, {{0, maxFreq}, {1, 7}, {64, 40}, {Max<ui64>(), 3}}},
                {true, {{0, maxFreq}, {1, 9}, {64, 2}, {Max<ui64>(), 1}}},
                {false, {{1, 4}, {64, 62}}},
                {true, {{1, 1}, {64, 5}}},
            };
            const TVector<TDeltaItem> expected = {
                {0, maxFreq},
                {1, 6},
                {64, 5},
                {Max<ui64>(), 1},
            };
            AssertDeltaItemsEqual(Merge(generations), expected);
        }

        Y_UNIT_TEST(DeltaReaderMaxIdBoundaryDoesNotConsume) {
            const auto encoded = Encode({{5, 1}, {10, 2}, {Max<ui64>(), 3}});
            TDeltaReader reader(encoded, true, false);
            reader.SetMaxId(7);

            ui64 docId = 0;
            ui32 freq = 0;
            UNIT_ASSERT(reader.Read(docId, freq));
            UNIT_ASSERT_VALUES_EQUAL(docId, 5);
            UNIT_ASSERT_VALUES_EQUAL(freq, 1);
            UNIT_ASSERT(!reader.Read(docId, freq));

            // Read() restores its cursor when MaxId rejects an otherwise valid item.
            reader.SetMaxId(10);
            UNIT_ASSERT(reader.Read(docId, freq));
            UNIT_ASSERT_VALUES_EQUAL(docId, 10);
            UNIT_ASSERT_VALUES_EQUAL(freq, 2);
            UNIT_ASSERT(!reader.Read(docId, freq));
            reader.SetMaxId(Max<ui64>());
            UNIT_ASSERT(reader.Read(docId, freq));
            UNIT_ASSERT_VALUES_EQUAL(docId, Max<ui64>());
            UNIT_ASSERT_VALUES_EQUAL(freq, 3);
            UNIT_ASSERT(!reader.Read(docId, freq));
        }

        Y_UNIT_TEST(DeltaReaderRejectsMalformedSegments) {
            ui64 docId = 0;
            ui32 freq = 0;

            const TVector<ui8> truncated = {0x80};
            TDeltaReader truncatedReader(truncated, false, false);
            UNIT_ASSERT_EXCEPTION_CONTAINS(truncatedReader.Read(docId, freq), yexception, "truncated varint");

            const TVector<ui8> overlong(10, 0x80);
            TDeltaReader overlongReader(overlong, false, false);
            UNIT_ASSERT_EXCEPTION(overlongReader.Read(docId, freq), yexception);

            const TVector<ui8> nonCanonical = {0x80, 0x00};
            TDeltaReader nonCanonicalReader(nonCanonical, false, false);
            UNIT_ASSERT_EXCEPTION_CONTAINS(
                nonCanonicalReader.Read(docId, freq), yexception, "non-canonical varint");

            TVector<ui8> overflowingFlagged = {0x80};
            overflowingFlagged.insert(overflowingFlagged.end(), 8, 0x80);
            overflowingFlagged.push_back(0x04); // tail=2^58 cannot be shifted left by the reserved six bits
            TDeltaReader flaggedReader(overflowingFlagged, true, false);
            UNIT_ASSERT_EXCEPTION_CONTAINS(
                flaggedReader.Read(docId, freq), yexception, "overflowing flagged varint");

            const TVector<ui8> overflowingFrequency = {0x41, 0x80, 0x80, 0x80, 0x80, 0x10};
            TDeltaReader frequencyReader(overflowingFrequency, true, false);
            UNIT_ASSERT_EXCEPTION_CONTAINS(frequencyReader.Read(docId, freq), yexception, "overflowing frequency");

            TVector<ui8> overflowingDocId = Encode({{Max<ui64>(), 1}}, false);
            overflowingDocId.push_back(1);
            TDeltaReader docIdReader(overflowingDocId, false, false);
            UNIT_ASSERT(docIdReader.Read(docId, freq));
            UNIT_ASSERT_VALUES_EQUAL(docId, Max<ui64>());
            UNIT_ASSERT_EXCEPTION_CONTAINS(docIdReader.Read(docId, freq), yexception, "overflowing document id");

            const TVector<ui8> explicitZeroFrequency = {0x41, 0x00};
            TDeltaReader zeroFrequencyReader(explicitZeroFrequency, true, false);
            UNIT_ASSERT_EXCEPTION_CONTAINS(
                zeroFrequencyReader.Read(docId, freq), yexception, "non-canonical frequency");
        }

        Y_UNIT_TEST(MultiDeltaReaderRejectsFrequencyOverflow) {
            const ui32 maxFreq = Max<ui32>();
            UNIT_ASSERT_EXCEPTION_CONTAINS(
                Merge({
                    {true, {{1, maxFreq}}},
                    {true, {{1, 1}}},
                }),
                yexception,
                "exceeds ui32");
        }

        Y_UNIT_TEST(MultiDeltaReader2) {
            TDeltaReader r1(TConstArrayRef<ui8>((const ui8*)"2\x0A", 2), false, false);
            r1.SetMaxId(203);
            TMultiDeltaReader rdr;
            rdr.Reset(false, false);
            rdr.Add(true, &r1);
            rdr.Add(false, TConstArrayRef<ui8>((const ui8*)"dd", 2));
            rdr.Add(true, TConstArrayRef<ui8>((const ui8*)"\x0A\x0A\x0A", 3));
            rdr.Add(true, TConstArrayRef<ui8>((const ui8*)"dd\x01\x02", 4));
            rdr.Start();
            ui64 docId;
            ui32 freq;
            auto check = [&](ui64 expectedDoc) {
                UNIT_ASSERT(rdr.Read(docId, freq));
                Cerr << "Read: " << docId << " == " << expectedDoc << "\n";
                UNIT_ASSERT_VALUES_EQUAL(docId, expectedDoc);
                UNIT_ASSERT_VALUES_EQUAL(freq, 1);
            };
            check(10);
            check(20);
            check(30);
            check(50);
            check(60);
            check(201);
            check(203);
            UNIT_ASSERT(!rdr.Read(docId, freq));
        }

        Y_UNIT_TEST(SignedDelta) {
            TDeltaWriter wr;
            wr.Reset(false, true);
            wr.Add(-1, 1);
            UNIT_ASSERT_VALUES_EQUAL(wr.GetBuf().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(wr.GetBuf()[0], 1);
            wr.Reset(true, true);
            for (i64 i = -50; i <= 50; i++) {
                wr.Add(i, 1 + ((i + 50) % 3));
            }
            TDeltaWriter wr2;
            wr2.Reset(true, true);
            for (i64 i = -25; i <= 25; i += 2) {
                wr2.Add(i, 1 + ((i + 50) % 3));
            }
            TMultiDeltaReader rdr;
            rdr.Reset(true, true);
            rdr.Add(true, wr.GetBuf());
            rdr.Add(false, wr2.GetBuf());
            rdr.Start();
            ui64 docId;
            ui32 freq;
            for (i64 i = -50; i <= 50; i++) {
                if (i >= -25 && i <= 25 && !((i + 25) % 2)) {
                    continue;
                }
                UNIT_ASSERT(rdr.Read(docId, freq));
                Cerr << "Read: " << (i64)docId << " " << freq << "\n";
                UNIT_ASSERT_VALUES_EQUAL((i64)docId, i);
                UNIT_ASSERT_VALUES_EQUAL(freq, 1 + ((i + 50) % 3));
            }
            UNIT_ASSERT(!rdr.Read(docId, freq));
        }

        Y_UNIT_TEST(ValidateColumnsMatches) {
            TString error;

            Ydb::Table::FulltextIndexSettings settings;
            settings.add_columns()->set_column("column1");
            settings.add_columns()->set_column("column2");

            UNIT_ASSERT(!ValidateColumnsMatches(TVector<TString>{"column2"}, settings, error));
            UNIT_ASSERT_VALUES_EQUAL(error, "indexed columns [ column1 column2 ] should be the suffix of index columns [ column2 ]");

            UNIT_ASSERT(!ValidateColumnsMatches(TVector<TString>{"column2", "column1"}, settings, error));
            UNIT_ASSERT_VALUES_EQUAL(error, "indexed columns [ column1 column2 ] should be the suffix of index columns [ column2 column1 ]");

            UNIT_ASSERT(ValidateColumnsMatches(TVector<TString>{"column1", "column2"}, settings, error));
            UNIT_ASSERT_VALUES_EQUAL(error, "");

            // prefix columns are allowed before the indexed (text) suffix
            Ydb::Table::FulltextIndexSettings single;
            single.add_columns()->set_column("text");
            UNIT_ASSERT(ValidateColumnsMatches(TVector<TString>{"user_id", "text"}, single, error));
            UNIT_ASSERT_VALUES_EQUAL(error, "");
            UNIT_ASSERT(ValidateColumnsMatches(TVector<TString>{"a", "b", "text"}, single, error));
            UNIT_ASSERT(!ValidateColumnsMatches(TVector<TString>{"user_id", "other"}, single, error));
        }

        Y_UNIT_TEST(ValidateSettings) {
            Ydb::Table::FulltextIndexSettings settings;
            TString error;

            UNIT_ASSERT(!ValidateSettings(settings, error));
            UNIT_ASSERT_VALUES_EQUAL(error, "columns should be set");

            auto columnSettings = settings.add_columns();
            UNIT_ASSERT(!ValidateSettings(settings, error));
            UNIT_ASSERT_VALUES_EQUAL(error, "column name should be set");

            columnSettings->set_column("text");
            UNIT_ASSERT(!ValidateSettings(settings, error));
            UNIT_ASSERT_VALUES_EQUAL(error, "column analyzers should be set");

            auto columnAnalyzers = columnSettings->mutable_analyzers();
            UNIT_ASSERT(!ValidateSettings(settings, error));
            UNIT_ASSERT_VALUES_EQUAL(error, "tokenizer should be set");

            columnAnalyzers->set_tokenizer(Ydb::Table::FulltextIndexSettings::STANDARD);
            UNIT_ASSERT_C(ValidateSettings(settings, error), error);
            UNIT_ASSERT_VALUES_EQUAL(error, "");

            columnAnalyzers->set_use_filter_length(false);
            UNIT_ASSERT_C(ValidateSettings(settings, error), error);
            UNIT_ASSERT_VALUES_EQUAL(error, "");

            columnAnalyzers->set_use_filter_length(true);
            UNIT_ASSERT(!ValidateSettings(settings, error));
            UNIT_ASSERT_VALUES_EQUAL(error, "either filter_length_min or filter_length_max should be set with use_filter_length");

            columnAnalyzers->set_filter_length_min(5);
            UNIT_ASSERT_C(ValidateSettings(settings, error), error);
            UNIT_ASSERT_VALUES_EQUAL(error, "");

            columnAnalyzers->set_filter_length_max(6);
            UNIT_ASSERT_C(ValidateSettings(settings, error), error);
            UNIT_ASSERT_VALUES_EQUAL(error, "");

            columnAnalyzers->set_filter_length_max(3);
            UNIT_ASSERT_C(!ValidateSettings(settings, error), error);
            UNIT_ASSERT_VALUES_EQUAL(error, "Invalid filter_length_min: should be less than or equal to filter_length_max");

            columnAnalyzers->set_filter_length_min(-5);
            UNIT_ASSERT_C(!ValidateSettings(settings, error), error);
            UNIT_ASSERT_VALUES_EQUAL(error, "Invalid filter_length_min: -5 should be between 1 and 1000");

            columnAnalyzers->set_filter_length_min(3);
            columnAnalyzers->set_filter_length_max(3000);
            UNIT_ASSERT_C(!ValidateSettings(settings, error), error);
            UNIT_ASSERT_VALUES_EQUAL(error, "Invalid filter_length_max: 3000 should be between 1 and 1000");

            columnAnalyzers->set_use_filter_snowball(true);
            columnAnalyzers->clear_language();
            UNIT_ASSERT_C(!ValidateSettings(settings, error), error);
            UNIT_ASSERT_VALUES_EQUAL(error, "language required when use_filter_snowball is set");

            columnAnalyzers->set_language("klingon");
            UNIT_ASSERT_C(!ValidateSettings(settings, error), error);
            UNIT_ASSERT_VALUES_EQUAL(error, "language is not supported by snowball");

            columnAnalyzers->set_filter_length_max(6);
            columnAnalyzers->set_language("english, russian,english");
            UNIT_ASSERT_C(ValidateSettings(settings, error), error);
            UNIT_ASSERT_VALUES_EQUAL(error, "");

            columnAnalyzers->set_language("english,klingon");
            UNIT_ASSERT_C(!ValidateSettings(settings, error), error);
            UNIT_ASSERT_VALUES_EQUAL(error, "language is not supported by snowball");

            columnAnalyzers->set_language("english,,russian");
            UNIT_ASSERT_C(ValidateSettings(settings, error), error);
            UNIT_ASSERT_VALUES_EQUAL(error, "");

            columnAnalyzers->set_language("english,german");
            UNIT_ASSERT_C(!ValidateSettings(settings, error), error);
            UNIT_ASSERT_VALUES_EQUAL(error, "language is not supported by snowball");

            columnAnalyzers->set_language("armenian,english,greek,russian,tamil,yiddish");
            UNIT_ASSERT_C(ValidateSettings(settings, error), error);
            UNIT_ASSERT_VALUES_EQUAL(error, "");

            columnAnalyzers->set_language("english");
            columnAnalyzers->set_use_filter_ngram(true);
            UNIT_ASSERT_C(!ValidateSettings(settings, error), error);
            UNIT_ASSERT_VALUES_EQUAL(error, "cannot set use_filter_snowball with use_filter_ngram or use_filter_edge_ngram at the same time");

            columnSettings = settings.add_columns();
            columnSettings->set_column("text2");
            UNIT_ASSERT_C(!ValidateSettings(settings, error), error);
            UNIT_ASSERT_VALUES_EQUAL(error, "columns should have a single value");
        }

        Y_UNIT_TEST(ValidateSuperLemmerSettings) {
            const auto makeSettings = [] {
                Ydb::Table::FulltextIndexSettings settings;
                auto* column = settings.add_columns();
                column->set_column("text");
                auto* analyzers = column->mutable_analyzers();
                analyzers->set_tokenizer(Ydb::Table::FulltextIndexSettings::STANDARD);
                analyzers->set_use_filter_superlemmer(true);
                return settings;
            };

            TString error;

            {
                auto settings = makeSettings();
                UNIT_ASSERT_C(!ValidateSettings(settings, error), error);
                UNIT_ASSERT_VALUES_EQUAL(error, "language required when use_filter_superlemmer is set");
            }

            {
                auto settings = makeSettings();
                settings.mutable_columns()->at(0).mutable_analyzers()->set_language("klingon");
                UNIT_ASSERT_C(!ValidateSettings(settings, error), error);
                UNIT_ASSERT_VALUES_EQUAL(error, "language is not supported by superlemmer");
            }

            {
                auto settings = makeSettings();
                auto* analyzers = settings.mutable_columns()->at(0).mutable_analyzers();
                analyzers->set_language("russian");
                analyzers->set_use_filter_snowball(true);
                UNIT_ASSERT_C(!ValidateSettings(settings, error), error);
                UNIT_ASSERT_VALUES_EQUAL(error, "cannot set use_filter_snowball and use_filter_superlemmer at the same time");
            }

            for (bool edge : {false, true}) {
                auto settings = makeSettings();
                auto* analyzers = settings.mutable_columns()->at(0).mutable_analyzers();
                analyzers->set_language("russian");
                if (edge) {
                    analyzers->set_use_filter_edge_ngram(true);
                } else {
                    analyzers->set_use_filter_ngram(true);
                }
                UNIT_ASSERT_C(!ValidateSettings(settings, error), error);
                UNIT_ASSERT_VALUES_EQUAL(error, "cannot set use_filter_superlemmer with use_filter_ngram or use_filter_edge_ngram at the same time");
            }

            {
                auto settings = makeSettings();
                settings.mutable_columns()->at(0).mutable_analyzers()->set_language("russian, english");
                UNIT_ASSERT_C(ValidateSettings(settings, error), error);
                UNIT_ASSERT_VALUES_EQUAL(error, "");
            }

            {
                auto settings = makeSettings();
                settings.mutable_columns()->at(0).mutable_analyzers()->set_language("russian,klingon");
                UNIT_ASSERT_C(!ValidateSettings(settings, error), error);
                UNIT_ASSERT_VALUES_EQUAL(error, "language is not supported by superlemmer");
            }
        }

        Y_UNIT_TEST(FillSetting) {
            TString error;
            Ydb::Table::FulltextIndexSettings settings;
            settings.add_columns()->set_column("text");
            UNIT_ASSERT_VALUES_EQUAL(settings.columns().size(), 1);
            UNIT_ASSERT_VALUES_EQUAL(settings.columns().at(0).column(), "text");

            UNIT_ASSERT_C(FillSetting(settings, "tokenizer", "standard", error), error);
            UNIT_ASSERT_VALUES_EQUAL(error, "");
            UNIT_ASSERT_EQUAL(settings.columns().at(0).analyzers().tokenizer(), Ydb::Table::FulltextIndexSettings::STANDARD);

            UNIT_ASSERT_C(FillSetting(settings, "use_filter_lowercase", "true", error), error);
            UNIT_ASSERT_VALUES_EQUAL(error, "");
            UNIT_ASSERT_VALUES_EQUAL(settings.columns().at(0).analyzers().use_filter_lowercase(), true);

            UNIT_ASSERT_C(FillSetting(settings, "use_filter_length", "true", error), error);
            UNIT_ASSERT_VALUES_EQUAL(error, "");
            UNIT_ASSERT_VALUES_EQUAL(settings.columns().at(0).analyzers().use_filter_length(), true);

            UNIT_ASSERT_C(FillSetting(settings, "filter_length_min", "4", error), error);
            UNIT_ASSERT_VALUES_EQUAL(error, "");
            UNIT_ASSERT_VALUES_EQUAL(settings.columns().at(0).analyzers().filter_length_min(), 4);

            UNIT_ASSERT_C(FillSetting(settings, "filter_length_max", "5", error), error);
            UNIT_ASSERT_VALUES_EQUAL(error, "");
            UNIT_ASSERT_VALUES_EQUAL(settings.columns().at(0).analyzers().filter_length_max(), 5);
        }

        Y_UNIT_TEST(FillAnalyzer) {
            TString error;
            Ydb::Table::FulltextIndexSettings settings;
            settings.add_columns()->set_column("text");

            UNIT_ASSERT_C(FillSetting(settings, "analyzer", "standard", error), error);
            auto* analyzers = settings.mutable_columns(0)->mutable_analyzers();
            UNIT_ASSERT_EQUAL(analyzers->tokenizer(), Ydb::Table::FulltextIndexSettings::STANDARD);
            UNIT_ASSERT(analyzers->use_filter_lowercase());
            UNIT_ASSERT(analyzers->use_filter_stopwords());
            UNIT_ASSERT_C(ValidateSettings(settings, error), error);

            analyzers->Clear();
            UNIT_ASSERT_C(FillSetting(settings, "analyzer", "snowball", error), error);
            UNIT_ASSERT_C(FillSetting(settings, "language", "russian", error), error);
            UNIT_ASSERT_EQUAL(analyzers->tokenizer(), Ydb::Table::FulltextIndexSettings::STANDARD);
            UNIT_ASSERT(analyzers->use_filter_lowercase());
            UNIT_ASSERT(analyzers->use_filter_stopwords());
            UNIT_ASSERT(analyzers->use_filter_snowball());
            UNIT_ASSERT_C(ValidateSettings(settings, error), error);

            analyzers->Clear();
            UNIT_ASSERT_C(FillSetting(settings, "analyzer", "keyword", error), error);
            UNIT_ASSERT_EQUAL(analyzers->tokenizer(), Ydb::Table::FulltextIndexSettings::KEYWORD);
            UNIT_ASSERT_C(ValidateSettings(settings, error), error);

            UNIT_ASSERT(!FillSetting(settings, "analyzer", "unknown", error));
            UNIT_ASSERT_VALUES_EQUAL(error, "Invalid analyzer: unknown");
        }

        Y_UNIT_TEST(FillSettingInvalid) {
            {
                Ydb::Table::FulltextIndexSettings settings;
                settings.add_columns()->set_column("text");

                TString error;
                UNIT_ASSERT_C(!FillSetting(settings, "asdf", "qwer", error), error);
                UNIT_ASSERT_VALUES_EQUAL(error, "Unknown index setting: asdf");
            }

            {
                Ydb::Table::FulltextIndexSettings settings;
                settings.add_columns()->set_column("text");

                TString error;
                UNIT_ASSERT_C(!FillSetting(settings, "layout", "flat", error), error);
                UNIT_ASSERT_VALUES_EQUAL(error, "Unknown index setting: layout");
            }

            {
                Ydb::Table::FulltextIndexSettings settings;
                settings.add_columns()->set_column("text");

                TString error;
                UNIT_ASSERT_C(!FillSetting(settings, "use_filter_lowercase", "asdf", error), error);
                UNIT_ASSERT_VALUES_EQUAL(error, "Invalid use_filter_lowercase: asdf");
            }
        }

        Y_UNIT_TEST(Analyze) {
            Ydb::Table::FulltextIndexSettings::Analyzers analyzers;
            TString text = "apple WaLLet  spaced-dog_cat 0123,456@";

            analyzers.set_tokenizer(Ydb::Table::FulltextIndexSettings::WHITESPACE);
            UNIT_ASSERT_VALUES_EQUAL(Analyze(text, analyzers), (TVector<TString>{"apple", "WaLLet", "spaced-dog_cat", "0123,456@"}));

            analyzers.set_tokenizer(Ydb::Table::FulltextIndexSettings::STANDARD);
            UNIT_ASSERT_VALUES_EQUAL(Analyze(text, analyzers), (TVector<TString>{"apple", "WaLLet", "spaced", "dog_cat", "0123,456"}));

            analyzers.set_tokenizer(Ydb::Table::FulltextIndexSettings::ALPHANUMERIC);
            UNIT_ASSERT_VALUES_EQUAL(Analyze(text, analyzers), (TVector<TString>{"apple", "WaLLet", "spaced", "dog", "cat", "0123", "456"}));

            analyzers.set_tokenizer(Ydb::Table::FulltextIndexSettings::KEYWORD);
            UNIT_ASSERT_VALUES_EQUAL(Analyze(text, analyzers), (TVector<TString>{text}));

            analyzers.set_tokenizer(Ydb::Table::FulltextIndexSettings::WHITESPACE);
            analyzers.set_use_filter_lowercase(true);

            UNIT_ASSERT_VALUES_EQUAL(Analyze(text, analyzers), (TVector<TString>{"apple", "wallet", "spaced-dog_cat", "0123,456@"}));
        }

        Y_UNIT_TEST(AnalyzeRu) {
            Ydb::Table::FulltextIndexSettings::Analyzers analyzers;
            TString text = "Привет, это test123 и слово Ёлка   ёль!";

            analyzers.set_tokenizer(Ydb::Table::FulltextIndexSettings::WHITESPACE);
            UNIT_ASSERT_VALUES_EQUAL(Analyze(text, analyzers), (TVector<TString>{"Привет,", "это", "test123", "и", "слово", "Ёлка", "ёль!"}));

            analyzers.set_tokenizer(Ydb::Table::FulltextIndexSettings::STANDARD);
            UNIT_ASSERT_VALUES_EQUAL(Analyze(text, analyzers), (TVector<TString>{"Привет", "это", "test123", "и", "слово", "Ёлка", "ёль"}));

            analyzers.set_tokenizer(Ydb::Table::FulltextIndexSettings::KEYWORD);
            UNIT_ASSERT_VALUES_EQUAL(Analyze(text, analyzers), (TVector<TString>{text}));

            analyzers.set_tokenizer(Ydb::Table::FulltextIndexSettings::STANDARD);
            analyzers.set_use_filter_lowercase(true);
            UNIT_ASSERT_VALUES_EQUAL(Analyze(text, analyzers), (TVector<TString>{"привет", "это", "test123", "и", "слово", "ёлка", "ёль"}));
        }

        Y_UNIT_TEST(AnalyzeFilterStopwords) {
            Ydb::Table::FulltextIndexSettings::Analyzers analyzers;
            analyzers.set_tokenizer(Ydb::Table::FulltextIndexSettings::STANDARD);
            analyzers.set_use_filter_lowercase(true);
            analyzers.set_use_filter_stopwords(true);

            UNIT_ASSERT_VALUES_EQUAL(
                Analyze("The quick brown fox is in the garden", analyzers),
                (TVector<TString>{"quick", "brown", "fox", "garden"}));

            analyzers.set_language("russian");
            UNIT_ASSERT_VALUES_EQUAL(
                Analyze("Это быстрый лис и он в саду", analyzers),
                (TVector<TString>{"быстрый", "лис", "саду"}));

            analyzers.set_language("english,russian");
            UNIT_ASSERT_VALUES_EQUAL(
                Analyze("The quick fox и быстрый лис", analyzers),
                (TVector<TString>{"quick", "fox", "быстрый", "лис"}));
        }

        Y_UNIT_TEST(AnalyzeInvalid) {
            Ydb::Table::FulltextIndexSettings::Analyzers analyzers;

            TVector<TString> texts = {
                "\xC2\x41",         // Invalid continuation byte
                "\xC0\x81",         // Overlong encoding
                "\x80",             // Lone continuation byte
                "\xF4\x90\x80\x80", // Outside Unicode range
                "\xE3\x81",         // Truncated (incomplete)
            };

            for (auto i : xrange(texts.size())) {
                TString testCase = TStringBuilder() << "case #" << i;
                auto& text = texts[i];

                analyzers.set_tokenizer(Ydb::Table::FulltextIndexSettings::WHITESPACE);
                UNIT_ASSERT_VALUES_EQUAL_C(Analyze(text, analyzers), (TVector<TString>{}), testCase);

                analyzers.set_tokenizer(Ydb::Table::FulltextIndexSettings::STANDARD);
                UNIT_ASSERT_VALUES_EQUAL_C(Analyze(text, analyzers), (TVector<TString>{}), testCase);

                analyzers.set_tokenizer(Ydb::Table::FulltextIndexSettings::KEYWORD);
                UNIT_ASSERT_VALUES_EQUAL_C(Analyze(text, analyzers), (TVector<TString>{}), testCase);

                analyzers.set_tokenizer(Ydb::Table::FulltextIndexSettings::KEYWORD);
                analyzers.set_use_filter_lowercase(true);
                UNIT_ASSERT_VALUES_EQUAL_C(Analyze(text, analyzers), (TVector<TString>{}), testCase);
            }
        }

        Y_UNIT_TEST(AnalyzeFilterLength) {
            Ydb::Table::FulltextIndexSettings::Analyzers analyzers;
            analyzers.set_tokenizer(Ydb::Table::FulltextIndexSettings::WHITESPACE);
            TString text = "cat eats mice every day";

            UNIT_ASSERT_VALUES_EQUAL(Analyze(text, analyzers), (TVector<TString>{"cat", "eats", "mice", "every", "day"}));

            analyzers.set_use_filter_length(true);
            UNIT_ASSERT_VALUES_EQUAL(Analyze(text, analyzers), (TVector<TString>{"cat", "eats", "mice", "every", "day"}));

            analyzers.set_filter_length_min(4);
            UNIT_ASSERT_VALUES_EQUAL(Analyze(text, analyzers), (TVector<TString>{"eats", "mice", "every"}));

            analyzers.set_filter_length_max(4);
            UNIT_ASSERT_VALUES_EQUAL(Analyze(text, analyzers), (TVector<TString>{"eats", "mice"}));

            analyzers.clear_filter_length_min();
            UNIT_ASSERT_VALUES_EQUAL(Analyze(text, analyzers), (TVector<TString>{"cat", "eats", "mice", "day"}));
        }

        Y_UNIT_TEST(AnalyzeFilterLengthRu) {
            Ydb::Table::FulltextIndexSettings::Analyzers analyzers;
            analyzers.set_tokenizer(Ydb::Table::FulltextIndexSettings::WHITESPACE);
            TString text = "кот ест мышей каждый день";

            UNIT_ASSERT_VALUES_EQUAL(Analyze(text, analyzers), (TVector<TString>{"кот", "ест", "мышей", "каждый", "день"}));

            analyzers.set_use_filter_length(true);
            UNIT_ASSERT_VALUES_EQUAL(Analyze(text, analyzers), (TVector<TString>{"кот", "ест", "мышей", "каждый", "день"}));

            analyzers.set_filter_length_min(4);
            UNIT_ASSERT_VALUES_EQUAL(Analyze(text, analyzers), (TVector<TString>{"мышей", "каждый", "день"}));

            analyzers.set_filter_length_max(4);
            UNIT_ASSERT_VALUES_EQUAL(Analyze(text, analyzers), (TVector<TString>{"день"}));

            analyzers.clear_filter_length_min();
            UNIT_ASSERT_VALUES_EQUAL(Analyze(text, analyzers), (TVector<TString>{"кот", "ест", "день"}));
        }

        Y_UNIT_TEST(AnalyzeFilterNgram) {
            Ydb::Table::FulltextIndexSettings::Analyzers analyzers;
            analyzers.set_tokenizer(Ydb::Table::FulltextIndexSettings::WHITESPACE);
            TString text = "это текст";

            UNIT_ASSERT_VALUES_EQUAL(Analyze(text, analyzers), (TVector<TString>{"это", "текст"}));

            analyzers.set_use_filter_ngram(true);
            analyzers.set_filter_ngram_min_length(2);
            analyzers.set_filter_ngram_max_length(3);
            UNIT_ASSERT_VALUES_EQUAL(Analyze(text, analyzers), (TVector<TString>{"эт", "это", "то", "те", "тек", "ек", "екс", "кс", "кст", "ст"}));

            analyzers.set_filter_ngram_min_length(4);
            analyzers.set_filter_ngram_max_length(10);
            UNIT_ASSERT_VALUES_EQUAL(Analyze("слово", analyzers), (TVector<TString>{"слов", "слово", "лово"}));

            analyzers.set_filter_ngram_min_length(10);
            analyzers.set_filter_ngram_max_length(10);
            UNIT_ASSERT_VALUES_EQUAL(Analyze("слово", analyzers), (TVector<TString>{}));

            analyzers.set_use_filter_ngram(false);
            analyzers.set_use_filter_edge_ngram(true);
            analyzers.set_filter_ngram_min_length(2);
            analyzers.set_filter_ngram_max_length(3);
            UNIT_ASSERT_VALUES_EQUAL(Analyze(text, analyzers), (TVector<TString>{"эт", "это", "те", "тек"}));
        }

        Y_UNIT_TEST(AnalyzeFilterSnowball) {
            Ydb::Table::FulltextIndexSettings::Analyzers analyzers;
            analyzers.set_tokenizer(Ydb::Table::FulltextIndexSettings::WHITESPACE);
            const TString russianText = "машины ездят по дорогам исправно";

            UNIT_ASSERT_VALUES_EQUAL(Analyze(russianText, analyzers), (TVector<TString>{"машины", "ездят", "по", "дорогам", "исправно"}));

            analyzers.set_use_filter_snowball(true);
            analyzers.set_language("russian");
            UNIT_ASSERT_VALUES_EQUAL(Analyze(russianText, analyzers), (TVector<TString>{"машин", "езд", "по", "дорог", "исправн"}));

            const TString englishText = "cars are driving properly on the roads";
            analyzers.set_language("english");
            UNIT_ASSERT_VALUES_EQUAL(Analyze(englishText, analyzers), (TVector<TString>{"car", "are", "drive", "proper", "on", "the", "road"}));

            analyzers.set_language("russian,english");
            UNIT_ASSERT_VALUES_EQUAL(
                Analyze("cars driving машины дорогам ελληνικά 123", analyzers),
                (TVector<TString>{"car", "drive", "машин", "дорог", "ελληνικά", "123"}));

            const TVector<TString> languages = {"armenian", "english", "greek", "russian", "tamil", "yiddish"};
            const TVector<TString> words = {"մեքենաներ", "cars", "αυτοκίνητα", "машины", "மரங்கள்", "הײַזער"};
            TVector<TString> expected;
            for (size_t i = 0; i < languages.size(); ++i) {
                analyzers.set_language(languages[i]);
                const auto stemmed = Analyze(words[i], analyzers);
                UNIT_ASSERT_VALUES_EQUAL(stemmed.size(), 1);
                expected.push_back(stemmed.front());
            }

            analyzers.set_language("armenian,english,greek,russian,tamil,yiddish");
            UNIT_ASSERT_VALUES_EQUAL(Analyze("մեքենաներ cars αυτοκίνητα машины மரங்கள் הײַזער", analyzers), expected);

            analyzers.set_language("klingon");
            UNIT_ASSERT_EXCEPTION(Analyze(englishText, analyzers), yexception);

            analyzers.clear_language();
            UNIT_ASSERT_EXCEPTION(Analyze(englishText, analyzers), yexception);
        }

        Y_UNIT_TEST(AnalyzeFilterSuperLemmerUsesLanguageMaskOnce) {
            RegisterSuperLemmer(IsTestSuperLemmerLanguageSupported, ApplyTestSuperLemmer);
            SuperLemmerCallState = {};

            Ydb::Table::FulltextIndexSettings::Analyzers analyzers;
            analyzers.set_tokenizer(Ydb::Table::FulltextIndexSettings::WHITESPACE);
            analyzers.set_use_filter_superlemmer(true);
            analyzers.set_language("english, russian,english");

            Analyze("cars машины", analyzers);

            UNIT_ASSERT_VALUES_EQUAL(SuperLemmerCallState.Calls, 2);
            UNIT_ASSERT_VALUES_EQUAL(SuperLemmerCallState.Languages, "english, russian,english");
            RegisterSuperLemmer(nullptr, nullptr);
        }

        Y_UNIT_TEST(BuildNgramsUtf8) {
            {
                TVector<TString> ngrams;
                BuildNgrams("abc023", 3, 3, false, ngrams);
                UNIT_ASSERT_VALUES_EQUAL(ngrams, (TVector<TString>{"abc", "bc0", "c02", "023"}));
            }

            {
                TVector<TString> ngrams;
                BuildNgrams("◌̧◌̇◌̣", 3, 3, false, ngrams);
                UNIT_ASSERT_VALUES_EQUAL(ngrams, (TVector<TString>{"◌̧◌", "\u0327◌̇", "◌̇◌", "\u0307◌̣"}));
            }

            {
                TVector<TString> ngrams;
                BuildNgrams("﷽‎؈ۻ", 2, 2, false, ngrams);
                UNIT_ASSERT_VALUES_EQUAL(ngrams, (TVector<TString>{"﷽‎", "‎؈", "؈ۻ"}));
            }

            {
                TVector<TString> ngrams;
                BuildNgrams("异体字異體字", 3, 3, false, ngrams);
                UNIT_ASSERT_VALUES_EQUAL(ngrams, (TVector<TString>{"异体字", "体字異", "字異體", "異體字"}));
            }

            {
                TVector<TString> ngrams;
                BuildNgrams("ä̸̱b̴̪͛", 3, 3, false, ngrams);
                UNIT_ASSERT_VALUES_EQUAL(ngrams, (TVector<TString>{"a\u0338\u0308", "\u0338\u0308\u0331", "\u0308\u0331b", "\u0331b\u0334", "b\u0334\u035B", "\u0334\u035B\u032A"}));
            }

            {
                TVector<TString> ngrams;
                BuildNgrams("😢🐶🐕🐈", 2, 2, false, ngrams);
                UNIT_ASSERT_VALUES_EQUAL(ngrams, (TVector<TString>{"😢🐶", "🐶🐕", "🐕🐈"}));
            }

            {
                TVector<TString> ngrams;
                BuildNgrams("4️⃣🐕‍🦺🐈‍⬛", 3, 3, false, ngrams);
                UNIT_ASSERT_VALUES_EQUAL(ngrams, (TVector<TString>{"4️⃣", "\uFE0F\u20E3🐕", "\u20E3🐕\u200D", "🐕‍🦺", "\u200D\U0001F9BA🐈", "\U0001F9BA🐈\u200D", "🐈‍⬛"}));
            }

            {
                TVector<TString> ngrams;
                BuildNgrams("👨‍👩‍👧‍👦🇦🇨", 2, 2, false, ngrams);
                UNIT_ASSERT_VALUES_EQUAL(ngrams, (TVector<TString>{"👨\u200D", "\u200D👩", "👩\u200D", "\u200D👧", "👧\u200D", "\u200D👦", "👦🇦", "🇦🇨"}));
            }
        }

        Y_UNIT_TEST(BuildSearchTermsStructured) {
            using T = TSearchTerm;
            Ydb::Table::FulltextIndexSettings::Analyzers analyzers;
            analyzers.set_tokenizer(Ydb::Table::FulltextIndexSettings::STANDARD);

            // No `+`: every term optional, tokenized like BuildSearchTerms.
            UNIT_ASSERT_VALUES_EQUAL(BuildSearchTermsStructured("apple banana", analyzers),
                                     (TVector<T>{{"apple", false}, {"banana", false}}));

            // Leading `+` marks a required term; bare terms stay optional.
            UNIT_ASSERT_VALUES_EQUAL(BuildSearchTermsStructured("+apple banana", analyzers),
                                     (TVector<T>{{"apple", true}, {"banana", false}}));

            // Multiple required terms mixed with optional ones.
            UNIT_ASSERT_VALUES_EQUAL(BuildSearchTermsStructured("+apple +banana cherry", analyzers),
                                     (TVector<T>{{"apple", true}, {"banana", true}, {"cherry", false}}));

            // A `+` term that analyzes into several tokens marks all of them required.
            UNIT_ASSERT_VALUES_EQUAL(BuildSearchTermsStructured("+spaced-dog cat", analyzers),
                                     (TVector<T>{{"spaced", true}, {"dog", true}, {"cat", false}}));

            // Bare `+` (no body) is ignored.
            UNIT_ASSERT_VALUES_EQUAL(BuildSearchTermsStructured("+ apple", analyzers),
                                     (TVector<T>{{"apple", false}}));

            // A `+` inside a term (not at term start) is not an operator.
            UNIT_ASSERT_VALUES_EQUAL(BuildSearchTermsStructured("c++ test", analyzers),
                                     (TVector<T>{{"c", false}, {"test", false}}));

            // Extra whitespace between terms is collapsed.
            UNIT_ASSERT_VALUES_EQUAL(BuildSearchTermsStructured("+apple   banana", analyzers),
                                     (TVector<T>{{"apple", true}, {"banana", false}}));

            // Analyzer filters apply to the term body after stripping `+`.
            analyzers.set_use_filter_lowercase(true);
            UNIT_ASSERT_VALUES_EQUAL(BuildSearchTermsStructured("+Apple banana", analyzers),
                                     (TVector<T>{{"apple", true}, {"banana", false}}));
            analyzers.set_use_filter_lowercase(false);

            // Keyword tokenizer without `+` keeps the whole query as a single token.
            analyzers.set_tokenizer(Ydb::Table::FulltextIndexSettings::KEYWORD);
            UNIT_ASSERT_VALUES_EQUAL(BuildSearchTermsStructured("foo bar", analyzers),
                                     (TVector<T>{{"foo bar", false}}));
        }

        Y_UNIT_TEST(MidNumberChainedStandard) {
            // Regression test: after consuming a MidNumber separator + digit, prev was
            // incorrectly set to LETTER instead of DIGIT, so a second separator would not
            // satisfy the (prev == DIGIT || prev == MID_DIGIT) guard and the token was cut short.
            Ydb::Table::FulltextIndexSettings::Analyzers analyzers;
            analyzers.set_tokenizer(Ydb::Table::FulltextIndexSettings::STANDARD);

            // Two separators: "1,2,3" must be one token.
            UNIT_ASSERT_VALUES_EQUAL(Analyze("1,2,3", analyzers), (TVector<TString>{"1,2,3"}));

            // Mix of MidNumber chars (comma and period).
            UNIT_ASSERT_VALUES_EQUAL(Analyze("1,2.3", analyzers), (TVector<TString>{"1,2.3"}));

            // Trailing separator does not join — "1," ends at safeEnd before the comma.
            UNIT_ASSERT_VALUES_EQUAL(Analyze("1,2,", analyzers), (TVector<TString>{"1,2"}));

            // Longer chain.
            UNIT_ASSERT_VALUES_EQUAL(Analyze("1,2,3,4,5", analyzers), (TVector<TString>{"1,2,3,4,5"}));

            // Multiple tokens separated by whitespace — each chained number is its own token.
            UNIT_ASSERT_VALUES_EQUAL(Analyze("1,2,3 4,5,6", analyzers), (TVector<TString>{"1,2,3", "4,5,6"}));

            // Mixed: a word token followed by a chained number token.
            UNIT_ASSERT_VALUES_EQUAL(Analyze("price 1,234,567", analyzers), (TVector<TString>{"price", "1,234,567"}));

            // Chained number token followed by a letter token.
            UNIT_ASSERT_VALUES_EQUAL(Analyze("1,2,3 end", analyzers), (TVector<TString>{"1,2,3", "end"}));

            // Punctuation breaks tokens: "1,2.foo" — the period before a letter is MidLetter territory,
            // but here it follows digits so the chain stops at "1,2" and "foo" is separate.
            UNIT_ASSERT_VALUES_EQUAL(Analyze("1,2.foo", analyzers), (TVector<TString>{"1,2", "foo"}));
        }

        Y_UNIT_TEST(CompiledQueryAndOr) {
            const auto analyzers = StandardAnalyzers(true);
            const auto land = MustCompile("quick fox", analyzers, Options("and"));
            UNIT_ASSERT(land.Operator == NTableIndex::NFulltext::EDefaultOperator::And);
            UNIT_ASSERT(!land.OptionalThreshold.has_value());
            UNIT_ASSERT_VALUES_EQUAL(land.Terms, (TVector<TCompiledFulltextTerm>{{"quick", false}, {"fox", false}}));
            UNIT_ASSERT(EvaluateFulltextText(land, TStringBuf("quick brown fox")));
            UNIT_ASSERT(!EvaluateFulltextText(land, TStringBuf("quick brown")));

            const auto lor = MustCompile("quick fox", analyzers, Options("OR"));
            UNIT_ASSERT(lor.Operator == NTableIndex::NFulltext::EDefaultOperator::Or);
            UNIT_ASSERT_VALUES_EQUAL(*lor.OptionalThreshold, 1u);
            UNIT_ASSERT(EvaluateFulltextText(lor, TStringBuf("quick brown")));
            UNIT_ASSERT(EvaluateFulltextText(lor, TStringBuf("the fox")));
            UNIT_ASSERT(!EvaluateFulltextText(lor, TStringBuf("lazy dog")));

            const auto def = MustCompile("quick fox", analyzers);
            UNIT_ASSERT(def.Operator == NTableIndex::NFulltext::EDefaultOperator::And);
            UNIT_ASSERT(def.Mode == EFulltextQueryMode::Keywords);
            UNIT_ASSERT(def.WildcardPattern.empty());
        }

        Y_UNIT_TEST(CompiledQueryRequiredTerms) {
            const auto analyzers = StandardAnalyzers(true);
            const TVector<TString> docs = {
                "the quick brown fox jumps over the lazy dog",
                "quick quick fox",
                "lazy dog sleeps",
                "brown bear eats honey",
                "xylophone music is rare",
            };
            auto keys = [&](const TString& search, TFulltextQueryOptions options) {
                const auto query = MustCompile(search, analyzers, options);
                TVector<ui64> matched;
                for (size_t i = 0; i < docs.size(); ++i) {
                    if (EvaluateFulltextText(query, TStringBuf(docs[i]))) {
                        matched.push_back(i);
                    }
                }
                return matched;
            };

            const auto or1 = Options("or", "1");
            const auto or2 = Options("or", "2");
            UNIT_ASSERT_VALUES_EQUAL(keys("+brown +fox", or1), (TVector<ui64>{0}));
            UNIT_ASSERT_VALUES_EQUAL(keys("+quick fox", or1), (TVector<ui64>{0, 1}));
            UNIT_ASSERT_VALUES_EQUAL(keys("+honey quick", or1), (TVector<ui64>{}));
            UNIT_ASSERT_VALUES_EQUAL(keys("+brown lazy honey", or1), (TVector<ui64>{0, 3}));
            UNIT_ASSERT_VALUES_EQUAL(keys("+brown +lazy dog", or1), (TVector<ui64>{0}));
            UNIT_ASSERT_VALUES_EQUAL(keys("+quick fox brown", or1), (TVector<ui64>{0, 1}));
            UNIT_ASSERT_VALUES_EQUAL(keys("+quick fox brown", or2), (TVector<ui64>{0}));

            const auto allRequired = MustCompile("+brown +fox", analyzers, or1);
            UNIT_ASSERT(!allRequired.OptionalThreshold.has_value());
            // Explicit "1" against zero optional terms clamps to 0, so the row-table
            // total is the required-term count. An omitted threshold adds the parser's
            // default 1 before that same count is clamped back to the term count.
            UNIT_ASSERT_VALUES_EQUAL(allRequired.MinimumShouldMatch, 2u);
            UNIT_ASSERT_VALUES_EQUAL(MustCompile("+brown +fox", analyzers, Options("or")).MinimumShouldMatch, 3u);
            UNIT_ASSERT(EvaluateFulltextText(allRequired, TStringBuf(docs[0])));
            UNIT_ASSERT(!EvaluateFulltextText(allRequired, TStringBuf(docs[3])));
        }

        Y_UNIT_TEST(CompiledQueryDuplicateTerms) {
            const auto analyzers = StandardAnalyzers();
            UNIT_ASSERT_VALUES_EQUAL(MustCompile("apple apple", analyzers).Terms,
                (TVector<TCompiledFulltextTerm>{{"apple", false}}));
            UNIT_ASSERT_VALUES_EQUAL(MustCompile("apple +apple", analyzers).Terms,
                (TVector<TCompiledFulltextTerm>{{"apple", true}}));
            UNIT_ASSERT_VALUES_EQUAL(MustCompile("+apple apple", analyzers).Terms,
                (TVector<TCompiledFulltextTerm>{{"apple", true}}));
            UNIT_ASSERT_VALUES_EQUAL(MustCompile("banana apple +apple", analyzers).Terms,
                (TVector<TCompiledFulltextTerm>{{"banana", false}, {"apple", true}}));

            const auto ngram = NgramAnalyzers(2, 2);
            UNIT_ASSERT_VALUES_EQUAL(MustCompile("aaaa", ngram).Terms,
                (TVector<TCompiledFulltextTerm>{{"aa", false}}));
        }

        Y_UNIT_TEST(CompiledQueryPlusFoo) {
            const auto analyzers = StandardAnalyzers();
            const auto required = MustCompile("+foo", analyzers, Options("or"));
            UNIT_ASSERT_VALUES_EQUAL(required.Terms, (TVector<TCompiledFulltextTerm>{{"foo", true}}));
            UNIT_ASSERT(!required.OptionalThreshold.has_value());
            UNIT_ASSERT(EvaluateFulltextText(required, TStringBuf("foo bar")));
            UNIT_ASSERT(!EvaluateFulltextText(required, TStringBuf("bar")));

            const auto plain = MustCompile("foo", analyzers, Options("or"));
            UNIT_ASSERT_VALUES_EQUAL(plain.Terms, (TVector<TCompiledFulltextTerm>{{"foo", false}}));
            UNIT_ASSERT(EvaluateFulltextText(plain, TStringBuf("foo")));
        }

        Y_UNIT_TEST(CompiledQueryBarePlus) {
            const auto analyzers = StandardAnalyzers();
            for (const char* query : {"+", "+ ", " + ", "+ +"}) {
                const auto validation = CompileFulltextQuery(query, analyzers);
                UNIT_ASSERT_C(!validation, query);
                UNIT_ASSERT_STRING_CONTAINS(validation.Error, "No search terms were extracted from the query");
            }
            UNIT_ASSERT_VALUES_EQUAL(MustCompile("+ foo", analyzers).Terms,
                (TVector<TCompiledFulltextTerm>{{"foo", false}}));

            const auto keyword = KeywordAnalyzers();
            const auto plusPlus = CompileFulltextQuery("++", keyword);
            UNIT_ASSERT(plusPlus);
            UNIT_ASSERT_VALUES_EQUAL(plusPlus.Compiled->Terms, (TVector<TCompiledFulltextTerm>{{"+", true}}));
            const auto standardPlusPlus = CompileFulltextQuery("++", analyzers);
            UNIT_ASSERT(!standardPlusPlus);
            UNIT_ASSERT_STRING_CONTAINS(standardPlusPlus.Error, "No search terms were extracted from the query");
        }

        Y_UNIT_TEST(CompiledQueryCPlusPlus) {
            const auto standard = StandardAnalyzers();
            UNIT_ASSERT_VALUES_EQUAL(MustCompile("c++", standard).Terms,
                (TVector<TCompiledFulltextTerm>{{"c", false}}));
            UNIT_ASSERT_VALUES_EQUAL(MustCompile("c++ test", standard).Terms,
                (TVector<TCompiledFulltextTerm>{{"c", false}, {"test", false}}));
            UNIT_ASSERT_VALUES_EQUAL(MustCompile("+c++", standard).Terms,
                (TVector<TCompiledFulltextTerm>{{"c", true}}));

            const auto keyword = KeywordAnalyzers();
            const auto whole = MustCompile("c++", keyword);
            UNIT_ASSERT_VALUES_EQUAL(whole.Terms, (TVector<TCompiledFulltextTerm>{{"c++", false}}));
            UNIT_ASSERT(EvaluateFulltextText(whole, TStringBuf("c++")));
            UNIT_ASSERT(!EvaluateFulltextText(whole, TStringBuf("c")));
        }

        Y_UNIT_TEST(CompiledQueryWhitespace) {
            const auto analyzers = StandardAnalyzers();
            UNIT_ASSERT_VALUES_EQUAL(MustCompile("  apple\t\tbanana\nbaz  ", analyzers).Terms,
                (TVector<TCompiledFulltextTerm>{{"apple", false}, {"banana", false}, {"baz", false}}));
            UNIT_ASSERT_VALUES_EQUAL(MustCompile("\t+apple\r\nbanana\f\vbaz", analyzers).Terms,
                (TVector<TCompiledFulltextTerm>{{"apple", true}, {"banana", false}, {"baz", false}}));
            UNIT_ASSERT_VALUES_EQUAL(MustCompile("+apple   banana", analyzers).Terms,
                (TVector<TCompiledFulltextTerm>{{"apple", true}, {"banana", false}}));
        }

        Y_UNIT_TEST(CompiledQueryKeywordWithSpaces) {
            const auto analyzers = KeywordAnalyzers();
            const auto spaced = MustCompile("foo bar", analyzers);
            UNIT_ASSERT_VALUES_EQUAL(spaced.Terms, (TVector<TCompiledFulltextTerm>{{"foo bar", false}}));
            UNIT_ASSERT(EvaluateFulltextText(spaced, TStringBuf("foo bar")));
            UNIT_ASSERT(!EvaluateFulltextText(spaced, TStringBuf("foo")));
            UNIT_ASSERT(!EvaluateFulltextText(spaced, TStringBuf("foo  bar")));

            const auto padded = MustCompile(" foo bar ", analyzers);
            UNIT_ASSERT_VALUES_EQUAL(padded.Terms, (TVector<TCompiledFulltextTerm>{{" foo bar ", false}}));
            UNIT_ASSERT(EvaluateFulltextText(padded, TStringBuf(" foo bar ")));
            UNIT_ASSERT(!EvaluateFulltextText(padded, TStringBuf("foo bar")));

            UNIT_ASSERT_VALUES_EQUAL(MustCompile("+foo bar", analyzers).Terms,
                (TVector<TCompiledFulltextTerm>{{"foo", true}, {"bar", false}}));
        }

        Y_UNIT_TEST(CompiledQueryThresholdBoundaries) {
            const auto analyzers = StandardAnalyzers();
            const TString query = "a b c d";

            auto threshold = [&](const TString& minimumShouldMatch) {
                const auto compiled = MustCompile(query, analyzers, Options("or", minimumShouldMatch));
                UNIT_ASSERT(compiled.OptionalThreshold.has_value());
                return *compiled.OptionalThreshold;
            };

            UNIT_ASSERT_VALUES_EQUAL(threshold(""), 1u);
            UNIT_ASSERT_VALUES_EQUAL(threshold("1"), 1u);
            UNIT_ASSERT_VALUES_EQUAL(threshold("0"), 1u);
            UNIT_ASSERT_VALUES_EQUAL(threshold("4"), 4u);
            UNIT_ASSERT_VALUES_EQUAL(threshold("100"), 4u);
            UNIT_ASSERT_VALUES_EQUAL(threshold("-1"), 3u);
            UNIT_ASSERT_VALUES_EQUAL(threshold("-3"), 1u);
            UNIT_ASSERT_VALUES_EQUAL(threshold("-4"), 1u);
            UNIT_ASSERT_VALUES_EQUAL(threshold("-100"), 1u);
            UNIT_ASSERT_VALUES_EQUAL(threshold("50%"), 2u);
            UNIT_ASSERT_VALUES_EQUAL(threshold("100%"), 4u);
            UNIT_ASSERT_VALUES_EQUAL(threshold("1%"), 1u);

            const auto three = MustCompile("a b c", analyzers, Options("or", "50%"));
            UNIT_ASSERT_VALUES_EQUAL(*three.OptionalThreshold, 1u);
            UNIT_ASSERT(EvaluateFulltextText(three, TStringBuf("a")));
            UNIT_ASSERT(!EvaluateFulltextText(three, TStringBuf("z")));
            const auto threeAll = MustCompile("a b c", analyzers, Options("or", "100%"));
            UNIT_ASSERT_VALUES_EQUAL(*threeAll.OptionalThreshold, 3u);
            UNIT_ASSERT(!EvaluateFulltextText(threeAll, TStringBuf("a b")));
            UNIT_ASSERT(EvaluateFulltextText(threeAll, TStringBuf("c b a")));

            const auto one = MustCompile("a", analyzers, Options("or", "50%"));
            UNIT_ASSERT_VALUES_EQUAL(*one.OptionalThreshold, 1u);

            const auto withRequired = MustCompile("+a b c", analyzers, Options("or", "1"));
            UNIT_ASSERT_VALUES_EQUAL(*withRequired.OptionalThreshold, 1u);
            UNIT_ASSERT_VALUES_EQUAL(withRequired.MinimumShouldMatch, 2u);
            UNIT_ASSERT(!EvaluateFulltextText(withRequired, TStringBuf("a")));
            UNIT_ASSERT(EvaluateFulltextText(withRequired, TStringBuf("a b")));
            UNIT_ASSERT(!EvaluateFulltextText(withRequired, TStringBuf("b c")));
            const auto bothOptionals = MustCompile("+a b c", analyzers, Options("or", "2"));
            UNIT_ASSERT(!EvaluateFulltextText(bothOptionals, TStringBuf("a b")));
            UNIT_ASSERT(EvaluateFulltextText(bothOptionals, TStringBuf("a b c")));

            auto expectError = [&](const TString& search, TFulltextQueryOptions options, const TString& message) {
                const auto validation = CompileFulltextQuery(search, analyzers, options);
                UNIT_ASSERT_C(!validation, search);
                UNIT_ASSERT_STRING_CONTAINS(validation.Error, message);
            };
            expectError("a b", Options("and", "1"), "MinimumShouldMatch is not supported for AND default operator");
            expectError("a b", Options("or", "0%"), "Should be positive");
            expectError("a b", Options("or", "101%"), "Should be less than or equal to 100");
            expectError("a b", Options("or", "-1%"), "Should be positive");
            expectError("a b", Options("or", "non_numeric%"), "Invalid percentage");
            expectError("a b", Options("or", "non_numeric"), "Should be a number");
            expectError("a b", Options("some"), "Unsupported default operator");
        }

        Y_UNIT_TEST(CompiledQueryNullAndEmptyText) {
            const auto standard = StandardAnalyzers();
            const auto keyword = KeywordAnalyzers();
            const auto standardQuery = MustCompile("hello", standard);
            const auto keywordQuery = MustCompile("hello", keyword);

            UNIT_ASSERT(!EvaluateFulltextText(standardQuery, std::nullopt));
            UNIT_ASSERT(!EvaluateFulltextText(keywordQuery, std::nullopt));
            UNIT_ASSERT(!EvaluateFulltextText(standardQuery, TStringBuf()));
            UNIT_ASSERT(!EvaluateFulltextText(keywordQuery, TStringBuf()));

            UNIT_ASSERT_VALUES_EQUAL(TokenizeFulltextDocument(standardQuery, ""), (TVector<TString>{}));
            UNIT_ASSERT_VALUES_EQUAL(TokenizeFulltextDocument(keywordQuery, ""), (TVector<TString>{""}));
            UNIT_ASSERT(!EvaluateFulltextMembership(keywordQuery, TokenSet(keywordQuery, "")));
            UNIT_ASSERT(EvaluateFulltextMembership(keywordQuery, TokenSet(keywordQuery, "hello")));

            const auto spaces = MustCompile("   ", keyword);
            UNIT_ASSERT_VALUES_EQUAL(spaces.Terms, (TVector<TCompiledFulltextTerm>{{"   ", false}}));
            UNIT_ASSERT(!EvaluateFulltextText(spaces, TStringBuf()));
            UNIT_ASSERT(EvaluateFulltextText(spaces, TStringBuf("   ")));
            const auto blank = CompileFulltextQuery("   ", standard);
            UNIT_ASSERT(!blank);
            UNIT_ASSERT_STRING_CONTAINS(blank.Error, "No search terms were extracted from the query");
        }

        Y_UNIT_TEST(CompiledQueryValidationErrors) {
            const auto analyzers = StandardAnalyzers();
            const auto empty = CompileFulltextQuery("", analyzers);
            UNIT_ASSERT(!empty);
            UNIT_ASSERT_VALUES_EQUAL(empty.Error, "Empty fulltext query");

            auto withStopwords = analyzers;
            withStopwords.set_use_filter_stopwords(true);
            withStopwords.set_use_filter_lowercase(true);
            const auto noTerms = CompileFulltextQuery("the a", withStopwords);
            UNIT_ASSERT(!noTerms);
            UNIT_ASSERT_VALUES_EQUAL(noTerms.Error, "No search terms were extracted from the query");

            auto columnMode = Options();
            columnMode.Mode = "Query";
            const auto queryMode = CompileFulltextQuery("hello", analyzers, columnMode);
            UNIT_ASSERT(!queryMode);
            UNIT_ASSERT_STRING_CONTAINS(queryMode.Error, "Unsupported fulltext mode");

            columnMode.Mode = "bogus";
            const auto bogus = CompileFulltextQuery("hello", analyzers, columnMode);
            UNIT_ASSERT(!bogus);
            UNIT_ASSERT_STRING_CONTAINS(bogus.Error, "Unsupported fulltext mode");

            columnMode.Mode.clear();
            columnMode.ModeIsParameter = true;
            const auto parameter = CompileFulltextQuery("hello", analyzers, columnMode);
            UNIT_ASSERT(!parameter);
            UNIT_ASSERT_VALUES_EQUAL(parameter.Error, "Parameterized fulltext mode is not supported");

            columnMode = Options();
            columnMode.Mode = "Wildcard";
            const auto wildcard = CompileFulltextQuery("hello%", analyzers, columnMode);
            UNIT_ASSERT(!wildcard);
            UNIT_ASSERT_STRING_CONTAINS(wildcard.Error, "use_filter_ngram or use_filter_edge_ngram");

            const auto ngram = NgramAnalyzers(3, 4);
            for (const char* pattern : {"%", "%%%", "_", "ab%"}) {
                const auto tooShort = CompileFulltextQuery(pattern, ngram, columnMode);
                UNIT_ASSERT_C(!tooShort, pattern);
                UNIT_ASSERT_STRING_CONTAINS(tooShort.Error, "No search terms were extracted from the query");
            }

            auto row = Options();
            row.Checks = EFulltextQueryChecks::RowTable;
            row.Mode = "Query";
            const auto preserved = MustCompile("hello world", analyzers, row);
            UNIT_ASSERT(preserved.Mode == EFulltextQueryMode::Keywords);
            UNIT_ASSERT_VALUES_EQUAL(preserved.Terms, MustCompile("hello world", analyzers).Terms);

            row.Mode = "Wildcard";
            const auto rowWildcard = MustCompile("hello%", analyzers, row);
            UNIT_ASSERT(rowWildcard.Mode == EFulltextQueryMode::Wildcard);
            UNIT_ASSERT_VALUES_EQUAL(rowWildcard.WildcardPattern, "hello%");

            const auto keywords = MustCompile("Hello", analyzers, Options({}, {}, "KeYwOrDs"));
            UNIT_ASSERT(keywords.Mode == EFulltextQueryMode::Keywords);
            UNIT_ASSERT_VALUES_EQUAL(keywords.AnalyzerIdentity, MustCompile("other", analyzers).AnalyzerIdentity);
            auto lower = analyzers;
            lower.set_use_filter_lowercase(true);
            UNIT_ASSERT_VALUES_UNEQUAL(keywords.AnalyzerIdentity, MustCompile("Hello", lower).AnalyzerIdentity);
            UNIT_ASSERT_VALUES_UNEQUAL(keywords.AnalyzerRevision, MustCompile("Hello", lower).AnalyzerRevision);
            UNIT_ASSERT_VALUES_EQUAL(keywords.AnalyzerRevision, CityHash64(keywords.AnalyzerIdentity));
        }

        Y_UNIT_TEST(CompiledQueryWildcardResidual) {
            auto options = Options();
            options.Mode = "wildcard";
            const auto ngram = NgramAnalyzers(3, 4);
            const auto wildcard = MustCompile("abc%", ngram, options);
            UNIT_ASSERT(wildcard.Mode == EFulltextQueryMode::Wildcard);
            UNIT_ASSERT_VALUES_EQUAL(wildcard.WildcardPattern, "abc%");
            UNIT_ASSERT_VALUES_EQUAL(wildcard.Terms, (TVector<TCompiledFulltextTerm>{{"abc", false}}));

            UNIT_ASSERT(EvaluateFulltextText(wildcard, TStringBuf("abcd")));
            UNIT_ASSERT(EvaluateFulltextText(wildcard, TStringBuf("abc")));
            UNIT_ASSERT(!EvaluateFulltextText(wildcard, TStringBuf("zzabc")));
            UNIT_ASSERT(!EvaluateFulltextText(wildcard, TStringBuf("ABCD")));
            UNIT_ASSERT(EvaluateFulltextMembership(wildcard, TokenSet(wildcard, "zzabc")));
            UNIT_ASSERT(!MatchFulltextWildcardResidual(wildcard, "zzabc"));
            UNIT_ASSERT(MatchFulltextWildcardResidual(wildcard, "ABCD"));
            UNIT_ASSERT(!EvaluateFulltextText(wildcard, std::nullopt));

            const auto keywords = MustCompile("abc%", ngram, Options({}, {}, "keywords"));
            UNIT_ASSERT(keywords.WildcardPattern.empty());
            UNIT_ASSERT(!MatchFulltextWildcardResidual(keywords, "abcd"));
            UNIT_ASSERT(EvaluateFulltextText(keywords, TStringBuf("zzabc")));
            UNIT_ASSERT(!EvaluateFulltextText(wildcard, TStringBuf("zzabc")));

            const auto like = MustCompile("a_c", NgramAnalyzers(1, 1), options);
            UNIT_ASSERT(EvaluateFulltextText(like, TStringBuf("abc")));
            UNIT_ASSERT(EvaluateFulltextText(like, TStringBuf("axc")));
            UNIT_ASSERT(EvaluateFulltextMembership(like, TokenSet(like, "abbc")));
            UNIT_ASSERT(!MatchFulltextWildcardResidual(like, "abbc"));
            UNIT_ASSERT(!EvaluateFulltextText(like, TStringBuf("abbc")));
            UNIT_ASSERT(!EvaluateFulltextText(like, TStringBuf("ac")));

            const auto both = MustCompile("abc%def", ngram, options);
            UNIT_ASSERT(EvaluateFulltextText(both, TStringBuf("abcdef")));
            UNIT_ASSERT(EvaluateFulltextMembership(both, TokenSet(both, "defabc")));
            UNIT_ASSERT(!EvaluateFulltextText(both, TStringBuf("defabc")));

            auto lowerAnalyzers = ngram;
            lowerAnalyzers.set_use_filter_lowercase(true);
            const auto lower = MustCompile("ABC%", lowerAnalyzers, options);
            UNIT_ASSERT(EvaluateFulltextText(lower, TStringBuf("abcd")));
            UNIT_ASSERT(EvaluateFulltextText(lower, TStringBuf("ABCD")));
        }

        Y_UNIT_TEST(WordBreakTest) {
            // Generated from
            // http://www.unicode.org/Public/12.1.0/ucd/auxiliary/WordBreakTest.txt
            // and
            // http://www.unicode.org/Public/12.1.0/ucd/auxiliary/WordBreakProperty.txt
            TString tests = NResource::Find("word_break_test.json");
            NJson::TJsonValue out;
            ReadJsonTree(tests, &out, true);
            Ydb::Table::FulltextIndexSettings::Analyzers analyzers;
            analyzers.set_tokenizer(Ydb::Table::FulltextIndexSettings::STANDARD);
            int ok = 0, failed = 0;
            for (auto& test : out.GetArray()) {
                TString in = test["input"].GetString();
                TVector<TString> expected;
                for (auto& token : test["tokens"].GetArray()) {
                    expected.push_back(token.GetString());
                }
                TVector<TString> out = Analyze(in, analyzers);
                if (out != expected) {
                    Cerr << "Input: " << in << "\nTokens:";
                    for (auto& token : out) {
                        Cerr << " " << token;
                    }
                    Cerr << "\nExpected:";
                    for (auto& token : expected) {
                        Cerr << " " << token;
                    }
                    Cerr << "\n\n";
                    failed++;
                } else {
                    ok++;
                }
            }
            if (failed > 0) {
                Cerr << "Ok " << ok << "/" << (ok + failed) << " tests\n";
            }
            UNIT_ASSERT(!failed);
        }
    } // Y_UNIT_TEST_SUITE(NFulltext)

} // namespace NKikimr::NFulltext
