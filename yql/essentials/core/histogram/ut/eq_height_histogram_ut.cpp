#include <library/cpp/testing/unittest/registar.h>

#include <yql/essentials/core/histogram/eq_height_histogram.h>

#include <util/generic/string.h>
#include <util/generic/strbuf.h>

#include <algorithm>
#include <cstring>
#include <random>
#include <utility>
#include <vector>

using namespace NKikimr;

namespace {

// Memcomparable key from an integer: 4 bytes big-endian, sign bit flipped.
TString MakeKey(i32 val) {
    const ui32 u = static_cast<ui32>(val) ^ (1U << 31);
    TString out(4, '\0');
    out[0] = static_cast<char>((u >> 24) & 0xFF);
    out[1] = static_cast<char>((u >> 16) & 0xFF);
    out[2] = static_cast<char>((u >> 8) & 0xFF);
    out[3] = static_cast<char>(u & 0xFF);
    return out;
}

TEqHeightHistogramBuilder::TParams MakeParams(ui32 numBuckets, ui64 maxStateBytes = 4ULL << 20) {
    TEqHeightHistogramBuilder::TParams p;
    p.NumBuckets = numBuckets;
    p.EmissionRate = numBuckets * 8;
    p.MaxStateBytes = maxStateBytes;
    return p;
}

// Sorted stream: 0,0,...,1,1,...,2,2,... with `perKey` copies each.
std::vector<TString> MakeSortedStream(ui32 numKeys, ui32 perKey) {
    std::vector<TString> keys;
    keys.reserve(static_cast<size_t>(numKeys) * perKey);
    for (ui32 k = 0; k < numKeys; ++k) {
        for (ui32 r = 0; r < perKey; ++r) {
            keys.push_back(MakeKey(static_cast<i32>(k)));
        }
    }
    return keys;
}

// Build from a sorted stream, adding each key.
TEqHeightHistogramBuilder BuildFromStream(const std::vector<TString>& keys, const TEqHeightHistogramBuilder::TParams& params) {
    TEqHeightHistogramBuilder b(params);
    for (const auto& k : keys) {
        b.Add(k);
    }
    return b;
}

// Split a sorted stream into k contiguous chunks.
std::vector<std::vector<TString>> SplitContiguous(const std::vector<TString>& keys, ui32 k) {
    std::vector<std::vector<TString>> chunks(k);
    size_t per = keys.size() / k;
    size_t rem = keys.size() % k;
    size_t idx = 0;
    for (ui32 c = 0; c < k; ++c) {
        size_t sz = per + (c < rem ? 1 : 0);
        chunks[c].assign(keys.begin() + idx, keys.begin() + idx + sz);
        idx += sz;
    }
    return chunks;
}

// Build from one chunk.
TEqHeightHistogramBuilder BuildChunk(const std::vector<TString>& chunk, const TEqHeightHistogramBuilder::TParams& params) {
    return BuildFromStream(chunk, params);
}

// Merge in linear left-fold order.
TEqHeightHistogramBuilder MergeLinear(std::vector<TEqHeightHistogramBuilder> states, const TEqHeightHistogramBuilder::TParams& params) {
    TEqHeightHistogramBuilder acc(params);
    for (auto& s : states) {
        acc.Merge(s);
    }
    return acc;
}

// Merge in a balanced binary tree.
TEqHeightHistogramBuilder MergeBalanced(std::vector<TEqHeightHistogramBuilder> states, const TEqHeightHistogramBuilder::TParams& params) {
    if (states.empty()) {
        return TEqHeightHistogramBuilder(params);
    }
    while (states.size() > 1) {
        std::vector<TEqHeightHistogramBuilder> next;
        for (size_t i = 0; i < states.size(); i += 2) {
            if (i + 1 < states.size()) {
                states[i].Merge(states[i + 1]);
                next.push_back(std::move(states[i]));
            } else {
                next.push_back(std::move(states[i]));
            }
        }
        states = std::move(next);
    }
    return std::move(states[0]);
}

// Merge in a random tree order.
TEqHeightHistogramBuilder MergeRandom(std::vector<TEqHeightHistogramBuilder> states, const TEqHeightHistogramBuilder::TParams& params, ui64 seed) {
    if (states.empty()) {
        return TEqHeightHistogramBuilder(params);
    }
    std::mt19937 rng(seed);
    while (states.size() > 1) {
        size_t i = rng() % states.size();
        size_t j = rng() % states.size();
        if (i == j) {
            continue;
        }
        states[i].Merge(states[j]);
        states.erase(states.begin() + j);
    }
    return std::move(states[0]);
}

// Serialize/deserialize round-trip a builder.
TEqHeightHistogramBuilder RoundTrip(const TEqHeightHistogramBuilder& b) {
    TString s = b.Serialize();
    return TEqHeightHistogramBuilder(s.data(), s.size());
}

// Assert each bucket's CumulativeCount is within MaxRankError and `tolerance`
// of its true rank.  Pins MaxRankError as a genuine upper bound.
void AssertTrueRanks(const TEqHeightHistogram& hist,
                     const std::vector<TString>& allKeys,
                     ui64 maxRankError,
                     ui64 tolerance,
                     TStringBuf label) {
    std::vector<TString> sorted = allKeys;
    std::sort(sorted.begin(), sorted.end());
    for (size_t i = 0; i < hist.GetNumBuckets(); ++i) {
        const auto& bkt = hist.GetBucket(i);
        auto it = std::upper_bound(sorted.begin(), sorted.end(), bkt.UpperBound,
                                   [](TStringBuf value, const TString& elem) { return value < TStringBuf(elem); });
        const ui64 trueRank = static_cast<ui64>(it - sorted.begin());
        const ui64 actual = bkt.CumulativeCount;
        const ui64 diff = (actual > trueRank) ? (actual - trueRank) : (trueRank - actual);
        UNIT_ASSERT_C(diff <= maxRankError,
                      label << ": bucket " << i << " cumulative " << actual
                            << " deviates from true rank " << trueRank << " by " << diff
                            << " > MaxRankError " << maxRankError);
        UNIT_ASSERT_C(diff <= tolerance,
                      label << ": bucket " << i << " cumulative " << actual
                            << " deviates from true rank " << trueRank << " by " << diff
                            << " > tolerance " << tolerance);
    }
}

// Assert each bucket's size deviates from ideal (N/B) by at most WeightCap.
void AssertBucketSizes(const TEqHeightHistogram& hist, ui64 N, ui32 B, ui64 weightCap, TStringBuf label) {
    const ui64 idealBucket = N / B;
    for (size_t i = 0; i < hist.GetNumBuckets(); ++i) {
        ui64 prev = (i == 0) ? 0 : hist.GetBucket(i - 1).CumulativeCount;
        ui64 bucketSize = hist.GetBucket(i).CumulativeCount - prev;
        ui64 diff = (bucketSize > idealBucket) ? (bucketSize - idealBucket) : (idealBucket - bucketSize);
        UNIT_ASSERT_C(diff <= weightCap,
                      label << ": bucket " << i << " size " << bucketSize
                            << " deviates from ideal " << idealBucket << " by " << diff
                            << " > WeightCap " << weightCap);
    }
}

// Assert each bucket's cumulative count is within `tolerance` of ideal.
void AssertCumulativeVsIdeal(const TEqHeightHistogram& hist, ui64 N, ui32 B, ui64 tolerance, TStringBuf label) {
    for (size_t i = 0; i < hist.GetNumBuckets(); ++i) {
        ui64 ideal = (i + 1) * N / B;
        ui64 actual = hist.GetBucket(i).CumulativeCount;
        ui64 diff = (actual > ideal) ? (actual - ideal) : (ideal - actual);
        UNIT_ASSERT_C(diff <= tolerance,
                      label << ": bucket " << i << " cumulative " << actual
                            << " deviates from ideal " << ideal << " by " << diff
                            << " > tolerance " << tolerance);
    }
}

} // namespace

Y_UNIT_TEST_SUITE(EqHeightHistogram) {

// === sorted single run — exact (Delta == 0) ===
Y_UNIT_TEST(SortedSingleRunExact) {
    const ui32 numKeys = 1000;
    const ui32 perKey = 10;
    const ui32 B = 20;
    const ui64 N = static_cast<ui64>(numKeys) * perKey;
    auto keys = MakeSortedStream(numKeys, perKey);
    auto params = MakeParams(B);

    auto builder = BuildFromStream(keys, params);
    UNIT_ASSERT_C(builder.IsExact(),
                  "sorted single run must be exact (Delta == 0), "
                  "MaxRankError = "
                      << builder.GetMaxRankError());
    UNIT_ASSERT_VALUES_EQUAL(builder.GetTotalCount(), N);

    auto blob = builder.Finalize();
    UNIT_ASSERT(blob.has_value());
    TEqHeightHistogram hist(blob->data(), blob->size());
    UNIT_ASSERT_VALUES_EQUAL(hist.GetTotalCount(), N);
    UNIT_ASSERT_C(hist.IsExact(),
                  "finalized blob must be exact for a sorted single run, "
                  "MaxRankError = "
                      << hist.GetMaxRankError());

    for (size_t i = 0; i < hist.GetNumBuckets(); ++i) {
        const auto& bkt = hist.GetBucket(i);
        if (i > 0) {
            UNIT_ASSERT(bkt.CumulativeCount > hist.GetBucket(i - 1).CumulativeCount);
        }
    }
    UNIT_ASSERT_VALUES_EQUAL(hist.GetBucket(hist.GetNumBuckets() - 1).CumulativeCount, N);

    AssertTrueRanks(hist, keys, hist.GetMaxRankError(), 0, "SortedSingleRunExact");

    AssertBucketSizes(hist, N, B, builder.GetWeightCap(), "SortedSingleRunExact");
}

// === disjoint-run splice — exact (Delta == 0) ===
Y_UNIT_TEST(DisjointRunSplice) {
    const ui32 numKeys = 1000;
    const ui32 perKey = 10;
    const ui32 B = 20;
    const ui64 N = static_cast<ui64>(numKeys) * perKey;
    auto keys = MakeSortedStream(numKeys, perKey);
    auto params = MakeParams(B);

    // Single-run reference
    auto refBuilder = BuildFromStream(keys, params);
    auto refBlob = refBuilder.Finalize();
    UNIT_ASSERT(refBlob.has_value());
    TEqHeightHistogram refHist(refBlob->data(), refBlob->size());
    UNIT_ASSERT_VALUES_EQUAL(refHist.GetTotalCount(), N);
    UNIT_ASSERT_C(refHist.IsExact(),
                  "ref: sorted single run must be exact, MaxRankError = "
                      << refHist.GetMaxRankError());

    // Semantic equivalence: same total/bucket count, each cumulative within
    // WeightCap of the reference.  Boundaries may differ (chunking shifts
    // them), but cumulative counts stay within WeightCap.
    auto checkEquivalent = [&](const TString& blob, const TString& label) {
        TEqHeightHistogram hist(blob.data(), blob.size());
        UNIT_ASSERT_VALUES_EQUAL_C(hist.GetTotalCount(), refHist.GetTotalCount(), label);
        UNIT_ASSERT_VALUES_EQUAL_C(hist.GetNumBuckets(), refHist.GetNumBuckets(), label);
        const ui64 weightCap = refBuilder.GetWeightCap();
        for (size_t i = 0; i < refHist.GetNumBuckets(); ++i) {
            ui64 refCum = refHist.GetBucket(i).CumulativeCount;
            ui64 histCum = hist.GetBucket(i).CumulativeCount;
            ui64 diff = (refCum > histCum) ? (refCum - histCum) : (histCum - refCum);
            UNIT_ASSERT_C(diff <= weightCap,
                          label << ": bucket " << i << " cumulative " << histCum
                                << " deviates from ref " << refCum
                                << " by " << diff << " > WeightCap " << weightCap);
        }
    };

    // Split into k=8 contiguous chunks, merge in different orders
    auto chunks = SplitContiguous(keys, 8);
    std::vector<TEqHeightHistogramBuilder> states;
    for (const auto& chunk : chunks) {
        states.push_back(BuildChunk(chunk, params));
    }

    auto linearResult = MergeLinear(states, params);
    auto linearBlob = linearResult.Finalize();
    UNIT_ASSERT(linearBlob.has_value());
    checkEquivalent(*linearBlob, "linear");
    UNIT_ASSERT_C(linearResult.IsExact(),
                  "linear: disjoint splice must be exact, MaxRankError = "
                      << linearResult.GetMaxRankError());
    {
        TEqHeightHistogram hist(linearBlob->data(), linearBlob->size());
        AssertTrueRanks(hist, keys, hist.GetMaxRankError(), 0, "linear");
    }

    // Rebuild states for balanced merge
    states.clear();
    for (const auto& chunk : chunks) {
        states.push_back(BuildChunk(chunk, params));
    }
    auto balancedResult = MergeBalanced(states, params);
    auto balancedBlob = balancedResult.Finalize();
    UNIT_ASSERT(balancedBlob.has_value());
    checkEquivalent(*balancedBlob, "balanced");
    UNIT_ASSERT_C(balancedResult.IsExact(),
                  "balanced: disjoint splice must be exact, MaxRankError = "
                      << balancedResult.GetMaxRankError());
    {
        TEqHeightHistogram hist(balancedBlob->data(), balancedBlob->size());
        AssertTrueRanks(hist, keys, hist.GetMaxRankError(), 0, "balanced");
    }

    // Random merge of disjoint chunks: Delta stays 0 (no straddling).
    states.clear();
    for (const auto& chunk : chunks) {
        states.push_back(BuildChunk(chunk, params));
    }
    auto randomResult = MergeRandom(states, params, 42);
    auto randomBlob = randomResult.Finalize();
    UNIT_ASSERT(randomBlob.has_value());
    checkEquivalent(*randomBlob, "random");
    UNIT_ASSERT_C(randomResult.IsExact(),
                  "random: disjoint splice must be exact, MaxRankError = "
                      << randomResult.GetMaxRankError());
    {
        TEqHeightHistogram hist(randomBlob->data(), randomBlob->size());
        AssertTrueRanks(hist, keys, hist.GetMaxRankError(), 0, "random");
    }
}

// === touching ranges — must interleave, not splice ===
Y_UNIT_TEST(TouchingRanges) {
    const ui32 B = 20;
    const ui64 N = 1001; // 500 + 501 = 1001 (key 499 shared)
    auto params = MakeParams(B);

    // Two runs sharing key 499 at the boundary (a.MaxKey == b.MinKey).
    TEqHeightHistogramBuilder a(params);
    for (ui32 k = 0; k < 500; ++k) {
        a.Add(MakeKey(static_cast<i32>(k)));
    }
    TEqHeightHistogramBuilder b(params);
    for (ui32 k = 499; k < 1000; ++k) {
        b.Add(MakeKey(static_cast<i32>(k)));
    }

    a.Merge(b);
    UNIT_ASSERT_VALUES_EQUAL(a.GetTotalCount(), N);

    auto blob = a.Finalize();
    UNIT_ASSERT(blob.has_value());
    TEqHeightHistogram hist(blob->data(), blob->size());
    UNIT_ASSERT_VALUES_EQUAL(hist.GetTotalCount(), N);

    UNIT_ASSERT_C(hist.GetMaxRankError() <= N / B,
                  "MaxRankError " << hist.GetMaxRankError() << " > total/B " << (N / B));

    // True-rank assertion: the two runs share key 499 at the frontier, so
    // the equal-key fold handles it.  Pin MaxRankError against the full stream.
    std::vector<TString> allKeys;
    allKeys.reserve(N);
    for (ui32 k = 0; k < 500; ++k) {
        allKeys.push_back(MakeKey(static_cast<i32>(k)));
    }
    for (ui32 k = 499; k < 1000; ++k) {
        allKeys.push_back(MakeKey(static_cast<i32>(k)));
    }
    AssertTrueRanks(hist, allKeys, hist.GetMaxRankError(), 0, "TouchingRanges");
}

// === touching ranges with a multi-key frontier — the §1.1 reproducer ===
// One side's first entry is multi-key (compacted under tight MaxStateBytes);
// the other ends exactly on that key.  The bump in InterleaveInto must fire
// (>=, not >), or MaxRankError is too small and IsExact() lies.
Y_UNIT_TEST(TouchingRangesMultiKeyFrontier) {
    const ui32 B = 10;
    const ui32 perKey = 100;
    const ui32 aKeys = 200; // side A: keys 0..199
    const ui32 bKeys = 200; // side B: keys 0..199 (overlapping at key 0)
    const ui64 N = static_cast<ui64>(aKeys + bKeys) * perKey;
    auto params = MakeParams(B);

    // Side A: keys 0..199, each repeated perKey times, under a tight budget
    // so the first entry fuses keys 0 and 1 into a multi-key entry at MinKey 0.
    auto tightParams = MakeParams(B, 512);
    TEqHeightHistogramBuilder a(tightParams);
    for (ui32 k = 0; k < aKeys; ++k) {
        for (ui32 r = 0; r < perKey; ++r) {
            a.Add(MakeKey(static_cast<i32>(k)));
        }
    }

    // A's first entry must be multi-key (fused by the tight budget).
    const auto& aEntries = a.GetEntries();
    UNIT_ASSERT_C(!aEntries.empty(), "A must have at least one entry");
    UNIT_ASSERT_C(!aEntries.front().SingleKey,
                  "A's first entry must be multi-key (fused by tight budget), "
                  "got SingleKey with UpperBound "
                      << aEntries.front().UpperBound);
    UNIT_ASSERT_VALUES_EQUAL(a.GetMinKey(), MakeKey(0));

    // Side B: keys 0..199, each repeated perKey times, under a normal budget so
    // entries stay single-key.  B's first entry is key 0, SingleKey.
    TEqHeightHistogramBuilder b(params);
    for (ui32 k = 0; k < bKeys; ++k) {
        for (ui32 r = 0; r < perKey; ++r) {
            b.Add(MakeKey(static_cast<i32>(k)));
        }
    }
    const auto& bEntries = b.GetEntries();
    UNIT_ASSERT_C(bEntries.front().SingleKey,
                  "B's first entry must be single-key under a normal budget");

    // Merge: B.Merge(A).  B's first entry is key 0 (SingleKey); A's first
    // entry starts at key 0 (== B's UpperBound == A's MinKey).  The bump is
    // owed because A's rows at key 0 are <= B's UpperBound.  With > the bump
    // is skipped and MaxRankError is too small.
    b.Merge(a);
    UNIT_ASSERT_VALUES_EQUAL(b.GetTotalCount(), N);

    // The merge must not be exact: A's multi-key entry contributes Delta.
    UNIT_ASSERT_C(!b.IsExact(),
                  "merge with a multi-key frontier at the MinKey must be inexact, "
                  "MaxRankError = "
                      << b.GetMaxRankError());

    auto blob = b.Finalize();
    UNIT_ASSERT_C(blob.has_value(),
                  "Finalize must produce a histogram for the multi-key frontier case");
    TEqHeightHistogram hist(blob->data(), blob->size());
    UNIT_ASSERT_VALUES_EQUAL(hist.GetTotalCount(), N);

    // Measured true-rank error must be <= GetMaxRankError.
    std::vector<TString> allKeys;
    allKeys.reserve(N);
    for (ui32 k = 0; k < aKeys; ++k) {
        for (ui32 r = 0; r < perKey; ++r) {
            allKeys.push_back(MakeKey(static_cast<i32>(k)));
        }
    }
    for (ui32 k = 0; k < bKeys; ++k) {
        for (ui32 r = 0; r < perKey; ++r) {
            allKeys.push_back(MakeKey(static_cast<i32>(k)));
        }
    }
    // Honesty: true-rank error must be <= reported MaxRankError. A second
    // quality bar of total/B cannot fire: Finalize already rejected anything
    // larger. This case is budget-starved on one side, so the true error sits
    // close to that rail (~2900 of 4000) and a tighter extra bound is noise.
    AssertTrueRanks(hist, allKeys, hist.GetMaxRankError(), hist.GetMaxRankError(),
                    "TouchingRangesMultiKeyFrontier");
}

// === partition seams — parameterized over chunk/bucket ratio ===
Y_UNIT_TEST(PartitionSeamsManyChunks) {
    const ui32 numKeys = 1000;
    const ui32 perKey = 10;
    const ui64 N = static_cast<ui64>(numKeys) * perKey;

    struct TCase {
        ui32 numChunks;
        ui32 numBuckets;
        TString name;
    };
    TCase cases[] = {
        {100, 10, "chunks>>buckets"},
        {20, 20, "chunks~=buckets"},
        {5, 20, "chunks<<buckets"},
        {1000, 10, "one_row_per_chunk"},
    };

    for (const auto& tc : cases) {
        auto keys = MakeSortedStream(numKeys, perKey);
        auto params = MakeParams(tc.numBuckets);

        auto chunks = SplitContiguous(keys, tc.numChunks);
        std::vector<TEqHeightHistogramBuilder> states;
        for (const auto& chunk : chunks) {
            states.push_back(BuildChunk(chunk, params));
        }
        auto result = MergeLinear(states, params);

        UNIT_ASSERT_VALUES_EQUAL_C(result.GetTotalCount(), N, tc.name);

        auto blob = result.Finalize();
        UNIT_ASSERT_C(blob.has_value(), tc.name);
        TEqHeightHistogram hist(blob->data(), blob->size());
        UNIT_ASSERT_VALUES_EQUAL_C(hist.GetTotalCount(), N, tc.name);

        UNIT_ASSERT_C(result.IsExact(),
                      tc.name << ": disjoint splice must be exact, MaxRankError = "
                              << result.GetMaxRankError());

        AssertBucketSizes(hist, N, tc.numBuckets, result.GetWeightCap(), tc.name);
        AssertTrueRanks(hist, keys, hist.GetMaxRankError(), 0, tc.name);
    }
}

// === partition seams — uneven chunk sizes ===
Y_UNIT_TEST(PartitionSeamsUneven) {
    const ui32 numKeys = 1000;
    const ui32 perKey = 10;
    const ui32 B = 10;
    const ui64 N = static_cast<ui64>(numKeys) * perKey;
    auto keys = MakeSortedStream(numKeys, perKey);
    auto params = MakeParams(B);

    // Deliberately uneven chunks: 1%, 50%, 1%, 48%.
    std::vector<std::vector<TString>> chunks(4);
    size_t sizes[] = {N / 100, N / 2, N / 100, 0};
    sizes[3] = keys.size() - sizes[0] - sizes[1] - sizes[2];
    size_t idx = 0;
    for (int c = 0; c < 4; ++c) {
        chunks[c].assign(keys.begin() + idx, keys.begin() + idx + sizes[c]);
        idx += sizes[c];
    }

    std::vector<TEqHeightHistogramBuilder> states;
    for (const auto& chunk : chunks) {
        states.push_back(BuildChunk(chunk, params));
    }
    auto result = MergeLinear(states, params);

    UNIT_ASSERT_VALUES_EQUAL(result.GetTotalCount(), N);
    UNIT_ASSERT_C(result.IsExact(),
                  "uneven: disjoint splice must be exact, MaxRankError = "
                      << result.GetMaxRankError());

    auto blob = result.Finalize();
    UNIT_ASSERT(blob.has_value());
    TEqHeightHistogram hist(blob->data(), blob->size());
    UNIT_ASSERT_VALUES_EQUAL(hist.GetTotalCount(), N);

    AssertBucketSizes(hist, N, B, result.GetWeightCap(), "uneven");
    AssertTrueRanks(hist, keys, hist.GetMaxRankError(), 0, "uneven");
}

// === many-way merge tree — exactness under random partitioning ===
Y_UNIT_TEST(ManyWayMergeTree) {
    const ui32 numKeys = 500;
    const ui32 perKey = 4;
    const ui32 B = 10;
    const ui64 N = static_cast<ui64>(numKeys) * perKey;
    const ui32 M = 64;

    auto keys = MakeSortedStream(numKeys, perKey);
    auto params = MakeParams(B);

    std::mt19937 rng(12345);
    auto shuffled = keys;
    std::shuffle(shuffled.begin(), shuffled.end(), rng);

    std::vector<std::vector<TString>> parts(M);
    for (size_t i = 0; i < shuffled.size(); ++i) {
        parts[i % M].push_back(shuffled[i]);
    }

    // 3 merge orders x 2 (with/without round-trip)
    const int Linear = 0, Balanced = 1, Random = 2;
    const int NoRoundTrip = 0, WithRoundTrip = 1;

    for (auto order : {Linear, Balanced, Random}) {
        for (auto rt : {NoRoundTrip, WithRoundTrip}) {
            std::vector<TEqHeightHistogramBuilder> states;
            for (const auto& part : parts) {
                auto s = BuildChunk(part, params);
                if (rt == WithRoundTrip) {
                    s = RoundTrip(s);
                }
                states.push_back(std::move(s));
            }

            TEqHeightHistogramBuilder result(params);
            if (order == Linear) {
                result = MergeLinear(states, params);
            } else if (order == Balanced) {
                result = MergeBalanced(states, params);
            } else {
                result = MergeRandom(states, params, 99);
            }

            if (rt == WithRoundTrip) {
                result = RoundTrip(result);
            }

            UNIT_ASSERT_VALUES_EQUAL(result.GetTotalCount(), N);

            auto blob = result.Finalize();
            UNIT_ASSERT(blob.has_value());
            TEqHeightHistogram hist(blob->data(), blob->size());
            UNIT_ASSERT_VALUES_EQUAL(hist.GetTotalCount(), N);

            // Tolerance: 2N/(f*B) = N/(4*B).
            ui64 tolerance = N / (4 * B);
            AssertCumulativeVsIdeal(hist, N, B, tolerance,
                                    TStringBuf("order=") + ToString(order) + " rt=" + ToString(rt));
            AssertTrueRanks(hist, keys, hist.GetMaxRankError(), tolerance,
                            TStringBuf("order=") + ToString(order) + " rt=" + ToString(rt));
        }
    }
}

// === many-way merge with disjoint key ranges — exactness (Delta stays 0) ===
// Verifies exactness and bucket counts within tolerance of ideal ranks.
Y_UNIT_TEST(ManyWayMergeDisjointRanges) {
    const ui32 B = 10;
    const ui32 M = 32; // 32 parts, each with a disjoint key range
    const ui32 keysPerPart = 100;
    const ui32 perKey = 4;
    const ui64 N = static_cast<ui64>(M) * keysPerPart * perKey;
    auto params = MakeParams(B);

    // Part i covers keys [i*keysPerPart, (i+1)*keysPerPart).
    std::vector<std::vector<TString>> parts(M);
    for (ui32 p = 0; p < M; ++p) {
        for (ui32 k = 0; k < keysPerPart; ++k) {
            TString key = MakeKey(static_cast<i32>(p * keysPerPart + k));
            for (ui32 r = 0; r < perKey; ++r) {
                parts[p].push_back(key);
            }
        }
    }

    std::vector<TEqHeightHistogramBuilder> states;
    for (const auto& part : parts) {
        states.push_back(BuildChunk(part, params));
    }
    auto result = MergeRandom(states, params, 7);

    UNIT_ASSERT_VALUES_EQUAL(result.GetTotalCount(), N);

    UNIT_ASSERT_C(result.IsExact(),
                  "disjoint ranges must stay exact, MaxRankError = "
                      << result.GetMaxRankError());

    auto blob = result.Finalize();
    UNIT_ASSERT(blob.has_value());
    TEqHeightHistogram hist(blob->data(), blob->size());
    UNIT_ASSERT_VALUES_EQUAL(hist.GetTotalCount(), N);

    const ui64 tolerance = N / (4 * B);
    AssertCumulativeVsIdeal(hist, N, B, tolerance, "disjoint");

    std::vector<TString> allKeys;
    allKeys.reserve(static_cast<size_t>(M) * keysPerPart * perKey);
    for (ui32 p = 0; p < M; ++p) {
        for (ui32 k = 0; k < keysPerPart; ++k) {
            TString key = MakeKey(static_cast<i32>(p * keysPerPart + k));
            for (ui32 r = 0; r < perKey; ++r) {
                allKeys.push_back(key);
            }
        }
    }
    AssertTrueRanks(hist, allKeys, hist.GetMaxRankError(), tolerance, "disjoint");
}

// === empty-state identity ===
Y_UNIT_TEST(EmptyStateIdentity) {
    auto params = MakeParams(10);

    TEqHeightHistogramBuilder empty1(params);
    TEqHeightHistogramBuilder empty2(params);
    UNIT_ASSERT(!empty1.Finalize().has_value());
    UNIT_ASSERT(!empty2.Finalize().has_value());

    std::vector<TString> keys = MakeSortedStream(100, 5);
    auto x = BuildFromStream(keys, params);
    auto xBlob = x.Finalize();
    UNIT_ASSERT(xBlob.has_value());

    {
        TEqHeightHistogramBuilder acc(params);
        acc.Merge(x);
        auto accBlob = acc.Finalize();
        UNIT_ASSERT(accBlob.has_value());
        UNIT_ASSERT_VALUES_EQUAL(*xBlob, *accBlob);
    }

    {
        TEqHeightHistogramBuilder acc = BuildFromStream(keys, params);
        TEqHeightHistogramBuilder empty(params);
        acc.Merge(empty);
        auto accBlob = acc.Finalize();
        UNIT_ASSERT(accBlob.has_value());
        UNIT_ASSERT_VALUES_EQUAL(*xBlob, *accBlob);
    }
}

// === merge rejects mismatched NumBuckets / EmissionRate ===
Y_UNIT_TEST(MergeRejectsMismatchedParams) {
    auto a = BuildFromStream(MakeSortedStream(10, 1), MakeParams(10));
    auto b = BuildFromStream(MakeSortedStream(10, 1), MakeParams(20));
    UNIT_ASSERT_EXCEPTION(a.Merge(b), yexception);

    auto p1 = MakeParams(10);
    auto p2 = p1;
    p2.EmissionRate = p1.EmissionRate + 1;
    auto c = BuildFromStream(MakeSortedStream(10, 1), p1);
    auto d = BuildFromStream(MakeSortedStream(10, 1), p2);
    UNIT_ASSERT_EXCEPTION(c.Merge(d), yexception);

    auto p3 = p1;
    p3.MaxStateBytes = p1.MaxStateBytes / 2;
    auto e = BuildFromStream(MakeSortedStream(10, 1), p1);
    auto f = BuildFromStream(MakeSortedStream(10, 1), p3);
    e.Merge(f);
    UNIT_ASSERT_VALUES_EQUAL(e.GetTotalCount(), 20U);
}

// === skew — heavy hitter gets its own bucket ===
Y_UNIT_TEST(Skew) {
    const ui32 B = 20;
    const ui64 N = 1000;
    auto params = MakeParams(B);

    TEqHeightHistogramBuilder b(params);
    for (ui32 i = 0; i < 300; ++i) {
        b.Add(MakeKey(42)); // heavy hitter
    }
    for (ui32 i = 0; i < 700; ++i) {
        b.Add(MakeKey(static_cast<i32>(1000 + i))); // unique keys
    }

    UNIT_ASSERT_VALUES_EQUAL(b.GetTotalCount(), N);

    auto blob = b.Finalize();
    UNIT_ASSERT(blob.has_value());
    TEqHeightHistogram hist(blob->data(), blob->size());
    UNIT_ASSERT_VALUES_EQUAL(hist.GetTotalCount(), N);

    // Heavy hitter key 42 should have its own bucket with count 300.
    TString key42 = MakeKey(42);
    bool found = false;
    for (size_t i = 0; i < hist.GetNumBuckets(); ++i) {
        const auto& bkt = hist.GetBucket(i);
        if (bkt.UpperBound == key42) {
            ui64 prev = (i == 0) ? 0 : hist.GetBucket(i - 1).CumulativeCount;
            ui64 bucketSize = bkt.CumulativeCount - prev;
            UNIT_ASSERT_VALUES_EQUAL(bucketSize, 300);
            found = true;
            break;
        }
    }
    UNIT_ASSERT_C(found, "heavy hitter key not found as a bucket boundary");

    UNIT_ASSERT_C(b.GetMaxRankError() == 0,
                  "sorted skew must be exact (Delta == 0), MaxRankError = "
                      << b.GetMaxRankError());
}

// === duplicate across a group boundary ===
Y_UNIT_TEST(DuplicateAcrossGroupBoundary) {
    const ui32 B = 5;
    const ui64 N = 1000;
    auto params = MakeParams(B);

    // Key 0 repeated 50 times (WeightCap = 25), spanning two groups.
    TEqHeightHistogramBuilder b(params);
    for (ui32 i = 0; i < 50; ++i) {
        b.Add(MakeKey(0));
    }
    for (ui32 i = 1; i < 951; ++i) {
        b.Add(MakeKey(static_cast<i32>(i)));
    }

    UNIT_ASSERT_VALUES_EQUAL(b.GetTotalCount(), N);

    auto blob = b.Finalize();
    UNIT_ASSERT(blob.has_value());
    TEqHeightHistogram hist(blob->data(), blob->size());
    UNIT_ASSERT_VALUES_EQUAL(hist.GetTotalCount(), N);

    for (size_t i = 1; i < hist.GetNumBuckets(); ++i) {
        UNIT_ASSERT_C(hist.GetBucket(i).UpperBound > hist.GetBucket(i - 1).UpperBound,
                      "bucket bounds not strictly increasing at " << i);
    }

    // Mid-key-split regression: the duplicate key (0 repeated 50 times) must
    // not close an entry mid-run.  Sorted input keeps Delta == 0, so IsExact()
    // must hold — a false positive means a boundary fell inside the run.
    UNIT_ASSERT_C(b.IsExact(),
                  "sorted duplicate-across-boundary must be exact (Delta == 0), "
                  "MaxRankError = "
                      << b.GetMaxRankError());
    UNIT_ASSERT_C(hist.IsExact(),
                  "finalized blob must be exact for sorted duplicates, "
                  "MaxRankError = "
                      << hist.GetMaxRankError());
}

// === small domain — 3 distinct keys, B=100, 3 buckets ===
// MIN_ENTRIES fires on BudgetForced, not on small data.
Y_UNIT_TEST(SmallDomain) {
    const ui32 B = 100;
    const ui64 N = 30;
    auto params = MakeParams(B);

    TEqHeightHistogramBuilder b(params);
    for (ui32 i = 0; i < 10; ++i) {
        b.Add(MakeKey(0));
    }
    for (ui32 i = 0; i < 10; ++i) {
        b.Add(MakeKey(1));
    }
    for (ui32 i = 0; i < 10; ++i) {
        b.Add(MakeKey(2));
    }

    auto blob = b.Finalize();
    UNIT_ASSERT_C(blob.has_value(),
                  "small domain must produce a histogram, not nullopt; "
                  "MIN_ENTRIES should only reject budget-starved states");
    TEqHeightHistogram hist(blob->data(), blob->size());
    UNIT_ASSERT_VALUES_EQUAL(hist.GetTotalCount(), N);
    UNIT_ASSERT_VALUES_EQUAL(hist.GetNumBuckets(), 3u);
    UNIT_ASSERT(hist.IsExact());
    UNIT_ASSERT_VALUES_EQUAL(hist.GetBucket(hist.GetNumBuckets() - 1).CumulativeCount, N);
}

// === byte budget — 4 KB keys with small budget ===
Y_UNIT_TEST(ByteBudget) {
    const ui32 B = 10;
    const ui64 N = 1000;
    const ui64 maxBytes = 4096;
    auto params = MakeParams(B, maxBytes);

    TEqHeightHistogramBuilder b(params);
    for (ui32 i = 0; i < N; ++i) {
        TString bigKey(4095, 'x');
        bigKey[0] = static_cast<char>(i & 0xFF);
        bigKey[1] = static_cast<char>((i >> 8) & 0xFF);
        b.Add(bigKey);
    }

    UNIT_ASSERT_VALUES_EQUAL(b.GetTotalCount(), N);

    UNIT_ASSERT_C(b.GetBudgetForced(),
                  "byte budget should have forced compaction (BudgetForced must be set)");

    auto blob = b.Finalize();
    // 4095-byte keys vs SoftBudget=2048: compaction bottoms out at one entry
    // with BudgetForced, so Finalize rejects it (< MIN_ENTRIES).
    UNIT_ASSERT_C(!blob.has_value(),
                  "Finalize should return nullopt: BudgetForced with a single "
                  "entry is rejected (< MIN_ENTRIES)");
}

// === degenerate budget — Finalize returns nullopt ===
Y_UNIT_TEST(DegenerateBudget) {
    const ui32 B = 10;
    const ui64 N = 1000;
    const ui64 maxBytes = 100;
    auto params = MakeParams(B, maxBytes);

    TEqHeightHistogramBuilder b(params);
    for (ui32 i = 0; i < N; ++i) {
        TString key = MakeKey(static_cast<i32>(i));
        b.Add(key);
    }

    UNIT_ASSERT_VALUES_EQUAL(b.GetTotalCount(), N);

    auto blob = b.Finalize();
    UNIT_ASSERT(!blob.has_value());
}

// === degenerate budget — one key exceeds MaxStateBytes ===
Y_UNIT_TEST(OneKeyExceedsBudget) {
    const ui32 B = 10;
    const ui64 N = 100;
    const ui64 maxBytes = 50;
    auto params = MakeParams(B, maxBytes);

    TEqHeightHistogramBuilder b(params);
    TString hugeKey(100, 'z');
    b.Add(hugeKey);
    for (ui32 i = 0; i < N - 1; ++i) {
        b.Add(MakeKey(static_cast<i32>(i)));
    }

    auto blob = b.Finalize();
    UNIT_ASSERT(!blob.has_value());
}

// === round-trip — intermediate state ===
Y_UNIT_TEST(RoundTripIntermediateState) {
    const ui32 B = 20;
    auto params = MakeParams(B);

    auto keys = MakeSortedStream(100, 10);
    auto builder = BuildFromStream(keys, params);

    TString serialized = builder.Serialize();
    TEqHeightHistogramBuilder deserialized(serialized.data(), serialized.size());

    UNIT_ASSERT_VALUES_EQUAL(deserialized.GetTotalCount(), builder.GetTotalCount());
    UNIT_ASSERT_VALUES_EQUAL(deserialized.GetWeightCap(), builder.GetWeightCap());

    const auto& origEntries = builder.GetEntries();
    const auto& deserEntries = deserialized.GetEntries();
    UNIT_ASSERT_VALUES_EQUAL(deserEntries.size(), origEntries.size());

    for (size_t i = 0; i < origEntries.size(); ++i) {
        UNIT_ASSERT_VALUES_EQUAL(deserEntries[i].UpperBound, origEntries[i].UpperBound);
        UNIT_ASSERT_VALUES_EQUAL(deserEntries[i].Weight, origEntries[i].Weight);
        UNIT_ASSERT_VALUES_EQUAL(deserEntries[i].Delta, origEntries[i].Delta);
        UNIT_ASSERT_VALUES_EQUAL(deserEntries[i].SingleKey, origEntries[i].SingleKey);
    }

    auto blob1 = builder.Finalize();
    auto blob2 = deserialized.Finalize();
    UNIT_ASSERT(blob1.has_value());
    UNIT_ASSERT(blob2.has_value());
    UNIT_ASSERT_VALUES_EQUAL(*blob1, *blob2);
}

// === round-trip — final blob ===
Y_UNIT_TEST(RoundTripFinalBlob) {
    const ui32 B = 20;
    const ui64 N = 1000;
    auto params = MakeParams(B);

    auto keys = MakeSortedStream(100, 10);
    auto builder = BuildFromStream(keys, params);
    auto blob = builder.Finalize();
    UNIT_ASSERT(blob.has_value());

    TEqHeightHistogram hist1(blob->data(), blob->size());
    UNIT_ASSERT_VALUES_EQUAL(hist1.GetTotalCount(), N);
    UNIT_ASSERT_VALUES_EQUAL(hist1.GetMaxRankError(), 0);
    UNIT_ASSERT(hist1.IsExact());

    TEqHeightHistogram hist2(blob->data(), blob->size());
    UNIT_ASSERT_VALUES_EQUAL(hist2.GetTotalCount(), hist1.GetTotalCount());
    UNIT_ASSERT_VALUES_EQUAL(hist2.GetNumBuckets(), hist1.GetNumBuckets());
    for (size_t i = 0; i < hist1.GetNumBuckets(); ++i) {
        UNIT_ASSERT_VALUES_EQUAL(hist2.GetBucket(i).UpperBound, hist1.GetBucket(i).UpperBound);
        UNIT_ASSERT_VALUES_EQUAL(hist2.GetBucket(i).CumulativeCount, hist1.GetBucket(i).CumulativeCount);
    }
}

// === EstimateLessOrEqual ===
Y_UNIT_TEST(EstimateLessOrEqual) {
    const ui32 B = 10;
    const ui64 N = 1000;
    auto params = MakeParams(B);

    auto keys = MakeSortedStream(100, 10);
    auto builder = BuildFromStream(keys, params);
    auto blob = builder.Finalize();
    UNIT_ASSERT(blob.has_value());
    TEqHeightHistogram hist(blob->data(), blob->size());

    // Key 50 (true rank 510): bucket boundary at key 49 (cumulative 500), a lower bound within N/B.
    const ui64 trueRank50 = 510;
    ui64 est = hist.EstimateLessOrEqual(MakeKey(50));
    UNIT_ASSERT_C(est <= trueRank50, "estimate " << est << " > true rank " << trueRank50);
    UNIT_ASSERT_C(trueRank50 - est <= N / B,
                  "estimate " << est << " deviates from true rank " << trueRank50
                              << " by " << (trueRank50 - est) << " > N/B " << (N / B));

    UNIT_ASSERT_VALUES_EQUAL(hist.EstimateLessOrEqual(MakeKey(-1)), 0);
    UNIT_ASSERT_VALUES_EQUAL(hist.EstimateLessOrEqual(MakeKey(200)), N);
}

// === string keys — order preservation ===
Y_UNIT_TEST(StringKeys) {
    const ui32 B = 10;
    const ui64 N = 300;
    auto params = MakeParams(B);

    TEqHeightHistogramBuilder b(params);
    // 2-byte keys "XY": X advances every 10, Y cycles within.  Printable ASCII only.
    for (int i = 0; i < 100; ++i) {
        TString key;
        key.push_back(static_cast<char>('a' + (i / 10)));
        key.push_back(static_cast<char>('a' + (i % 10)));
        for (int r = 0; r < 3; ++r) {
            b.Add(key);
        }
    }

    UNIT_ASSERT_VALUES_EQUAL(b.GetTotalCount(), N);
    UNIT_ASSERT(b.IsExact());

    auto blob = b.Finalize();
    UNIT_ASSERT(blob.has_value());
    TEqHeightHistogram hist(blob->data(), blob->size());
    UNIT_ASSERT_VALUES_EQUAL(hist.GetTotalCount(), N);
    UNIT_ASSERT(hist.IsExact());

    // "ea" (index 40, true rank 123): lower bound within N/B.
    const ui64 trueRankEA = 123;
    ui64 est = hist.EstimateLessOrEqual("ea");
    UNIT_ASSERT_C(est <= trueRankEA, "estimate " << est << " > true rank " << trueRankEA);
    UNIT_ASSERT_C(trueRankEA - est <= N / B,
                  "estimate " << est << " deviates from true rank " << trueRankEA
                              << " by " << (trueRankEA - est) << " > N/B " << (N / B));
}

// === variable-length string keys on the sorted path — EntriesBytes underflow ===
// AddSorted must not underflow when a shorter key follows a longer one ("bb" -> "c").
Y_UNIT_TEST(VariableLengthStringKeysSorted) {
    TEqHeightHistogramBuilder::TParams params;
    params.NumBuckets = 10;
    params.EmissionRate = 80;
    params.MaxStateBytes = 4ULL << 20;

    TEqHeightHistogramBuilder b(params);
    // Pad Cap() so the next distinct key extends rather than PushEntry:
    // after 1000 "aaa", Cap = 1000/80 = 12, so "bb" opens a new entry.
    for (int i = 0; i < 1000; ++i) {
        b.Add("aaa");
    }
    b.Add("bb"); // Weight 1, SingleKey
    // "c" is shorter than "bb" and last.Weight < Cap, so this extends.
    // The old add-then-subtract wrapped Bytes to ~2^64 and Compact fused
    // everything to one entry.
    b.Add("c");

    const auto& entries = b.GetEntries();
    UNIT_ASSERT_VALUES_EQUAL(entries.size(), 2U);
    UNIT_ASSERT_VALUES_EQUAL(entries[0].UpperBound, "aaa");
    UNIT_ASSERT_VALUES_EQUAL(entries[0].Weight, 1000U);
    UNIT_ASSERT_VALUES_EQUAL(entries[1].UpperBound, "c");
    UNIT_ASSERT_VALUES_EQUAL(entries[1].Weight, 2U);
    UNIT_ASSERT(!entries[1].SingleKey);

    const ui32 perKey = 10;
    std::vector<TString> rest = {
        "cc",
        "ccc",
        "d",
        "dd",
        "ddd",
        "e",
        "ee",
        "eee",
        "f",
        "ff",
        "fff",
        "g",
        "gg",
        "ggg",
        "h",
    };
    for (const auto& k : rest) {
        for (ui32 r = 0; r < perKey; ++r) {
            b.Add(k);
        }
    }

    const ui64 N = b.GetTotalCount();
    UNIT_ASSERT_C(b.IsExact(), "variable-length sorted string keys must stay exact");

    auto blob = b.Finalize();
    UNIT_ASSERT_C(blob.has_value(),
                  "Finalize must produce a histogram for valid sorted string-keyed data");
    TEqHeightHistogram hist(blob->data(), blob->size());
    UNIT_ASSERT_VALUES_EQUAL(hist.GetTotalCount(), N);
    UNIT_ASSERT(hist.IsExact());

    for (size_t i = 1; i < hist.GetNumBuckets(); ++i) {
        UNIT_ASSERT_C(hist.GetBucket(i).UpperBound > hist.GetBucket(i - 1).UpperBound,
                      "bucket bounds not strictly increasing at " << i);
    }
    UNIT_ASSERT_VALUES_EQUAL(hist.GetBucket(hist.GetNumBuckets() - 1).CumulativeCount, N);
}

// === BudgetForced with enough entries — Finalize produces a usable histogram ===
// Primary path: budget tight but not degenerate (>= MIN_ENTRIES survive).
Y_UNIT_TEST(BudgetForcedEnoughEntries) {
    const ui32 B = 10;
    const ui64 N = 100000;
    // SoftBudget=2048, ~32 bytes/entry -> ~80 before compaction, ~40 survive.
    const ui64 maxBytes = 4096;
    auto params = MakeParams(B, maxBytes);

    TEqHeightHistogramBuilder b(params);
    auto keys = MakeSortedStream(10000, 10);
    for (const auto& k : keys) {
        b.Add(k);
    }

    UNIT_ASSERT_VALUES_EQUAL(b.GetTotalCount(), N);

    UNIT_ASSERT_C(b.GetBudgetForced(),
                  "byte budget should have forced compaction (BudgetForced must be set)");

    UNIT_ASSERT_C(b.GetEntries().size() >= TEqHeightHistogramBuilder::MIN_ENTRIES,
                  "expected >= MIN_ENTRIES entries, got " << b.GetEntries().size());

    auto blob = b.Finalize();
    UNIT_ASSERT_C(blob.has_value(),
                  "Finalize must produce a histogram when BudgetForced but >= MIN_ENTRIES entries survive");

    TEqHeightHistogram hist(blob->data(), blob->size());
    UNIT_ASSERT_VALUES_EQUAL(hist.GetTotalCount(), N);

    // Sorted-path compaction stays exact: Delta_fused = 0.
    UNIT_ASSERT_C(hist.IsExact(),
                  "sorted-path compaction must stay exact (Delta == 0), "
                  "MaxRankError = "
                      << hist.GetMaxRankError());
    UNIT_ASSERT_VALUES_EQUAL(hist.GetMaxRankError(), 0);

    UNIT_ASSERT_VALUES_EQUAL(hist.GetBucket(hist.GetNumBuckets() - 1).CumulativeCount, N);
}

// === round-trip preserves BudgetForced flag ===
// Flags must survive serialization: partial states ship between merge stages.
Y_UNIT_TEST(RoundTripPreservesFlags) {
    const ui32 B = 10;
    const ui64 N = 1000;

    // --- BudgetForced: huge key → single entry → nullopt ---
    {
        const ui64 maxBytes = 50;
        auto params = MakeParams(B, maxBytes);

        TEqHeightHistogramBuilder b(params);
        TString hugeKey(100, 'z');
        b.Add(hugeKey);
        for (ui32 i = 0; i < N - 1; ++i) {
            b.Add(MakeKey(static_cast<i32>(i)));
        }

        UNIT_ASSERT(!b.Finalize().has_value());

        auto rt = RoundTrip(b);
        UNIT_ASSERT_C(!rt.Finalize().has_value(),
                      "BudgetForced flag lost in round-trip: Finalize produced a histogram");
    }

    // --- BudgetForced (< MIN_ENTRIES → nullopt) ---
    {
        const ui64 maxBytes = 200;
        auto params = MakeParams(B, maxBytes);

        TEqHeightHistogramBuilder b(params);
        for (ui32 i = 0; i < N; ++i) {
            b.Add(MakeKey(static_cast<i32>(i)));
        }

        UNIT_ASSERT(!b.Finalize().has_value());

        auto rt = RoundTrip(b);
        UNIT_ASSERT_C(!rt.Finalize().has_value(),
                      "BudgetForced flag lost in round-trip: Finalize produced a histogram");
    }
}

// === byte-path forced fusion in Compact ===
// When no pair is admissible (FusedCost > 2*Cap()), Compact fuses the lightest
// pair regardless.  Verifies compaction terminates and Finalize succeeds.

Y_UNIT_TEST(BytePathForcedFusion) {
    const ui32 B = 10;
    const ui64 N = 10000;
    // SoftBudget=768, ~25 bytes/entry -> ~30 entries max.  10000 rows force
    // fusion; budget sized so >= MIN_ENTRIES survive, else Finalize rejects.
    const ui64 maxBytes = 1536;
    auto params = MakeParams(B, maxBytes);

    TEqHeightHistogramBuilder b(params);
    // Extreme skew: 2 heavy hitters (4000 + 3000) + 3000 light unique keys.
    // Light entries fuse first (admissible), then heavy entries remain
    // (FusedCost > 2*Cap()), forcing FuseLightestPair().
    for (ui32 i = 0; i < 4000; ++i) {
        b.Add(MakeKey(0));
    }
    for (ui32 i = 0; i < 3000; ++i) {
        b.Add(MakeKey(1));
    }
    for (ui32 i = 0; i < 3000; ++i) {
        b.Add(MakeKey(static_cast<i32>(100 + i)));
    }

    UNIT_ASSERT_VALUES_EQUAL(b.GetTotalCount(), N);

    const auto& entries = b.GetEntries();
    UNIT_ASSERT_C(!entries.empty(), "compaction should leave at least one entry");

    UNIT_ASSERT_C(b.GetBudgetForced(),
                  "byte budget should have forced compaction (BudgetForced must be set)");

    auto blob = b.Finalize();
    UNIT_ASSERT_C(blob.has_value(),
                  "Finalize must produce a histogram after byte-path forced fusion");

    TEqHeightHistogram hist(blob->data(), blob->size());
    UNIT_ASSERT_VALUES_EQUAL(hist.GetTotalCount(), N);

    // Sorted-path forced fusion stays exact: Delta_fused = 0.
    UNIT_ASSERT_C(hist.IsExact(),
                  "sorted-path forced fusion must stay exact (Delta == 0), "
                  "MaxRankError = "
                      << hist.GetMaxRankError());
    UNIT_ASSERT_VALUES_EQUAL(hist.GetMaxRankError(), 0);

    UNIT_ASSERT_VALUES_EQUAL(hist.GetBucket(hist.GetNumBuckets() - 1).CumulativeCount, N);
}

// === malformed blob — EmissionRate == 0 must be rejected ===
// EmissionRate is a divisor in Cap(); zero must throw, not SIGFPE later.
Y_UNIT_TEST(MalformedBlobZeroEmissionRate) {
    const ui32 B = 10;
    auto params = MakeParams(B);

    auto keys = MakeSortedStream(100, 10);
    auto builder = BuildFromStream(keys, params);
    TString blob = builder.Serialize();

    // Zero EmissionRate at byte offset 6: version(1) flags(1) NumBuckets(4) EmissionRate(4)...
    UNIT_ASSERT_VALUES_EQUAL(blob[0], static_cast<char>(1)); // version
    UNIT_ASSERT(blob.size() >= static_cast<size_t>(6) + sizeof(ui32));
    TString corrupted = blob;
    ui32 zero = 0;
    std::memcpy(corrupted.begin() + 6, &zero, sizeof(ui32));

    UNIT_ASSERT_EXCEPTION(TEqHeightHistogramBuilder(corrupted.data(), corrupted.size()), yexception);
}

// === unsorted input forces fusion — Delta machinery exercised ===
// Delta accrues only when a straddling entry spans multiple keys (!SingleKey).
// Unsorted input under a tight budget produces Delta > 0.
Y_UNIT_TEST(UnsortedFusionProducesDelta) {
    const ui32 B = 10;
    const ui32 numKeys = 20000;
    const ui64 N = numKeys;
    const ui64 maxBytes = 64 * 1024;
    auto params = MakeParams(B, maxBytes);

    std::mt19937 rng(7);
    auto keys = MakeSortedStream(numKeys, 1);
    std::shuffle(keys.begin(), keys.end(), rng);

    auto builder = BuildFromStream(keys, params);
    UNIT_ASSERT_VALUES_EQUAL(builder.GetTotalCount(), N);

    const auto& entries = builder.GetEntries();
    UNIT_ASSERT_C(entries.size() < numKeys,
                  "expected fusion under a tight budget, got " << entries.size()
                                                               << " entries for " << numKeys << " keys");

    // At least one multi-key entry (the only way Delta accrues).
    bool hasMultiKey = false;
    for (const auto& e : entries) {
        if (!e.SingleKey) {
            hasMultiKey = true;
            break;
        }
    }
    UNIT_ASSERT_C(hasMultiKey,
                  "expected at least one multi-key entry after fusion, "
                  "but all entries have SingleKey == true");

    // Delta must be nonzero somewhere.
    bool hasDelta = false;
    for (const auto& e : entries) {
        if (e.Delta > 0) {
            hasDelta = true;
            break;
        }
    }
    UNIT_ASSERT_C(hasDelta,
                  "expected Delta > 0 after unsorted fusion, but all entries "
                  "have Delta == 0 — the approximation mechanism is unexercised");

    const ui64 maxRankError = builder.GetMaxRankError();
    UNIT_ASSERT_C(maxRankError > 0,
                  "expected MaxRankError > 0 when Delta > 0, got 0");
    UNIT_ASSERT_C(maxRankError <= N / B,
                  "MaxRankError " << maxRankError << " > total/B " << (N / B));

    auto blob = builder.Finalize();
    UNIT_ASSERT(blob.has_value());
    TEqHeightHistogram hist(blob->data(), blob->size());
    UNIT_ASSERT_VALUES_EQUAL(hist.GetTotalCount(), N);
    UNIT_ASSERT_C(!hist.IsExact(),
                  "unsorted fusion must be approximate, not exact");

    AssertTrueRanks(hist, keys, hist.GetMaxRankError(), N / (4 * B), "UnsortedFusionProducesDelta");
}

// === Finalize guard rail — rejects inexact small tables ===
// Guard: maxRankError > total/b.  With total < b, total/b == 0, so any
// nonzero error is rejected — exactness required on small tables.
Y_UNIT_TEST(FinalizeGuardRailSmallTable) {
    const ui32 B = 100;
    // 40 rows, 40 distinct keys, B=100: total/b == 0, so any error rejects.
    const ui32 numKeys = 40;
    const ui64 N = numKeys;
    const ui64 maxBytes = 256;
    auto params = MakeParams(B, maxBytes);

    std::mt19937 rng(31);
    auto keys = MakeSortedStream(numKeys, 1);
    std::shuffle(keys.begin(), keys.end(), rng);

    auto builder = BuildFromStream(keys, params);
    UNIT_ASSERT_VALUES_EQUAL(builder.GetTotalCount(), N);

    UNIT_ASSERT_C(builder.GetBudgetForced(),
                  "byte budget should have forced compaction");

    // A small table under a tight budget is rejected by Finalize — either
    // the guard rail (maxRankError > total/b == 0) or the BudgetForced +
    // too-few-entries check.  Either way, no histogram.
    auto blob = builder.Finalize();
    UNIT_ASSERT_C(!blob.has_value(),
                  "small inexact table under tight budget must not finalize");
}

// Finalize guard rail: corrupt Delta to exceed total/b, verify Finalize rejects.
Y_UNIT_TEST(FinalizeGuardRailLargeError) {
    const ui32 B = 10;
    const ui32 numKeys = 20;
    const ui32 perKey = 50;
    const ui64 N = static_cast<ui64>(numKeys) * perKey;
    auto params = MakeParams(B);

    auto keys = MakeSortedStream(numKeys, perKey);
    auto builder = BuildFromStream(keys, params);
    UNIT_ASSERT_VALUES_EQUAL(builder.GetTotalCount(), N);
    UNIT_ASSERT_C(!builder.GetBudgetForced(), "BudgetForced must be false");
    UNIT_ASSERT_VALUES_EQUAL(builder.GetEntries().size(), numKeys);

    TString blob = builder.Serialize();

    // Overwrite every entry's Delta in the serialized blob.
    TString corrupted = blob;
    char* base = corrupted.begin();
    const char* p = base;
    p += 1 + 1 + 4 + 4 + 8 + 8; // version..TotalCount
    ui32 mkLen;
    std::memcpy(&mkLen, p, sizeof(ui32));
    p += sizeof(ui32) + mkLen + 4; // MinKey + entryCount
    ui32 entryCount;
    std::memcpy(&entryCount, p - sizeof(ui32), sizeof(ui32));
    UNIT_ASSERT_VALUES_EQUAL(entryCount, numKeys);

    const ui64 totalOverB = N / B;
    const ui64 largeDelta = totalOverB + 50;
    for (ui32 i = 0; i < entryCount; ++i) {
        ui32 ubLen;
        std::memcpy(&ubLen, p, sizeof(ui32));
        p += sizeof(ui32) + ubLen + 8; // UpperBound + Weight
        size_t deltaOffset = p - base;
        std::memcpy(base + deltaOffset, &largeDelta, sizeof(ui64));
        p += 8 + 1; // Delta + SingleKey
    }

    TEqHeightHistogramBuilder result(corrupted.data(), corrupted.size());
    UNIT_ASSERT_VALUES_EQUAL(result.GetTotalCount(), N);
    UNIT_ASSERT_C(!result.GetBudgetForced(), "BudgetForced must be false");
    const ui32 entryThreshold = std::max<ui32>(TEqHeightHistogramBuilder::MIN_ENTRIES, B);
    UNIT_ASSERT_C(result.GetEntries().size() >= entryThreshold,
                  "need >= " << entryThreshold << " entries, got " << result.GetEntries().size());
    UNIT_ASSERT_C(result.GetMaxRankError() > totalOverB,
                  "need MaxRankError > total/b = " << totalOverB
                                                   << ", got " << result.GetMaxRankError());

    auto finalized = result.Finalize();
    UNIT_ASSERT_C(!finalized.has_value(),
                  "guard rail must reject: MaxRankError " << result.GetMaxRankError()
                                                          << " > total/b " << totalOverB);
}

// === malformed blobs — TEqHeightHistogramBuilder deserializer ===
Y_UNIT_TEST(MalformedBlobBuilder) {
    const ui32 B = 10;
    auto params = MakeParams(B);
    auto keys = MakeSortedStream(100, 10);
    auto builder = BuildFromStream(keys, params);
    TString blob = builder.Serialize();

    UNIT_ASSERT_EXCEPTION(TEqHeightHistogramBuilder(nullptr, blob.size()), yexception);
    UNIT_ASSERT_EXCEPTION(TEqHeightHistogramBuilder(nullptr, 0), yexception);

    // Truncated: cut off the last byte.
    {
        TString truncated(blob.data(), blob.size() - 1);
        UNIT_ASSERT_EXCEPTION(TEqHeightHistogramBuilder(truncated.data(), truncated.size()), yexception);
    }

    // Trailing bytes: append garbage.
    {
        TString trailing = blob + TString("\xFF\xFF\xFF\xFF");
        UNIT_ASSERT_EXCEPTION(TEqHeightHistogramBuilder(trailing.data(), trailing.size()), yexception);
    }

    // Zero Weight: corrupt the first entry's Weight field to 0.
    // Layout: version(1) flags(1) NumBuckets(4) EmissionRate(4) MaxStateBytes(8)
    //         TotalCount(8) MinKey(4+len) entryCount(4)
    //         entry[0]: UpperBound(4+len) Weight(8) Delta(8) SingleKey(1)
    {
        TString corrupted = blob;
        const char* p = corrupted.data();
        p += 1 + 1 + 4 + 4 + 8 + 8; // version..TotalCount
        ui32 mkLen;
        std::memcpy(&mkLen, p, sizeof(ui32));
        p += sizeof(ui32) + mkLen + 4; // MinKey + entryCount
        ui32 ubLen;
        std::memcpy(&ubLen, p, sizeof(ui32));
        p += sizeof(ui32) + ubLen; // entry[0] UpperBound
        size_t weightOffset = p - corrupted.data();
        ui64 zero = 0;
        std::memcpy(corrupted.begin() + weightOffset, &zero, sizeof(ui64));
        UNIT_ASSERT_EXCEPTION(TEqHeightHistogramBuilder(corrupted.data(), corrupted.size()), yexception);
    }

    // Non-increasing UpperBound: overwrite entry[1]'s bound with entry[0]'s.
    {
        TString corrupted = blob;
        const char* p = corrupted.data();
        p += 1 + 1 + 4 + 4 + 8 + 8; // version..TotalCount
        ui32 mkLen;
        std::memcpy(&mkLen, p, sizeof(ui32));
        p += sizeof(ui32) + mkLen + 4; // MinKey + entryCount
        // entry[0] UpperBound
        ui32 ub0Len;
        std::memcpy(&ub0Len, p, sizeof(ui32));
        TString ub0(p + sizeof(ui32), ub0Len);
        p += sizeof(ui32) + ub0Len + 8 + 8 + 1; // UpperBound + Weight + Delta + SingleKey
        // entry[1] UpperBound
        size_t ub1Offset = p - corrupted.data();
        ui32 ub1Len;
        std::memcpy(&ub1Len, p, sizeof(ui32));
        UNIT_ASSERT_VALUES_EQUAL(ub1Len, ub0Len); // all MakeKey keys are same length
        std::memcpy(corrupted.begin() + ub1Offset + sizeof(ui32), ub0.data(), ub0Len);
        UNIT_ASSERT_EXCEPTION(TEqHeightHistogramBuilder(corrupted.data(), corrupted.size()), yexception);
    }

    // Weight sum != TotalCount: inflate the first entry's Weight.
    {
        TString corrupted = blob;
        const char* p = corrupted.data();
        p += 1 + 1 + 4 + 4 + 8 + 8; // version..TotalCount
        ui32 mkLen;
        std::memcpy(&mkLen, p, sizeof(ui32));
        p += sizeof(ui32) + mkLen + 4; // MinKey + entryCount
        ui32 ubLen;
        std::memcpy(&ubLen, p, sizeof(ui32));
        p += sizeof(ui32) + ubLen; // entry[0] UpperBound
        size_t weightOffset = p - corrupted.data();
        ui64 huge = 1000000000000ULL;
        std::memcpy(corrupted.begin() + weightOffset, &huge, sizeof(ui64));
        UNIT_ASSERT_EXCEPTION(TEqHeightHistogramBuilder(corrupted.data(), corrupted.size()), yexception);
    }

    // MinKey > first UpperBound: suppresses InterleaveInto bumps on merge.
    {
        TString corrupted = blob;
        const char* p = corrupted.data();
        p += 1 + 1 + 4 + 4 + 8 + 8; // version..TotalCount
        ui32 mkLen;
        std::memcpy(&mkLen, p, sizeof(ui32));
        UNIT_ASSERT_VALUES_EQUAL(mkLen, 4u);
        const size_t mkOffset = (p - corrupted.data()) + sizeof(ui32);
        std::memset(corrupted.begin() + mkOffset, '\xFF', mkLen);
        UNIT_ASSERT_EXCEPTION(TEqHeightHistogramBuilder(corrupted.data(), corrupted.size()), yexception);
    }
}

// === malformed blobs — TEqHeightHistogram deserializer ===
Y_UNIT_TEST(MalformedBlobHistogram) {
    const ui32 B = 10;
    auto params = MakeParams(B);
    auto keys = MakeSortedStream(100, 10);
    auto builder = BuildFromStream(keys, params);
    auto blobOpt = builder.Finalize();
    UNIT_ASSERT(blobOpt.has_value());
    TString blob = *blobOpt;

    UNIT_ASSERT_EXCEPTION(TEqHeightHistogram(nullptr, blob.size()), yexception);
    UNIT_ASSERT_EXCEPTION(TEqHeightHistogram(nullptr, 0), yexception);

    // Truncated: cut off the last byte.
    {
        TString truncated(blob.data(), blob.size() - 1);
        UNIT_ASSERT_EXCEPTION(TEqHeightHistogram(truncated.data(), truncated.size()), yexception);
    }

    // Trailing bytes: append garbage.
    {
        TString trailing = blob + TString("\xFF\xFF\xFF\xFF");
        UNIT_ASSERT_EXCEPTION(TEqHeightHistogram(trailing.data(), trailing.size()), yexception);
    }

    // Zero buckets: corrupt numBuckets to 0.
    // Layout: version(1) numBuckets(4) TotalCount(8) MaxRankError(8) buckets...
    {
        TString corrupted = blob;
        ui32 zero = 0;
        std::memcpy(corrupted.begin() + 1, &zero, sizeof(ui32)); // offset 1 = numBuckets
        UNIT_ASSERT_EXCEPTION(TEqHeightHistogram(corrupted.data(), corrupted.size()), yexception);
    }

    // Cumulative count exceeds total: corrupt the first bucket's CumulativeCount.
    {
        TString corrupted = blob;
        const char* p = corrupted.data();
        p += 1 + 4 + 8 + 8; // skip to first bucket
        ui32 ubLen;
        std::memcpy(&ubLen, p, sizeof(ui32));
        p += sizeof(ui32) + ubLen; // UpperBound
        size_t ccOffset = p - corrupted.data();
        ui64 huge = 999999999ULL;
        std::memcpy(corrupted.begin() + ccOffset, &huge, sizeof(ui64));
        UNIT_ASSERT_EXCEPTION(TEqHeightHistogram(corrupted.data(), corrupted.size()), yexception);
    }

    // Last cumulative != total: corrupt the last bucket's CumulativeCount.
    {
        TString corrupted = blob;
        const char* p = corrupted.data();
        p += 1; // version
        ui32 numBuckets;
        std::memcpy(&numBuckets, p, sizeof(ui32));
        p += 4 + 8 + 8; // numBuckets, TotalCount, MaxRankError
        for (ui32 i = 0; i < numBuckets - 1; ++i) {
            ui32 ubLen;
            std::memcpy(&ubLen, p, sizeof(ui32));
            p += sizeof(ui32) + ubLen + 8; // UpperBound + CumulativeCount
        }
        // Last bucket: UpperBound(4+len) then CumulativeCount(8)
        ui32 ubLen;
        std::memcpy(&ubLen, p, sizeof(ui32));
        p += sizeof(ui32) + ubLen;
        size_t ccOffset = p - corrupted.data();
        ui64 wrong = 1; // != TotalCount (1000)
        std::memcpy(corrupted.begin() + ccOffset, &wrong, sizeof(ui64));
        UNIT_ASSERT_EXCEPTION(TEqHeightHistogram(corrupted.data(), corrupted.size()), yexception);
    }

    // Non-increasing UpperBound: overwrite bucket[1]'s bound with bucket[0]'s.
    {
        TString corrupted = blob;
        const char* p = corrupted.data();
        p += 1 + 4 + 8 + 8; // skip to first bucket
        ui32 ub0Len;
        std::memcpy(&ub0Len, p, sizeof(ui32));
        TString ub0(p + sizeof(ui32), ub0Len);
        p += sizeof(ui32) + ub0Len + 8; // UpperBound + CumulativeCount
        size_t ub1Offset = p - corrupted.data();
        ui32 ub1Len;
        std::memcpy(&ub1Len, p, sizeof(ui32));
        UNIT_ASSERT_VALUES_EQUAL(ub1Len, ub0Len); // all MakeKey keys are same length
        std::memcpy(corrupted.begin() + ub1Offset + sizeof(ui32), ub0.data(), ub0Len);
        UNIT_ASSERT_EXCEPTION(TEqHeightHistogram(corrupted.data(), corrupted.size()), yexception);
    }

    // Non-increasing cumulative: copy bucket[0]'s count onto bucket[1].
    {
        TString corrupted = blob;
        const char* p = corrupted.data();
        p += 1 + 4 + 8 + 8; // skip to first bucket
        ui32 ub0Len;
        std::memcpy(&ub0Len, p, sizeof(ui32));
        p += sizeof(ui32) + ub0Len; // UpperBound
        ui64 cc0;
        std::memcpy(&cc0, p, sizeof(ui64));
        p += sizeof(ui64);
        ui32 ub1Len;
        std::memcpy(&ub1Len, p, sizeof(ui32));
        p += sizeof(ui32) + ub1Len;
        size_t cc1Offset = p - corrupted.data();
        std::memcpy(corrupted.begin() + cc1Offset, &cc0, sizeof(ui64));
        UNIT_ASSERT_EXCEPTION(TEqHeightHistogram(corrupted.data(), corrupted.size()), yexception);
    }
}

// === wide-key usable byte budget — per-Add invariant ===
// 4 KB keys with a budget large enough to keep the state usable.
// Asserts the serialized state fits MaxStateBytes and whole-key boundaries.
Y_UNIT_TEST(WideKeyUsableByteBudget) {
    const ui32 B = 10;
    const ui64 N = 100;
    // Budget for >= MIN_ENTRIES (16) entries of 4 KB keys after compaction.
    // Each entry ~4120 bytes; 16 need ~66 KB + header, so 256 KB is ample.
    const ui64 maxBytes = 256 * 1024;
    auto params = MakeParams(B, maxBytes);

    TEqHeightHistogramBuilder b(params);
    for (ui32 i = 0; i < N; ++i) {
        TString bigKey(4090, 'x');
        bigKey[0] = static_cast<char>(i & 0xFF);
        bigKey[1] = static_cast<char>((i >> 8) & 0xFF);
        b.Add(bigKey);
    }

    UNIT_ASSERT_VALUES_EQUAL(b.GetTotalCount(), N);

    UNIT_ASSERT_C(b.Serialize().size() <= maxBytes,
                  "serialized state " << b.Serialize().size() << " > MaxStateBytes " << maxBytes);

    auto blob = b.Finalize();
    UNIT_ASSERT_C(blob.has_value(),
                  "wide-key state with sufficient budget must finalize, not nullopt");

    TEqHeightHistogram hist(blob->data(), blob->size());
    UNIT_ASSERT_VALUES_EQUAL(hist.GetTotalCount(), N);

    // Every bucket boundary must be a whole key (4 KB + 2 byte prefix).
    for (size_t i = 0; i < hist.GetNumBuckets(); ++i) {
        const auto& bkt = hist.GetBucket(i);
        UNIT_ASSERT_C(bkt.UpperBound.size() >= 4090,
                      "bucket " << i << " UpperBound truncated: size "
                                << bkt.UpperBound.size() << " < 4090");
    }
}

// === MinKey round-trip ===
// MinKey must survive round-trip; a wrong MinKey mischarges Delta in Merge.
Y_UNIT_TEST(MinKeyRoundTrip) {
    const ui32 B = 20;
    auto params = MakeParams(B);

    auto keys = MakeSortedStream(100, 10);
    auto builder = BuildFromStream(keys, params);

    TStringBuf origMinKey = builder.GetMinKey();
    UNIT_ASSERT_C(!origMinKey.empty(), "MinKey must be set after adding keys");

    TString serialized = builder.Serialize();
    TEqHeightHistogramBuilder deserialized(serialized.data(), serialized.size());

    UNIT_ASSERT_VALUES_EQUAL(deserialized.GetMinKey(), origMinKey);

    // After merging two non-adjacent states, MinKey must be the smaller.
    TEqHeightHistogramBuilder a(params);
    for (ui32 i = 50; i < 100; ++i) {
        a.Add(MakeKey(static_cast<i32>(i)));
    }
    TEqHeightHistogramBuilder b(params);
    for (ui32 i = 0; i < 50; ++i) {
        b.Add(MakeKey(static_cast<i32>(i)));
    }
    a.Merge(b);
    UNIT_ASSERT_VALUES_EQUAL(a.GetMinKey(), MakeKey(0));
}

// === Delta > 0 merge ===
// Build M states under a tight budget so each carries fused multi-key entries,
// merge in several orders, and assert true-rank error is non-zero and
// <= GetMaxRankError().
Y_UNIT_TEST(MergeProducesDelta) {
    const ui32 B = 10;
    const ui32 M = 16;
    const ui32 keysPerPart = 2000;
    const ui32 perKey = 1;
    const ui64 N = static_cast<ui64>(M) * keysPerPart * perKey;
    // Tight budget: forces fusion so entries become multi-key.
    const ui64 maxBytes = 8 * 1024;
    auto params = MakeParams(B, maxBytes);

    // Each part covers a disjoint key range, shuffled within the part.
    std::mt19937 rng(42);
    std::vector<std::vector<TString>> parts(M);
    for (ui32 p = 0; p < M; ++p) {
        for (ui32 k = 0; k < keysPerPart; ++k) {
            parts[p].push_back(MakeKey(static_cast<i32>(p * keysPerPart + k)));
        }
        std::shuffle(parts[p].begin(), parts[p].end(), rng);
    }

    // Build all keys for AssertTrueRanks.
    std::vector<TString> allKeys;
    allKeys.reserve(static_cast<size_t>(N));
    for (ui32 p = 0; p < M; ++p) {
        for (ui32 k = 0; k < keysPerPart; ++k) {
            allKeys.push_back(MakeKey(static_cast<i32>(p * keysPerPart + k)));
        }
    }

    auto buildStates = [&] {
        std::vector<TEqHeightHistogramBuilder> states;
        for (const auto& part : parts) {
            states.push_back(BuildChunk(part, params));
        }
        return states;
    };

    // Merge in multiple orders and check each.
    for (auto order : {0, 1, 2}) { // 0=linear, 1=balanced, 2=random
        std::vector<TEqHeightHistogramBuilder> states = buildStates();
        TEqHeightHistogramBuilder result(params);
        if (order == 0) {
            result = MergeLinear(std::move(states), params);
        } else if (order == 1) {
            result = MergeBalanced(std::move(states), params);
        } else {
            result = MergeRandom(std::move(states), params, 7);
        }

        UNIT_ASSERT_VALUES_EQUAL(result.GetTotalCount(), N);

        const ui64 maxRankError = result.GetMaxRankError();

        // Delta must be non-zero: the tight budget fuses entries into
        // multi-key ones, and merging interleaves them at frontiers.
        UNIT_ASSERT_C(maxRankError > 0,
                      "order " << order << ": expected MaxRankError > 0 from "
                               << "merged fused states, got 0");

        auto blob = result.Finalize();
        UNIT_ASSERT_C(blob.has_value(),
                      "order " << order << ": Finalize must produce a histogram");
        TEqHeightHistogram hist(blob->data(), blob->size());
        UNIT_ASSERT_VALUES_EQUAL(hist.GetTotalCount(), N);

        const ui64 tolerance = N / (4 * B);
        AssertTrueRanks(hist, allKeys, hist.GetMaxRankError(), tolerance,
                        TStringBuf("MergeProducesDelta order=") + ToString(order));
    }
}

// === merge-count sweep with overlapping ranges ===
// Sweep M in {2, 8, 32, 128} with overlapping ranges and multi-key entries.
// Disjoint ranges produce Delta == 0, so overlapping + tight budgets are
// needed for the sweep to have teeth.
Y_UNIT_TEST(MergeCountSweepOverlapping) {
    const ui32 B = 10;
    const ui32 keysPerPart = 500;
    const ui32 perKey = 2;
    const ui64 maxBytes = 4 * 1024;
    auto params = MakeParams(B, maxBytes);

    ui64 firstErr = 0;
    ui64 firstN = 0;

    for (ui32 M : {2u, 8u, 32u, 128u}) {
        const ui64 N = static_cast<ui64>(M) * keysPerPart * perKey;
        std::mt19937 rng(M * 17 + 3);

        // Overlapping parts: each part draws from the full key range [0, keysPerPart).
        std::vector<std::vector<TString>> parts(M);
        for (ui32 p = 0; p < M; ++p) {
            for (ui32 k = 0; k < keysPerPart; ++k) {
                for (ui32 r = 0; r < perKey; ++r) {
                    parts[p].push_back(MakeKey(static_cast<i32>(k)));
                }
            }
            std::shuffle(parts[p].begin(), parts[p].end(), rng);
        }

        std::vector<TEqHeightHistogramBuilder> states;
        for (const auto& part : parts) {
            states.push_back(BuildChunk(part, params));
        }
        auto result = MergeRandom(std::move(states), params, M);

        UNIT_ASSERT_VALUES_EQUAL_C(result.GetTotalCount(), N, "M=" << M);

        const ui64 maxRankError = result.GetMaxRankError();
        // Honest bound: Finalize's guard rail, not 2*Cap() (fusion does not
        // enforce that). Frozen-cap regression: error/N stays comparable
        // across M (a cap frozen at the first state's size blows up).
        UNIT_ASSERT_C(maxRankError <= N / B,
                      "M=" << M << ": MaxRankError " << maxRankError
                           << " > total/B " << (N / B));
        if (M == 2) {
            firstErr = maxRankError;
            firstN = N;
            UNIT_ASSERT_C(firstErr > 0, "M=2 must produce Delta so the ratio check has a baseline");
        } else {
            UNIT_ASSERT_C(static_cast<unsigned __int128>(maxRankError) * firstN <=
                              static_cast<unsigned __int128>(4) * firstErr * N,
                          "M=" << M << ": error/N " << maxRankError << "/" << N
                               << " > 4x M=2 " << firstErr << "/" << firstN);
        }

        auto blob = result.Finalize();
        UNIT_ASSERT_C(blob.has_value(), "M=" << M << ": Finalize must produce a histogram");
        TEqHeightHistogram hist(blob->data(), blob->size());
        UNIT_ASSERT_VALUES_EQUAL_C(hist.GetTotalCount(), N, "M=" << M);

        // Build all keys for AssertTrueRanks.
        std::vector<TString> allKeys;
        allKeys.reserve(static_cast<size_t>(N));
        for (ui32 p = 0; p < M; ++p) {
            for (ui32 k = 0; k < keysPerPart; ++k) {
                for (ui32 r = 0; r < perKey; ++r) {
                    allKeys.push_back(MakeKey(static_cast<i32>(k)));
                }
            }
        }
        const ui64 tolerance = N / (4 * B);
        AssertTrueRanks(hist, allKeys, hist.GetMaxRankError(), tolerance,
                        TStringBuf("sweep M=") + ToString(M));
    }
}

// === golden blob — pins the serialized format ===
// Hardcoded finalized blob with known values; catches layout changes.
Y_UNIT_TEST(GoldenBlob) {
    // Build a small, deterministic histogram: 20 keys, 2 per key, B=4.
    const ui32 B = 4;
    const ui32 numKeys = 20;
    const ui32 perKey = 2;
    const ui64 N = static_cast<ui64>(numKeys) * perKey;
    auto params = MakeParams(B);
    auto keys = MakeSortedStream(numKeys, perKey);
    auto builder = BuildFromStream(keys, params);
    auto blob = builder.Finalize();
    UNIT_ASSERT(blob.has_value());

    // Parse and verify known properties.
    TEqHeightHistogram hist(blob->data(), blob->size());
    UNIT_ASSERT_VALUES_EQUAL(hist.GetTotalCount(), N);
    UNIT_ASSERT_VALUES_EQUAL(hist.GetMaxRankError(), 0);
    UNIT_ASSERT(hist.IsExact());
    UNIT_ASSERT_VALUES_EQUAL(hist.GetNumBuckets(), B);

    // The blob must be deterministic: re-build and compare.
    auto builder2 = BuildFromStream(keys, params);
    auto blob2 = builder2.Finalize();
    UNIT_ASSERT(blob2.has_value());
    UNIT_ASSERT_VALUES_EQUAL(*blob, *blob2);

    // Hardcoded golden blob: 4 buckets, 40 rows, exact (MaxRankError=0).
    // Layout: version(1) numBuckets(4) TotalCount(8) MaxRankError(8)
    //         bucket[i]: UpperBoundLen(4) UpperBound(4) CumulativeCount(8)
    // Each bucket holds 10 rows; UpperBounds are MakeKey(4), MakeKey(9), MakeKey(14), MakeKey(19).
    const TString expectedBlob = TString()
                                     .append("\x01", 1)                              // version
                                     .append("\x04\x00\x00\x00", 4)                  // numBuckets = 4
                                     .append("\x28\x00\x00\x00\x00\x00\x00\x00", 8)  // TotalCount = 40
                                     .append("\x00\x00\x00\x00\x00\x00\x00\x00", 8)  // MaxRankError = 0
                                     .append("\x04\x00\x00\x00\x80\x00\x00\x04", 8)  // bucket 0: UB=MakeKey(4)
                                     .append("\x0a\x00\x00\x00\x00\x00\x00\x00", 8)  // CC=10
                                     .append("\x04\x00\x00\x00\x80\x00\x00\x09", 8)  // bucket 1: UB=MakeKey(9)
                                     .append("\x14\x00\x00\x00\x00\x00\x00\x00", 8)  // CC=20
                                     .append("\x04\x00\x00\x00\x80\x00\x00\x0e", 8)  // bucket 2: UB=MakeKey(14)
                                     .append("\x1e\x00\x00\x00\x00\x00\x00\x00", 8)  // CC=30
                                     .append("\x04\x00\x00\x00\x80\x00\x00\x13", 8)  // bucket 3: UB=MakeKey(19)
                                     .append("\x28\x00\x00\x00\x00\x00\x00\x00", 8); // CC=40
    UNIT_ASSERT_VALUES_EQUAL(*blob, expectedBlob);

    // Round-trip through the builder's intermediate format.
    TString state = builder.Serialize();
    TEqHeightHistogramBuilder rt(state.data(), state.size());
    auto rtBlob = rt.Finalize();
    UNIT_ASSERT(rtBlob.has_value());
    UNIT_ASSERT_VALUES_EQUAL(*rtBlob, *blob);
}

// === golden intermediate state — pins the Serialize() wire format ===
Y_UNIT_TEST(GoldenIntermediateState) {
    auto params = MakeParams(1);
    TEqHeightHistogramBuilder b(params);
    b.Add(MakeKey(0));
    TString state = b.Serialize();

    const TString expected = TString()
                                 .append("\x01", 1)                             // version
                                 .append("\x00", 1)                             // flags
                                 .append("\x01\x00\x00\x00", 4)                 // NumBuckets = 1
                                 .append("\x08\x00\x00\x00", 4)                 // EmissionRate = 8
                                 .append("\x00\x00\x40\x00\x00\x00\x00\x00", 8) // MaxStateBytes = 4MiB
                                 .append("\x01\x00\x00\x00\x00\x00\x00\x00", 8) // TotalCount = 1
                                 .append("\x04\x00\x00\x00\x80\x00\x00\x00", 8) // MinKey = MakeKey(0)
                                 .append("\x01\x00\x00\x00", 4)                 // entryCount = 1
                                 .append("\x04\x00\x00\x00\x80\x00\x00\x00", 8) // UpperBound = MakeKey(0)
                                 .append("\x01\x00\x00\x00\x00\x00\x00\x00", 8) // Weight = 1
                                 .append("\x00\x00\x00\x00\x00\x00\x00\x00", 8) // Delta = 0
                                 .append("\x01", 1);                            // SingleKey
    UNIT_ASSERT_VALUES_EQUAL(state, expected);

    TEqHeightHistogramBuilder rt(state.data(), state.size());
    UNIT_ASSERT_VALUES_EQUAL(rt.GetTotalCount(), 1u);
    UNIT_ASSERT_VALUES_EQUAL(rt.GetMinKey(), MakeKey(0));
    UNIT_ASSERT_VALUES_EQUAL(rt.GetEntries().size(), 1u);
    UNIT_ASSERT_VALUES_EQUAL(rt.GetEntries()[0].Weight, 1u);
    UNIT_ASSERT(rt.GetEntries()[0].SingleKey);
}

// === TEqHeightHistogram copyability — smoke test ===
Y_UNIT_TEST(HistogramCopyable) {
    const ui32 B = 10;
    const ui64 N = 1000;
    auto params = MakeParams(B);
    auto keys = MakeSortedStream(N / 10, 10);
    auto builder = BuildFromStream(keys, params);
    auto blob = builder.Finalize();
    UNIT_ASSERT(blob.has_value());

    TEqHeightHistogram orig(blob->data(), blob->size());
    TEqHeightHistogram copy = orig; // copy ctor

    UNIT_ASSERT_VALUES_EQUAL(copy.GetTotalCount(), orig.GetTotalCount());
    UNIT_ASSERT_VALUES_EQUAL(copy.GetMaxRankError(), orig.GetMaxRankError());
    UNIT_ASSERT_VALUES_EQUAL(copy.GetNumBuckets(), orig.GetNumBuckets());
    for (size_t i = 0; i < orig.GetNumBuckets(); ++i) {
        UNIT_ASSERT_VALUES_EQUAL(copy.GetBucket(i).UpperBound, orig.GetBucket(i).UpperBound);
        UNIT_ASSERT_VALUES_EQUAL(copy.GetBucket(i).CumulativeCount, orig.GetBucket(i).CumulativeCount);
    }

    // EstimateLessOrEqual must work on the copy.
    for (size_t i = 0; i < keys.size(); i += 100) {
        UNIT_ASSERT_VALUES_EQUAL(copy.EstimateLessOrEqual(keys[i]),
                                 orig.EstimateLessOrEqual(keys[i]));
    }

    TEqHeightHistogram moved = std::move(copy);
    UNIT_ASSERT_VALUES_EQUAL(moved.GetTotalCount(), orig.GetTotalCount());
    UNIT_ASSERT_VALUES_EQUAL(moved.GetNumBuckets(), orig.GetNumBuckets());
    for (size_t i = 0; i < orig.GetNumBuckets(); ++i) {
        UNIT_ASSERT_VALUES_EQUAL(moved.GetBucket(i).UpperBound, orig.GetBucket(i).UpperBound);
        UNIT_ASSERT_VALUES_EQUAL(moved.GetBucket(i).CumulativeCount, orig.GetBucket(i).CumulativeCount);
    }
}

// === merge then Add must not dump half the summary ===
// Merge and Add share SoftBudget, so a merged state already fits the
// accumulation budget. One subsequent Add must not halve the entry count.
Y_UNIT_TEST(MergeThenAddKeepsResolution) {
    const ui32 B = 10;
    const ui32 M = 8;
    const ui32 keysPerPart = 500;
    const ui32 perKey = 2;
    const ui64 maxBytes = 4 * 1024;
    auto params = MakeParams(B, maxBytes);

    std::mt19937 rng(11);
    std::vector<std::vector<TString>> parts(M);
    for (ui32 p = 0; p < M; ++p) {
        for (ui32 k = 0; k < keysPerPart; ++k) {
            for (ui32 r = 0; r < perKey; ++r) {
                parts[p].push_back(MakeKey(static_cast<i32>(k)));
            }
        }
        std::shuffle(parts[p].begin(), parts[p].end(), rng);
    }
    std::vector<TEqHeightHistogramBuilder> states;
    for (const auto& part : parts) {
        states.push_back(BuildChunk(part, params));
    }
    auto result = MergeRandom(std::move(states), params, 5);

    const size_t before = result.GetEntries().size();
    UNIT_ASSERT_C(before > 1, "merged state must have entries");
    result.Add(MakeKey(12345));
    const size_t after = result.GetEntries().size();
    UNIT_ASSERT_C(after + 2 >= before,
                  "one Add after merge dropped entries from " << before << " to " << after);
}

// === unsorted Add flush batching — not quadratic ===
// Flush rebuilds Entries. flushAt = max(EmissionRate, |Entries|) keeps the
// Flush count well below N/EmissionRate; dropping the max() fails this.
Y_UNIT_TEST(UnsortedAddFlushCount) {
    const ui32 B = 10;
    const ui32 numKeys = 20000;
    auto params = MakeParams(B);

    std::mt19937 rng(7);
    auto keys = MakeSortedStream(numKeys, 1);
    std::shuffle(keys.begin(), keys.end(), rng);

    auto builder = BuildFromStream(keys, params);
    UNIT_ASSERT_VALUES_EQUAL(builder.GetTotalCount(), numKeys);

    const ui64 flushCount = builder.GetFlushCount();
    UNIT_ASSERT_C(flushCount * params.EmissionRate < numKeys / 2,
                  "Flush called " << flushCount << " times; naive every-EmissionRate "
                                  << "flushing is ~" << (numKeys / params.EmissionRate));
}

} // Y_UNIT_TEST_SUITE(EqHeightHistogram)
