#pragma once

#include <ydb/library/signals/owner.h>

namespace NKikimr::NOlap::NIndexes::NFulltext {

// Build and skip are events. The outstanding gauge is the scheme-actualizer queue of
// portions whose schema still lacks a fulltext index id. A zero gauge is not coverage:
// later writes may skip insert-time build, and a deliberately skipped portion stays
// unindexed after it leaves the queue.
class TFulltextBuildCounters: public NColumnShard::TCommonCountersOwner {
private:
    using TBase = NColumnShard::TCommonCountersOwner;

    NMonitoring::TDynamicCounters::TCounterPtr Builds;
    NMonitoring::TDynamicCounters::TCounterPtr BuildRows;
    NMonitoring::TDynamicCounters::TCounterPtr BuildChunks;
    NMonitoring::TDynamicCounters::TCounterPtr BuildBytes;
    NMonitoring::TDynamicCounters::TCounterPtr Skips;
    NMonitoring::TDynamicCounters::TCounterPtr IndexedChunksRead;
    NMonitoring::TDynamicCounters::TCounterPtr IndexedBytesRead;
    NMonitoring::TDynamicCounters::TCounterPtr RowsFromPostings;
    NMonitoring::TDynamicCounters::TCounterPtr MatchedRows;
    NMonitoring::TDynamicCounters::TCounterPtr FallbackRows;
    NMonitoring::TDynamicCounters::TCounterPtr FallbackBytes;
    NMonitoring::TDynamicCounters::TCounterPtr DecodeErrors;
    std::shared_ptr<NColumnShard::TValueAggregationAgent> OutstandingEligiblePortions;

    TFulltextBuildCounters()
        : TBase("FulltextIndex")
    {
        Builds = TBase::GetDeriviative("Build/Count");
        BuildRows = TBase::GetDeriviative("Build/Rows");
        BuildChunks = TBase::GetDeriviative("Build/Chunks");
        BuildBytes = TBase::GetDeriviative("Build/Bytes");
        Skips = TBase::GetDeriviative("Skip/Count");
        IndexedChunksRead = TBase::GetDeriviative("Read/IndexedChunks");
        IndexedBytesRead = TBase::GetDeriviative("Read/IndexedBytes");
        RowsFromPostings = TBase::GetDeriviative("Read/RowsFromPostings");
        MatchedRows = TBase::GetDeriviative("Read/MatchedRows");
        FallbackRows = TBase::GetDeriviative("Read/FallbackRows");
        FallbackBytes = TBase::GetDeriviative("Read/FallbackBytes");
        DecodeErrors = TBase::GetDeriviative("Read/DecodeErrors");
        OutstandingEligiblePortions = TBase::GetValueAutoAggregations("Actualization/OutstandingEligiblePortions");
    }

    static TFulltextBuildCounters& Instance() {
        static TFulltextBuildCounters counters;
        return counters;
    }

public:
    static void OnBuilt(const ui32 rows, const ui32 chunks, const ui64 bytes) {
        auto& self = Instance();
        self.Builds->Add(1);
        self.BuildRows->Add(rows);
        self.BuildChunks->Add(chunks);
        self.BuildBytes->Add(bytes);
    }

    static void OnSkipped() {
        Instance().Skips->Add(1);
    }

    static void OnIndexedRead(const ui32 chunks, const ui64 bytes, const ui32 rows, const ui32 matched) {
        auto& self = Instance();
        self.IndexedChunksRead->Add(chunks);
        self.IndexedBytesRead->Add(bytes);
        self.RowsFromPostings->Add(rows);
        self.MatchedRows->Add(matched);
    }

    static void OnFallback(const ui32 rows, const ui64 bytes) {
        auto& self = Instance();
        self.FallbackRows->Add(rows);
        self.FallbackBytes->Add(bytes);
    }

    static void OnDecodeError() {
        Instance().DecodeErrors->Add(1);
    }

    static std::shared_ptr<NColumnShard::TValueAggregationClient> BuildOutstandingEligiblePortions() {
        return Instance().OutstandingEligiblePortions->GetClient();
    }
};

} // namespace NKikimr::NOlap::NIndexes::NFulltext
