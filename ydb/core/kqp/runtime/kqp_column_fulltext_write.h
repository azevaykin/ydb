#pragma once

#include "kqp_write_table.h"

#include <ydb/core/base/fulltext.h>
#include <ydb/core/base/table_index.h>
#include <ydb/core/scheme/scheme_pathid.h>
#include <ydb/core/scheme/scheme_tablecell.h>
#include <ydb/core/scheme/scheme_types_proto.h>
#include <ydb/public/api/protos/ydb_table.pb.h>

#include <memory>
#include <optional>
#include <vector>

#include <util/generic/hash.h>

namespace NKikimr::NKqp {

inline constexpr i64 ColumnFulltextTokenMemoryLimit = 8ll * 1024 * 1024;

// Plans column-table compact fulltext maintenance for one statement flush.
// The caller reads forward state (and, when needed, the column old image) first.
// Build() then emits posting, docs, stats, state, and reverse-map batches.
// Conflict retries construct a new planner, so stats and deltas are not reused.
class TColumnFulltextMaintainer {
public:
    struct TColumn {
        TString Name;
        NScheme::TTypeInfo Type;
        bool NotNull = false;
    };

    struct TIndex {
        TPathId PostingId;
        TPathId StateId;
        TPathId DocsId;
        TPathId StatsId;
        TPathId MapId;
        bool Relevance = false;
        bool Synthetic = false;
        bool Ready = false;
        // When true, skip keys that already have forward state. Used by the
        // online build scanner so a concurrent mutation that won the race is kept.
        bool SeedOnly = false;
        ui64 BuildGeneration = 1;
        NScheme::TTypeInfo DocIdType;
        TColumn Text;
        std::vector<TColumn> Prefix;
        std::vector<TColumn> Covered;
        std::optional<TColumn> Ttl;
        // Full state-table column list in the order the write payload uses.
        std::vector<TColumn> StateColumns;
        // Non-key state columns, in the order the state lookup returns them after the primary key.
        std::vector<TString> StateValueNames;
        std::vector<TColumn> MapColumns;
        Ydb::Table::FulltextIndexSettings Settings;
    };

    struct TInputRow {
        TConstArrayRef<TCell> Cells;
        bool BaseExists = false;
        bool BaseExistenceKnown = false;
    };

    struct TWriteBatch {
        TPathId PathId;
        // True selects the delete cookie. Posting removals stay on the write cookie:
        // they are upserts of removal segments, not deletions of posting keys.
        bool Delete = false;
        IDataBatchPtr Batch;
    };

    struct TStats {
        i64 StateBytes = 0;
        i64 PostingBytes = 0;
        i64 BatchMemory = 0;
    };

    void SetLayout(
        std::vector<TString> inputColumns,
        std::vector<TString> lookupColumns,
        std::vector<TString> keyColumns);

    void AddIndex(TIndex index);

    bool Empty() const {
        return Indexes.empty();
    }

    // Last write of a primary key wins, matching the SQL writer's duplicate-key rule.
    void SetRows(
        NKikimrKqp::TKqpTableSinkSettings::EType operation,
        const std::vector<TInputRow>& rows);

    // Drops state captured for the previous flush. Absent keys stay absent.
    void ClearState();

    // Full state-table row: primary key followed by StateValueNames. Absent keys are simply not set.
    void SetStateRow(const TPathId& stateId, TConstArrayRef<TCell> row);

    // Empty string means the plan is usable. A non-empty string rejects the mutation before any write.
    TString Prepare();

    struct TSequenceNeed {
        TPathId PathId;
        size_t Count = 0;
    };

    std::vector<TSequenceNeed> Sequences() const;

    // values are sequence results already range-checked by the caller.
    void AssignSequences(const TPathId& pathId, const std::vector<ui64>& values);

    TString Build(std::shared_ptr<NKikimr::NMiniKQL::TScopedAlloc> alloc, std::vector<TWriteBatch>& out, TStats& stats);

    // Fence-snapshot seed: scanned rows are the final image. Indexes must already
    // be added with SeedOnly=true and forward state filled via SetStateRow.
    // Initialized live rows and tombstones produce no writes. Support tables only.
    // After PrepareSeed, call Sequences()/AssignSequences() then BuildSeedBatches.
    TString PrepareSeed(const std::vector<TInputRow>& rows);

    TString BuildSeedBatches(
        std::shared_ptr<NKikimr::NMiniKQL::TScopedAlloc> alloc,
        std::vector<TWriteBatch>& out,
        TStats& stats);

private:
    struct TResolved {
        bool Found = false;
        TCell Cell;
    };

    struct TPlanned {
        struct TStateField {
            bool DocId = false;
            bool Tokens = false;
            bool Null = false;
            TOwnedCellVec Value;
        };

        TOwnedCellVec Row;
        TString Key;
        ui64 Order = 0;
        bool BaseExists = false;
        bool BaseExistenceKnown = false;

        bool WriteState = false;
        bool Tombstone = false;
        bool RemovePostings = false;
        bool AddPostings = false;
        bool UpdateDocs = false;
        bool AllocateDocId = false;
        bool RetireDocId = false;
        bool DocIdNull = true;
        ui64 DocId = 0;
        ui64 RetiredDocId = 0;
        int DocIdSlot = -1;

        TOwnedCellVec OldPrefix;
        TOwnedCellVec NewPrefix;
        TOwnedCellVec NewCovered;
        TVector<NFulltext::TDocumentStateToken> OldTokens;
        TVector<NFulltext::TDocumentStateToken> NewTokens;
        TString TokensEncoded;
        ui32 NewLength = 0;
        std::vector<TStateField> Fields;
        std::vector<TString> Owned;
    };

    struct TIndexPlan {
        TIndex Index;
        std::vector<TPlanned> Rows;
        size_t DocIdCount = 0;
        size_t GenerationCount = 0;
        std::vector<ui64> DocIds;
        std::vector<ui64> Generations;
        THashMap<TString, TOwnedCellVec> StateRows;
    };

    TResolved FromInput(const TPlanned& row, const TString& name) const;
    TResolved FromLookup(const TPlanned& row, const TString& name) const;
    TResolved FromState(const TIndexPlan& index, const TPlanned& row, const TString& name) const;
    TResolved FinalCell(const TIndexPlan& index, const TPlanned& row, const TColumn& column, bool stateLive) const;

    struct TRawRow {
        TOwnedCellVec Cells;
        bool BaseExists = false;
        bool BaseExistenceKnown = false;
        ui64 Order = 0;
    };

    std::vector<TRawRow> Rows;
    std::vector<TString> InputColumns;
    std::vector<TString> LookupColumns;
    std::vector<TString> KeyColumns;
    THashMap<TString, size_t> InputPos;
    THashMap<TString, size_t> LookupPos;
    THashMap<TString, size_t> KeyPos;
    NKikimrKqp::TKqpTableSinkSettings::EType Operation =
        NKikimrKqp::TKqpTableSinkSettings::MODE_UPSERT;
    std::vector<TIndexPlan> Indexes;
};

}
