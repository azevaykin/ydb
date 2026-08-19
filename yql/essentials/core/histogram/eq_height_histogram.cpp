#include "eq_height_histogram.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <util/generic/yexception.h>
#include <util/generic/ylimits.h>

namespace NKikimr {

namespace {

// Serialization helpers. Native byte order; little-endian asserted at the
// persistence boundary in ydb/core/statistics/events.h.

template <typename T>
void WriteRaw(TString& out, T val) {
    out.append(reinterpret_cast<const char*>(&val), sizeof(T));
}

template <typename T>
T ReadRaw(const char*& data, size_t& remaining) {
    Y_ENSURE(remaining >= sizeof(T), "malformed eq-height histogram blob");
    T val;
    std::memcpy(&val, data, sizeof(T));
    data += sizeof(T);
    remaining -= sizeof(T);
    return val;
}

void WriteStr(TString& out, TStringBuf str) {
    Y_ENSURE(str.size() <= Max<ui32>(), "eq-height histogram key exceeds ui32 length");
    WriteRaw<ui32>(out, static_cast<ui32>(str.size()));
    out.append(str.data(), str.size());
}

TStringBuf ReadStr(const char*& data, size_t& remaining) {
    ui32 len = ReadRaw<ui32>(data, remaining);
    Y_ENSURE(remaining >= len, "malformed eq-height histogram blob");
    TStringBuf result(data, len);
    data += len;
    remaining -= len;
    return result;
}

// r.Delta + l.Weight + r.Weight as 128-bit: ui64 FuseCost wraps on large tables
// and would treat an expensive pair as cheapest.
unsigned __int128 FuseCost(const TEqHeightHistogramBuilder::TEntry& l,
                           const TEqHeightHistogramBuilder::TEntry& r) {
    return static_cast<unsigned __int128>(r.Delta) + l.Weight + r.Weight;
}

bool ExceedsTwiceCap(unsigned __int128 value, ui64 cap) {
    return value > static_cast<unsigned __int128>(cap) * 2;
}

} // namespace

// === TEqHeightHistogramBuilder ===

TEqHeightHistogramBuilder::TEqHeightHistogramBuilder(const TParams& params)
    : Params_(params)
{
    ValidateParams();
}

void TEqHeightHistogramBuilder::ValidateParams() const {
    Y_ENSURE(Params_.EmissionRate > 0 && Params_.NumBuckets > 0 && Params_.MaxStateBytes > 0,
             "TEqHeightHistogramBuilder: EmissionRate, NumBuckets, and MaxStateBytes must be > 0");
}

ui64 TEqHeightHistogramBuilder::Cap() const {
    return std::max<ui64>(1, Rep_.TotalCount / Params_.EmissionRate);
}

ui64 TEqHeightHistogramBuilder::GetWeightCap() const {
    return Cap();
}

bool TEqHeightHistogramBuilder::GetBudgetForced() {
    Flush();
    return Rep_.Summary.BudgetForced;
}

TEqHeightHistogramBuilder::TEqHeightHistogramBuilder(const char* data, size_t size) {
    Y_ENSURE(data && size, "malformed eq-height histogram blob");
    const char* p = data;
    size_t remaining = size;
    ui8 version = ReadRaw<ui8>(p, remaining);
    Y_ENSURE(version == 1, "unsupported eq-height histogram state version " << static_cast<int>(version));
    ui8 flags = ReadRaw<ui8>(p, remaining);
    Rep_.Summary.BudgetForced = flags & 0x01;
    Params_.NumBuckets = ReadRaw<ui32>(p, remaining);
    Params_.EmissionRate = ReadRaw<ui32>(p, remaining);
    Params_.MaxStateBytes = ReadRaw<ui64>(p, remaining);
    ValidateParams();
    Rep_.TotalCount = ReadRaw<ui64>(p, remaining);
    Rep_.MinKey = TString(ReadStr(p, remaining));
    ui32 entryCount = ReadRaw<ui32>(p, remaining);
    // Bound reservation by remaining bytes to avoid OOM on a malformed blob.
    constexpr size_t minEntryBytes = sizeof(ui32) + 2 * sizeof(ui64) + sizeof(ui8);
    const size_t maxPossibleEntries = remaining / minEntryBytes;
    Rep_.Summary.Entries.reserve(std::min<size_t>(entryCount, maxPossibleEntries));
    for (ui32 i = 0; i < entryCount; ++i) {
        TEntry e;
        e.UpperBound = TString(ReadStr(p, remaining));
        e.Weight = ReadRaw<ui64>(p, remaining);
        e.Delta = ReadRaw<ui64>(p, remaining);
        e.SingleKey = ReadRaw<ui8>(p, remaining) != 0;
        // Blobs cross process boundaries; validate content.
        Y_ENSURE(e.Weight >= 1, "malformed eq-height histogram blob: zero Weight");
        if (i > 0) {
            Y_ENSURE(e.UpperBound > Rep_.Summary.Entries.back().UpperBound,
                     "malformed eq-height histogram blob: entries not strictly increasing");
        }
        Rep_.Summary.Entries.push_back(std::move(e));
    }
    Y_ENSURE(remaining == 0, "malformed eq-height histogram blob: trailing data");
    ui64 weightSum = 0;
    for (const auto& e : Rep_.Summary.Entries) {
        Y_ENSURE(e.Weight <= Rep_.TotalCount - weightSum,
                 "malformed eq-height histogram blob: weight sum exceeds total");
        weightSum += e.Weight;
    }
    Y_ENSURE(weightSum == Rep_.TotalCount, "malformed eq-height histogram blob: weight sum != total");
    if (!Rep_.Summary.Entries.empty()) {
        Y_ENSURE(Rep_.MinKey <= Rep_.Summary.Entries.front().UpperBound,
                 "malformed eq-height histogram blob: MinKey exceeds first UpperBound");
    } else {
        Y_ENSURE(Rep_.MinKey.empty(), "malformed eq-height histogram blob: empty state with MinKey");
    }
    Rep_.Sorted = false;
    RecountEntriesBytes(Rep_.Summary);
}

TStringBuf TEqHeightHistogramBuilder::MaxKey() const {
    if (Rep_.Summary.Entries.empty()) {
        return TStringBuf();
    }
    return Rep_.Summary.Entries.back().UpperBound;
}

void TEqHeightHistogramBuilder::RecountEntriesBytes(TSummary& s) {
    s.Bytes = 0;
    for (const auto& e : s.Entries) {
        s.Bytes = SaturatedAdd(s.Bytes, EntryBytes(e));
    }
}

ui64 TEqHeightHistogramBuilder::MaxRankUncertainty(const TSummary& s) {
    ui64 maxLoad = 0;
    for (const auto& e : s.Entries) {
        maxLoad = std::max(maxLoad, e.Delta);
    }
    return maxLoad;
}

ui64 TEqHeightHistogramBuilder::GetMaxRankError() {
    Flush();
    return MaxRankUncertainty(Rep_.Summary);
}

bool TEqHeightHistogramBuilder::IsExact() {
    return GetMaxRankError() == 0;
}

const TVector<TEqHeightHistogramBuilder::TEntry>& TEqHeightHistogramBuilder::GetEntries() {
    Flush();
    return Rep_.Summary.Entries;
}

void TEqHeightHistogramBuilder::Add(TStringBuf key) {
    Y_ENSURE(Rep_.TotalCount < Max<ui64>(), "eq-height histogram TotalCount overflow");
    ++Rep_.TotalCount;

    if (Rep_.Sorted && (Rep_.Summary.Entries.empty() || key >= MaxKey())) {
        AddSorted(key);
        return;
    }
    Rep_.Sorted = false;

    // Deserialized-empty state has MinKey == ""; treat first row as min.
    if (Rep_.TotalCount == 1 || key < Rep_.MinKey) {
        Rep_.MinKey = key;
    }
    Rep_.StagingBytes = SaturatedAdd(Rep_.StagingBytes, SaturatedAdd(key.size(), sizeof(ui32)));
    Rep_.Staging.emplace_back(key);
    // Scale flush batch with |Entries|: Flush rebuilds the whole vector,
    // so flushing every EmissionRate rows is quadratic past that size.
    const size_t flushAt = std::max<size_t>(Params_.EmissionRate, Rep_.Summary.Entries.size());
    if (Rep_.Staging.size() >= flushAt || SaturatedAdd(Rep_.StagingBytes, StateBytes()) > SoftBudget()) {
        Flush();
    }
}

void TEqHeightHistogramBuilder::PushEntry(TSummary& s, TStringBuf key) {
    s.Entries.push_back(TEntry{
        .UpperBound = TString(key),
        .Weight = 1,
        .Delta = 0,
        .SingleKey = true,
    });
    s.Bytes = SaturatedAdd(s.Bytes, EntryBytes(s.Entries.back()));
}

void TEqHeightHistogramBuilder::AddSorted(TStringBuf key) {
    TSummary& s = Rep_.Summary;
    if (s.Entries.empty()) {
        Rep_.MinKey = key;
        PushEntry(s, key);
        return;
    }
    // Never close mid-run: boundaries fall between distinct keys, so
    // cumulative weight is the true rank and Delta stays 0.
    TEntry& last = s.Entries.back();
    if (key == last.UpperBound) {
        ++last.Weight;
        return;
    }
    // Open a new entry when full (Weight >= Cap()), or when folding a
    // different key into a single-key entry with Weight >= ceil(Cap()/2)
    // would push Delta above 2*Cap().  Otherwise extend.
    // Cap() - Cap()/2 is ceil(Cap()/2) without overflowing Weight*2.
    if (last.Weight >= Cap() || (last.SingleKey && last.Weight >= Cap() - Cap() / 2)) {
        PushEntry(s, key);
    } else {
        // Length-delta update: add-then-subtract underflows when the new key
        // is shorter (sorted does not imply nondecreasing length).
        const ui64 oldLen = last.UpperBound.size();
        const ui64 newLen = key.size();
        if (newLen >= oldLen) {
            s.Bytes = SaturatedAdd(s.Bytes, newLen - oldLen);
        } else {
            s.Bytes -= oldLen - newLen;
        }
        last.UpperBound = key;
        last.SingleKey = false;
        ++last.Weight;
    }
    if (StateBytes() > SoftBudget()) {
        Compact(s, SoftBudget());
    }
}

TVector<TEqHeightHistogramBuilder::TEntry>
TEqHeightHistogramBuilder::CollapseSorted(TVector<TString> keys) {
    std::sort(keys.begin(), keys.end());
    TVector<TEntry> staged;
    staged.reserve(keys.size());
    for (auto& key : keys) {
        if (!staged.empty() && staged.back().UpperBound == key) {
            ++staged.back().Weight;
        } else {
            staged.push_back(TEntry{
                .UpperBound = std::move(key),
                .Weight = 1,
                .Delta = 0,
                .SingleKey = true,
            });
        }
    }
    return staged;
}

void TEqHeightHistogramBuilder::Flush() {
    if (Rep_.Staging.empty()) {
        return;
    }
    ++Rep_.FlushCount;
    TVector<TEntry> staged = CollapseSorted(std::move(Rep_.Staging));
    Rep_.Staging.clear();
    Rep_.StagingBytes = 0;
    // Staging and Summary share the builder's MinKey (the global minimum).
    // That over-approximates each side's true lower bound, so Delta is never
    // under-charged.
    InterleaveInto(Rep_.Summary, Rep_.MinKey, std::move(staged), Rep_.MinKey);
    Compact(Rep_.Summary, SoftBudget());
}

TEqHeightHistogramBuilder::TSummary TEqHeightHistogramBuilder::Fold() const {
    TSummary s = Rep_.Summary;
    if (!Rep_.Staging.empty()) {
        TVector<TEntry> staged = CollapseSorted(Rep_.Staging);
        // Same conservative MinKey as Flush(); see comment there.
        InterleaveInto(s, Rep_.MinKey, std::move(staged), Rep_.MinKey);
    }
    Compact(s, SoftBudget());
    return s;
}

void TEqHeightHistogramBuilder::InterleaveInto(TSummary& dst, TStringBuf dstMinKey,
                                               TVector<TEntry>&& other, TStringBuf otherMinKey) {
    // Rows owed by the side taken after `s`: Delta plus Weight-1 when the
    // range spans multiple keys. Both vanish for SingleKey, so one predicate
    // governs both.
    auto owed = [](const TEntry& s) -> ui64 {
        Y_DEBUG_ABORT_UNLESS(s.Weight >= 1, "eq-height histogram: Weight must be >= 1");
        if (s.SingleKey) {
            return s.Delta;
        }
        return SaturatedAdd(s.Delta, s.Weight - 1);
    };

    TVector<TEntry> out;
    out.reserve(dst.Entries.size() + other.size());

    size_t i = 0, j = 0;
    while (i < dst.Entries.size() || j < other.size()) {
        if (i < dst.Entries.size() && j < other.size() && dst.Entries[i].UpperBound == other[j].UpperBound) {
            TEntry e = std::move(dst.Entries[i++]);
            e.Weight = SaturatedAdd(e.Weight, other[j].Weight);
            e.Delta = SaturatedAdd(e.Delta, other[j].Delta);
            e.SingleKey = e.SingleKey && other[j].SingleKey;
            ++j;
            out.push_back(std::move(e));
            continue;
        }
        const bool takeLeft = (j == other.size()) || (i < dst.Entries.size() && dst.Entries[i].UpperBound < other[j].UpperBound);
        if (takeLeft) {
            TEntry e = std::move(dst.Entries[i++]);
            if (j < other.size()) {
                // Lower-bound-aware bump: charge owed(other[j]) only when
                // other's range extends to or below e.UpperBound.  For j > 0
                // the bound is other[j-1].UpperBound < e.UpperBound (always
                // applies).  For j == 0 it's otherMinKey; applies when
                // otherMinKey <= e.UpperBound (>=, not >: at least one row
                // sits on the true minimum).
                if (j > 0 || e.UpperBound >= otherMinKey) {
                    e.Delta = SaturatedAdd(e.Delta, owed(other[j]));
                }
            }
            out.push_back(std::move(e));
        } else {
            TEntry e = std::move(other[j++]);
            if (i < dst.Entries.size()) {
                // Symmetric: left's lower bound is dstMinKey for i == 0,
                // or dst.Entries[i-1].UpperBound for i > 0.
                if (i > 0 || dstMinKey <= e.UpperBound) {
                    e.Delta = SaturatedAdd(e.Delta, owed(dst.Entries[i]));
                }
            }
            out.push_back(std::move(e));
        }
    }
    dst.Entries = std::move(out);
    RecountEntriesBytes(dst);
}

void TEqHeightHistogramBuilder::ApplyFusions(TSummary& s, const TVector<bool>& absorbed) {
    const size_t n = s.Entries.size();
    TVector<TEntry> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (absorbed[i]) {
            Y_DEBUG_ABORT_UNLESS(i + 1 < n, "eq-height histogram: last entry cannot be absorbed");
            s.Entries[i + 1].Weight = SaturatedAdd(s.Entries[i + 1].Weight, s.Entries[i].Weight);
            s.Entries[i + 1].SingleKey = false;
            // Keep r.Delta; adding l.Delta would double-count (see FuseCost).
        } else {
            out.push_back(std::move(s.Entries[i]));
        }
    }
    s.Entries = std::move(out);
    RecountEntriesBytes(s);
}

bool TEqHeightHistogramBuilder::FuseLightestPair(TSummary& s) {
    const size_t n = s.Entries.size();
    if (n < 2) {
        return false;
    }
    // Find the lightest adjacent pair.
    size_t best = 0;
    auto bestCost = FuseCost(s.Entries[0], s.Entries[1]);
    for (size_t i = 1; i + 1 < n; ++i) {
        const auto c = FuseCost(s.Entries[i], s.Entries[i + 1]);
        if (c < bestCost) {
            bestCost = c;
            best = i;
        }
    }
    TVector<bool> absorbed(n, false);
    absorbed[best] = true;
    ApplyFusions(s, absorbed);
    return true;
}

bool TEqHeightHistogramBuilder::FuseAdmissiblePairs(TSummary& s, ui64 cap) {
    const size_t n = s.Entries.size();
    if (n < 2) {
        return false;
    }

    struct TPair {
        size_t Left;
        unsigned __int128 Cost;
    };
    TVector<TPair> pairs;
    pairs.reserve(n - 1);
    for (size_t i = 0; i + 1 < n; ++i) {
        pairs.push_back({.Left = i, .Cost = FuseCost(s.Entries[i], s.Entries[i + 1])});
    }

    std::sort(pairs.begin(), pairs.end(), [](const TPair& a, const TPair& b) {
        return a.Cost < b.Cost;
    });

    // Mark entries absorbed into their right neighbour.
    TVector<bool> absorbed(n, false);
    TVector<bool> used(n, false);
    bool anyFused = false;

    for (const auto& p : pairs) {
        if (used[p.Left] || used[p.Left + 1]) {
            continue;
        }
        if (ExceedsTwiceCap(p.Cost, cap)) {
            break;
        }
        used[p.Left] = true;
        used[p.Left + 1] = true;
        absorbed[p.Left] = true;
        anyFused = true;
    }

    if (!anyFused) {
        return false;
    }

    ApplyFusions(s, absorbed);
    return true;
}

void TEqHeightHistogramBuilder::Compact(TSummary& s, ui64 budgetBytes) const {
    for (size_t i = 1; i < s.Entries.size(); ++i) {
        Y_DEBUG_ABORT_UNLESS(s.Entries[i - 1].UpperBound < s.Entries[i].UpperBound,
                             "eq-height histogram invariant: entries must be unique and sorted");
    }
    const ui64 cap = Cap();
    const ui64 overheadBytes = OverheadBytes();
    // Two triggers: byte budget (overBytes) and fusion-admissibility (overCap).
    // overCap is not a MaxRankError invariant: fusion keeps r.Delta, so a max
    // sitting on the last entry cannot be reduced. Finalize enforces total/B.
    for (;;) {
        const bool overBytes = SaturatedAdd(s.Bytes, overheadBytes) > budgetBytes;
        const ui64 maxUnc = MaxRankUncertainty(s);
        const bool overCap = ExceedsTwiceCap(maxUnc, cap);
        if (!overBytes && !overCap) {
            return;
        }
        if (overBytes) {
            s.BudgetForced = true;
        }
        if (s.Entries.size() <= 1) {
            return;
        }
        if (!FuseAdmissiblePairs(s, cap) && !(overBytes && FuseLightestPair(s))) {
            return;
        }
    }
}

void TEqHeightHistogramBuilder::Merge(const TEqHeightHistogramBuilder& other) {
    // Self-merge would double-count rows.
    Y_ENSURE(this != &other, "TEqHeightHistogramBuilder::Merge: self-merge is not supported");
    Y_ENSURE(Params_.NumBuckets == other.Params_.NumBuckets &&
                 Params_.EmissionRate == other.Params_.EmissionRate,
             "TEqHeightHistogramBuilder::Merge: NumBuckets and EmissionRate must match");
    Flush();

    if (other.Rep_.TotalCount == 0) {
        return;
    }

    Y_ENSURE(other.Rep_.TotalCount <= Max<ui64>() - Rep_.TotalCount,
             "eq-height histogram TotalCount overflow");

    TSummary otherSummary = other.Fold();
    const bool otherBudgetForced = otherSummary.BudgetForced;

    // InterleaveInto's bump handles disjoint ranges (no Delta when ranges don't overlap).
    InterleaveInto(Rep_.Summary, Rep_.MinKey, std::move(otherSummary.Entries), other.Rep_.MinKey);
    if (Rep_.TotalCount == 0 || other.Rep_.MinKey < Rep_.MinKey) {
        Rep_.MinKey = other.Rep_.MinKey;
    }

    Rep_.TotalCount += other.Rep_.TotalCount;
    Rep_.Sorted = false;
    Rep_.Summary.BudgetForced |= otherBudgetForced;
    Compact(Rep_.Summary, SoftBudget());
}

TString TEqHeightHistogramBuilder::Serialize() const {
    const TSummary s = Fold();
    TString out;
    WriteRaw<ui8>(out, 1);
    ui8 flags = 0;
    if (s.BudgetForced) {
        flags |= 0x01;
    }
    WriteRaw<ui8>(out, flags);
    WriteRaw<ui32>(out, Params_.NumBuckets);
    WriteRaw<ui32>(out, Params_.EmissionRate);
    WriteRaw<ui64>(out, Params_.MaxStateBytes);
    WriteRaw<ui64>(out, Rep_.TotalCount);
    WriteStr(out, Rep_.MinKey);
    Y_ENSURE(s.Entries.size() <= Max<ui32>(), "eq-height histogram entry count exceeds ui32");
    WriteRaw<ui32>(out, static_cast<ui32>(s.Entries.size()));
    for (const auto& e : s.Entries) {
        WriteStr(out, e.UpperBound);
        WriteRaw<ui64>(out, e.Weight);
        WriteRaw<ui64>(out, e.Delta);
        WriteRaw<ui8>(out, e.SingleKey ? 1 : 0);
    }
    return out;
}

std::optional<TString> TEqHeightHistogramBuilder::Finalize() const {
    const TSummary s = Fold();
    if (Rep_.TotalCount == 0 || s.Entries.empty()) {
        return std::nullopt;
    }
    // Reject budget-starved, not tiny domains.  BudgetForced separates
    // "budget starved us" from "data is small".  Reject when entries <
    // max(MIN_ENTRIES, NumBuckets).
    if (s.BudgetForced && s.Entries.size() < std::max<ui32>(MIN_ENTRIES, Params_.NumBuckets)) {
        return std::nullopt;
    }

    const ui64 total = Rep_.TotalCount;
    const ui32 b = Params_.NumBuckets; // ValidateParams() guarantees > 0

    struct TBucketOut {
        TString UpperBound;
        ui64 CumulativeCount;
    };

    // maxRankError maxes Delta over boundary entries only, so it can be
    // smaller than GetMaxRankError() (which maxes over all entries).
    TVector<TBucketOut> buckets;
    ui64 acc = 0, bucketIdx = 1, maxRankError = 0;

    // 128-bit threshold: acc >= ceil(idx*total/b) without overflow.
    auto reached = [&](ui64 idx) {
        return static_cast<unsigned __int128>(acc) * b >= static_cast<unsigned __int128>(idx) * total;
    };

    for (size_t i = 0; i < s.Entries.size(); ++i) {
        const auto& e = s.Entries[i];
        acc += e.Weight;
        const bool last = (i + 1 == s.Entries.size());
        if (last || reached(bucketIdx)) {
            buckets.push_back({
                .UpperBound = e.UpperBound,
                .CumulativeCount = acc,
            });
            maxRankError = std::max(maxRankError, e.Delta);
            while (bucketIdx <= b && reached(bucketIdx)) {
                ++bucketIdx;
            }
        }
    }

    // Guard rail: reject when rank error exceeds one bucket width.
    if (maxRankError > total / b) {
        return std::nullopt;
    }

    TString out;
    WriteRaw<ui8>(out, 1);
    WriteRaw<ui32>(out, static_cast<ui32>(buckets.size()));
    WriteRaw<ui64>(out, total);
    WriteRaw<ui64>(out, maxRankError);
    for (const auto& bkt : buckets) {
        WriteStr(out, bkt.UpperBound);
        WriteRaw<ui64>(out, bkt.CumulativeCount);
    }
    return out;
}

// === TEqHeightHistogram ===

TEqHeightHistogram::TEqHeightHistogram(const char* data, size_t size) {
    Y_ENSURE(data && size, "malformed eq-height histogram blob");
    Y_ENSURE(size <= Max<ui32>(), "malformed eq-height histogram blob: size exceeds ui32 offset range");
    Data_ = TString(data, size);
    const char* p = Data_.data();
    size_t remaining = Data_.size();
    ui8 version = ReadRaw<ui8>(p, remaining);
    Y_ENSURE(version == 1, "unsupported eq-height histogram version " << static_cast<int>(version));
    ui32 numBuckets = ReadRaw<ui32>(p, remaining);
    Y_ENSURE(numBuckets > 0, "malformed eq-height histogram blob: zero buckets");
    TotalCount_ = ReadRaw<ui64>(p, remaining);
    MaxRankError_ = ReadRaw<ui64>(p, remaining);
    // Bound reservation by remaining bytes to avoid OOM on a malformed blob.
    constexpr size_t minBucketBytes = sizeof(ui32) + sizeof(ui64);
    const size_t maxPossibleBuckets = remaining / minBucketBytes;
    Buckets_.reserve(std::min<size_t>(numBuckets, maxPossibleBuckets));
    for (ui32 i = 0; i < numBuckets; ++i) {
        TBucketRec r;
        ui32 len = ReadRaw<ui32>(p, remaining);
        Y_ENSURE(remaining >= len, "malformed eq-height histogram blob");
        r.Offset = static_cast<ui32>(p - Data_.data());
        r.Length = len;
        p += len;
        remaining -= len;
        r.CumulativeCount = ReadRaw<ui64>(p, remaining);
        Y_ENSURE(r.CumulativeCount <= TotalCount_,
                 "malformed eq-height histogram blob: cumulative count exceeds total");
        if (i > 0) {
            const TBucketRec& prev = Buckets_.back();
            TStringBuf curBound(Data_.data() + r.Offset, r.Length);
            TStringBuf prevBound(Data_.data() + prev.Offset, prev.Length);
            Y_ENSURE(curBound > prevBound,
                     "malformed eq-height histogram blob: buckets not strictly increasing");
            Y_ENSURE(r.CumulativeCount > prev.CumulativeCount,
                     "malformed eq-height histogram blob: cumulative count not increasing");
        }
        Buckets_.push_back(r);
    }
    Y_ENSURE(remaining == 0, "malformed eq-height histogram blob: trailing data");
    // Finalize makes the last bucket's CumulativeCount equal the total.
    Y_ENSURE(Buckets_.back().CumulativeCount == TotalCount_,
             "malformed eq-height histogram blob: last cumulative count != total");
}

ui64 TEqHeightHistogram::EstimateLessOrEqual(TStringBuf key) const {
    if (Buckets_.empty()) {
        return 0;
    }
    auto it = std::upper_bound(Buckets_.begin(), Buckets_.end(), key,
                               [this](TStringBuf k, const TBucketRec& r) {
                                   return k < TStringBuf(Data_.data() + r.Offset, r.Length);
                               });
    if (it == Buckets_.begin()) {
        return 0;
    }
    --it;
    return it->CumulativeCount;
}

} // namespace NKikimr
