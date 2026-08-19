#pragma once

#include <util/generic/string.h>
#include <util/generic/strbuf.h>
#include <util/generic/vector.h>
#include <util/generic/ylimits.h>
#include <util/system/types.h>

#include <optional>

namespace NKikimr {

// Equi-height histogram over memcomparable byte keys (`memcmp` == value order).
// Keys are produced with NMiniKQL::TPresortEncoder, shared with DQ sort keys.
// Mergeable summary: partial states from independent read tasks are combined
// pairwise in an order the builder does not control (see Merge()).
class TEqHeightHistogramBuilder {
public:
    struct TParams {
        ui32 NumBuckets = 1;           // B, buckets in the finalized histogram
        ui32 EmissionRate = 8;         // C = f * B, entries the summary aims to hold
        ui64 MaxStateBytes = 4U << 20; // hard bound on the serialized state bytes (entries + MinKey + header)
    };

    struct TEntry {
        TString UpperBound;     // boundary key
        ui64 Weight = 0;        // rows with key in (prev UpperBound, UpperBound]
        ui64 Delta = 0;         // rank uncertainty; 0 == exact rank
        bool SingleKey = false; // true iff the entry covers exactly one key
    };

    explicit TEqHeightHistogramBuilder(const TParams& params);
    TEqHeightHistogramBuilder(const char* data, size_t size); // from a serialized intermediate state

    // Add a key. Sorted input: AddSorted fast path (Delta == 0, exact rank).
    // Unsorted: buffered in Staging, interleaved on Flush (Delta > 0).
    void Add(TStringBuf key);

    // Absorb another builder's state. Result is independent of merge order.
    // NumBuckets and EmissionRate must match; MaxStateBytes may differ.
    void Merge(const TEqHeightHistogramBuilder& other);

    TString Serialize() const;               // intermediate state, wire form between stages
    std::optional<TString> Finalize() const; // final blob; nullopt == "do not store a histogram"

    ui64 GetTotalCount() const {
        return Rep_.TotalCount;
    }
    // Cap() = max(1, TotalCount / EmissionRate).
    ui64 GetWeightCap() const;
    // Flushes staging. Test only.
    bool GetBudgetForced();
    // Const ref into internal entries. Test only; invalidated by the next
    // Add/Merge or any method that triggers Flush/Compact.
    const TVector<TEntry>& GetEntries();
    // Max Delta across all entries (flushes staging). Finalize's blob stores
    // the max over bucket-boundary entries only, so the two can differ.
    ui64 GetMaxRankError();
    TStringBuf GetMinKey() const {
        return Rep_.MinKey;
    }
    // Completed Flush() calls. Test only: pins Add's flushAt against quadratic rebuilds.
    ui64 GetFlushCount() const {
        return Rep_.FlushCount;
    }
    // Flushes staging.
    bool IsExact();

    // Floor on entry count for Finalize; also requires >= NumBuckets.
    static constexpr ui32 MIN_ENTRIES = 16;

private:
    // Compaction state. Split out so Fold() can compact a copy.
    struct TSummary {
        TVector<TEntry> Entries;   // sorted by UpperBound, keys unique
        ui64 Bytes = 0;            // running sum of EntryBytes(), so the budget check is O(1)
        bool BudgetForced = false; // a compaction fired on the byte budget, not on the rank-uncertainty cap
    };

    struct TRep {
        TSummary Summary;
        TVector<TString> Staging; // unsorted keys not yet folded into Entries
        TString MinKey;
        ui64 TotalCount = 0;
        ui64 StagingBytes = 0;
        ui64 FlushCount = 0; // incremented by Flush() when Staging was non-empty
        bool Sorted = true;  // the input stream has been monotone so far (Add fast path)
    };

    TParams Params_;
    TRep Rep_;

    TStringBuf MaxKey() const; // == Summary.Entries.back().UpperBound, or empty
    // Operational budget for Add/Flush/Merge: leave half of MaxStateBytes for
    // Staging so a post-merge Add does not immediately compact the summary.
    ui64 SoftBudget() const {
        return Params_.MaxStateBytes / 2;
    }

    // Fixed serialization overhead: version, flags, params, count, MinKey, entry count.
    static constexpr ui64 HeaderBytes = 2 * sizeof(ui8) + 2 * sizeof(ui32) + 2 * sizeof(ui64) + 2 * sizeof(ui32);
    // Total serialized size estimate: entries + MinKey + fixed header.
    ui64 StateBytes() const {
        return SaturatedAdd(Rep_.Summary.Bytes, OverheadBytes());
    }
    ui64 OverheadBytes() const {
        return SaturatedAdd(Rep_.MinKey.size(), HeaderBytes);
    }

    static ui64 SaturatedAdd(ui64 a, ui64 b) {
        return a > Max<ui64>() - b ? Max<ui64>() : a + b;
    }

    void AddSorted(TStringBuf key);
    void Flush();          // Staging -> Summary (in place)
    TSummary Fold() const; // Summary + Staging, compacted, as a value. Pure
    static TVector<TEntry> CollapseSorted(TVector<TString> keys);
    static void PushEntry(TSummary& s, TStringBuf key);
    static void InterleaveInto(TSummary& dst, TStringBuf dstMinKey,
                               TVector<TEntry>&& other, TStringBuf otherMinKey);
    // Fold absorbed entries into their right neighbour; recount bytes.
    static void ApplyFusions(TSummary& s, const TVector<bool>& absorbed);
    // Fuse the lightest adjacent pair regardless of admissibility.
    static bool FuseLightestPair(TSummary& s);
    // Fuse all admissible adjacent pairs (FuseCost <= 2*Cap()).
    static bool FuseAdmissiblePairs(TSummary& s, ui64 cap);
    void Compact(TSummary& s, ui64 budgetBytes) const;
    static void RecountEntriesBytes(TSummary& s);
    // EmissionRate, NumBuckets, MaxStateBytes must be > 0.
    void ValidateParams() const;

    // Cap() = max(1, TotalCount / EmissionRate).
    ui64 Cap() const;

    // Max Delta across all entries. Self-reported; Finalize enforces total/B.
    static ui64 MaxRankUncertainty(const TSummary& s);

    static ui64 EntryBytes(const TEntry& e) {
        return SaturatedAdd(e.UpperBound.size(), 2 * sizeof(ui64) + sizeof(ui32) + sizeof(ui8));
    }
};

// Parsed finalized histogram. Owns its bytes; bucket bounds are offset-based,
// so copyable and movable by default.
class TEqHeightHistogram {
public:
    struct TBucket {
        TStringBuf UpperBound;
        ui64 CumulativeCount = 0; // rows with key <= UpperBound
    };

    TEqHeightHistogram(const char* data, size_t size); // Y_ENSUREs on a malformed blob

    TEqHeightHistogram(const TEqHeightHistogram&) = default;
    TEqHeightHistogram& operator=(const TEqHeightHistogram&) = default;
    TEqHeightHistogram(TEqHeightHistogram&&) = default;
    TEqHeightHistogram& operator=(TEqHeightHistogram&&) = default;

    ui64 GetTotalCount() const {
        return TotalCount_;
    }
    ui64 GetMaxRankError() const {
        return MaxRankError_;
    }
    bool IsExact() const {
        return MaxRankError_ == 0;
    }
    size_t GetNumBuckets() const {
        return Buckets_.size();
    }
    TBucket GetBucket(size_t i) const {
        const auto& r = Buckets_.at(i);
        return {BoundAt(i), r.CumulativeCount};
    }

    // Lower bound on rows with key <= `key`, O(log B). No interpolation.
    ui64 EstimateLessOrEqual(TStringBuf key) const;

private:
    struct TBucketRec {
        ui32 Offset = 0;
        ui32 Length = 0;
        ui64 CumulativeCount = 0;
    };

    TStringBuf BoundAt(size_t i) const {
        return TStringBuf(Data_.data() + Buckets_[i].Offset, Buckets_[i].Length);
    }

    TString Data_;
    TVector<TBucketRec> Buckets_;
    ui64 TotalCount_ = 0;
    ui64 MaxRankError_ = 0;
};

} // namespace NKikimr
