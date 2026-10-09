#pragma once

#include <ydb/core/protos/index_builder.pb.h>
#include <ydb/core/protos/kqp_tablemetadata.pb.h>
#include <ydb/core/scheme/scheme_pathid.h>
#include <ydb/core/scheme/scheme_tablecell.h>
#include <ydb/core/scheme/scheme_tabledefs.h>
#include <ydb/core/tx/schemeshard/index/build_index.h>
#include <ydb/library/actors/core/actorid.h>
#include <ydb/library/actors/core/event_local.h>
#include <ydb/public/api/protos/ydb_table.pb.h>

#include <util/generic/vector.h>

namespace NKikimr::NKqp {

// POD settings + response event. The actor implementation lives in
// ydb/core/kqp/column_fulltext_seed and is registered at process start via
// RegisterColumnFulltextSeedFactory (avoids a schemeshard <-> kqp PEERDIR cycle).

struct TEvColumnFulltextSeed {
    struct TEvResponse
        : public NActors::TEventLocal<TEvResponse, NSchemeShard::TEvIndexBuilder::EvColumnFulltextSeedResponse>
    {
        ui64 BuildId = 0;
        ui64 TabletId = 0;
        ui64 RequestSeqNoGeneration = 0;
        ui64 RequestSeqNoRound = 0;
        NKikimrIndexBuilder::EBuildStatus Status = NKikimrIndexBuilder::EBuildStatus::INVALID;
        TString LastKeyAck;
        TString Issues;
        NKikimrIndexBuilder::TMeteringStats MeteringStats;
    };
};

struct TColumnFulltextSeedSettings {
    NActors::TActorId Owner;
    ui64 BuildId = 0;
    ui64 ColumnTabletId = 0;
    ui64 SeqNoGeneration = 0;
    ui64 SeqNoRound = 0;

    TString Database;
    TTableId ParentTableId;
    TString ParentTablePath;
    ui64 SnapshotStep = 0;
    ui64 SnapshotTxId = 0;
    TString LastKeyAck;

    TVector<NKikimrKqp::TKqpColumnMetadataProto> ScanColumns;
    ui32 KeyColumnCount = 0;

    // Index meta flattened for the factory (no TColumnFulltextMaintainer dependency here).
    TPathId PostingId;
    TPathId StateId;
    TPathId DocsId;
    TPathId StatsId;
    TPathId MapId;
    bool Relevance = false;
    bool Synthetic = false;
    ui64 BuildGeneration = 1;
    NScheme::TTypeInfo DocIdType;
    TString TextColumn;
    bool TextNotNull = false;
    NScheme::TTypeInfo TextType;
    TVector<TString> PrefixColumns;
    TVector<TString> CoveredColumns;
    Ydb::Table::FulltextIndexSettings Settings;

    TVector<NKikimrKqp::TKqpColumnMetadataProto> StateColumns;
    TVector<TString> StateValueNames;
    TVector<NKikimrKqp::TKqpColumnMetadataProto> MapColumns;

    TString PostingTablePath;
    TString StateTablePath;
    TString DocsTablePath;
    TString StatsTablePath;
    TString MapTablePath;
    TString GenSequencePath;
    TString DocIdSequencePath;

    TVector<std::pair<TString, Ydb::Type>> PostingColumns;
    TVector<std::pair<TString, Ydb::Type>> StateUploadColumns;
    TVector<std::pair<TString, Ydb::Type>> DocsColumns;
    TVector<std::pair<TString, Ydb::Type>> StatsColumns;
    TVector<std::pair<TString, Ydb::Type>> MapUploadColumns;
    ui32 PostingKeyColumnCount = 0;
    ui32 StateKeyColumnCount = 0;
    ui32 DocsKeyColumnCount = 0;
    ui32 StatsKeyColumnCount = 0;
    ui32 MapKeyColumnCount = 0;

    ui32 MaxBatchRows = 500;
};

using TColumnFulltextSeedFactory = NActors::IActor* (*)(TColumnFulltextSeedSettings&&);

void RegisterColumnFulltextSeedFactory(TColumnFulltextSeedFactory factory);
TColumnFulltextSeedFactory GetColumnFulltextSeedFactory();

// Pulls the KQP seed actor implementation into the binary and registers the factory.
// Call from a live KQP entrypoint (e.g. CreateKqpProxyService); a static-only
// force-link object is not enough under --gc-sections.
void ForceLinkColumnFulltextSeed();

// Convenience: creates via the registered factory (nullptr if KQP did not register).
NActors::IActor* CreateColumnFulltextSeedActor(TColumnFulltextSeedSettings&& settings);

}
