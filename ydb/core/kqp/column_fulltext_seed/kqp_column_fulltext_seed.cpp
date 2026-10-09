#include <ydb/core/kqp/runtime/kqp_column_fulltext_write.h>
#include <ydb/core/kqp/runtime/kqp_write_table.h>
#include <ydb/core/tx/schemeshard/index/column_fulltext_seed.h>

#include <ydb/core/base/appdata.h>
#include <ydb/core/base/table_index.h>
#include <ydb/core/base/tablet_pipecache.h>
#include <ydb/core/engine/minikql/minikql_engine_host.h>
#include <ydb/core/formats/arrow/converter.h>
#include <ydb/core/kqp/common/kqp_tx_manager.h>
#include <ydb/core/kqp/compute_actor/kqp_compute_events.h>
#include <ydb/core/kqp/counters/kqp_counters.h>
#include <ydb/core/kqp/runtime/kqp_buffer_lookup_actor.h>
#include <ydb/core/protos/tx_datashard.pb.h>
#include <ydb/core/scheme/scheme_types_proto.h>
#include <ydb/core/tx/datashard/datashard.h>
#include <ydb/core/tx/datashard/range_ops.h>
#include <ydb/core/tx/scheme_cache/scheme_cache.h>
#include <ydb/core/tx/sequenceproxy/public/events.h>
#include <ydb/core/tx/tx_proxy/proxy.h>
#include <ydb/core/tx/tx_proxy/upload_rows.h>
#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/core/hfunc.h>
#include <ydb/library/actors/core/log.h>
#include <ydb/library/formats/arrow/arrow_helpers.h>
#include <ydb/library/formats/arrow/validation/validation.h>
#include <yql/essentials/minikql/mkql_alloc.h>
#include <yql/essentials/minikql/computation/mkql_computation_node_holders.h>
#include <yql/essentials/public/issue/yql_issue_message.h>

#include <util/string/builder.h>

namespace NKikimr::NKqp {

namespace {

using namespace NActors;

NScheme::TTypeInfo ColumnType(const NKikimrKqp::TKqpColumnMetadataProto& column) {
    return NScheme::TypeInfoFromProto(column.GetTypeId(), column.GetTypeInfo());
}

TColumnFulltextMaintainer::TColumn ToColumn(const NKikimrKqp::TKqpColumnMetadataProto& column) {
    return TColumnFulltextMaintainer::TColumn{
        .Name = column.GetName(),
        .Type = ColumnType(column),
        .NotNull = column.GetNotNull(),
    };
}

TColumnFulltextMaintainer::TIndex MakeIndex(const TColumnFulltextSeedSettings& settings) {
    TColumnFulltextMaintainer::TIndex index;
    index.PostingId = settings.PostingId;
    index.StateId = settings.StateId;
    index.DocsId = settings.DocsId;
    index.StatsId = settings.StatsId;
    index.MapId = settings.MapId;
    index.Relevance = settings.Relevance;
    index.Synthetic = settings.Synthetic;
    index.Ready = false;
    index.SeedOnly = true;
    index.BuildGeneration = settings.BuildGeneration;
    index.DocIdType = settings.DocIdType;
    index.Text = TColumnFulltextMaintainer::TColumn{
        .Name = settings.TextColumn,
        .Type = settings.TextType,
        .NotNull = settings.TextNotNull,
    };
    for (const auto& name : settings.PrefixColumns) {
        for (const auto& column : settings.ScanColumns) {
            if (column.GetName() == name) {
                index.Prefix.push_back(ToColumn(column));
                break;
            }
        }
    }
    for (const auto& name : settings.CoveredColumns) {
        for (const auto& column : settings.ScanColumns) {
            if (column.GetName() == name) {
                index.Covered.push_back(ToColumn(column));
                break;
            }
        }
    }
    index.Settings = settings.Settings;
    for (const auto& column : settings.StateColumns) {
        index.StateColumns.push_back(ToColumn(column));
    }
    index.StateValueNames = settings.StateValueNames;
    for (const auto& column : settings.MapColumns) {
        index.MapColumns.push_back(ToColumn(column));
    }
    return index;
}

class TColumnFulltextSeedActor
    : public TActorBootstrapped<TColumnFulltextSeedActor>
    , public IKqpBufferTableLookupCallbacks
{
public:
    explicit TColumnFulltextSeedActor(TColumnFulltextSeedSettings&& settings)
        : Settings(std::move(settings))
        , Index(MakeIndex(Settings))
        , PipeCacheId(MakePipePerNodeCacheID(false))
        , Alloc(std::make_shared<NMiniKQL::TScopedAlloc>(__LOCATION__))
        , TypeEnv(std::make_shared<NMiniKQL::TTypeEnvironment>(*Alloc))
        , MemInfo("TColumnFulltextSeedActor")
        , HolderFactory(std::make_shared<NMiniKQL::THolderFactory>(Alloc->Ref(), MemInfo, AppData()->FunctionRegistry))
        , TxManager(CreateKqpTransactionManager())
    {
        Maintainer.SetLayout(InputColumnNames(), {}, KeyColumnNames());
        Maintainer.AddIndex(Index);
    }

    static constexpr char ActorName[] = "COLUMN_FULLTEXT_SEED_ACTOR";

    void Bootstrap() {
        Become(&TThis::StateWork);
        if (Settings.ScanColumns.empty() || Settings.KeyColumnCount == 0) {
            return Fail("Column fulltext seed is missing scan columns");
        }
        StartScan();
    }

private:
    STFUNC(StateWork) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvKqpCompute::TEvScanInitActor, Handle);
            hFunc(TEvKqpCompute::TEvScanData, Handle);
            hFunc(TEvKqpCompute::TEvScanError, Handle);
            hFunc(TEvPipeCache::TEvDeliveryProblem, Handle);
            hFunc(TEvTxUserProxy::TEvUploadRowsResponse, Handle);
            hFunc(NSequenceProxy::TEvSequenceProxy::TEvNextValResult, Handle);
            hFunc(TEvents::TEvPoison, HandlePoison);
            default:
                break;
        }
    }

    std::vector<TString> InputColumnNames() const {
        std::vector<TString> names;
        names.reserve(Settings.ScanColumns.size());
        for (const auto& column : Settings.ScanColumns) {
            names.push_back(column.GetName());
        }
        return names;
    }

    std::vector<TString> KeyColumnNames() const {
        std::vector<TString> names;
        names.reserve(Settings.KeyColumnCount);
        for (ui32 i = 0; i < Settings.KeyColumnCount; ++i) {
            names.push_back(Settings.ScanColumns[i].GetName());
        }
        return names;
    }

    void StartScan() {
        auto ev = std::make_unique<TEvDataShard::TEvKqpScan>();
        ev->Record.SetLocalPathId(Settings.ParentTableId.PathId.LocalPathId);
        ev->Record.SetTablePath(Settings.ParentTablePath);
        ev->Record.SetSchemaVersion(Settings.ParentTableId.SchemaVersion);
        ev->Record.SetScanId(1);
        ev->Record.SetGeneration(1);
        ev->Record.SetTxId(Settings.SnapshotTxId ? Settings.SnapshotTxId : Settings.BuildId);
        if (Settings.SnapshotStep && Settings.SnapshotTxId) {
            ev->Record.MutableSnapshot()->SetStep(Settings.SnapshotStep);
            ev->Record.MutableSnapshot()->SetTxId(Settings.SnapshotTxId);
        }
        ev->Record.SetDataFormat(NKikimrDataEvents::FORMAT_ARROW);
        ev->Record.SetReverse(false);
        for (const auto& column : Settings.ScanColumns) {
            ev->Record.AddColumnTags(column.GetId());
            ev->Record.AddColumnTypes(column.GetTypeId());
            if (column.HasTypeInfo()) {
                *ev->Record.AddColumnTypeInfos() = column.GetTypeInfo();
            } else {
                *ev->Record.AddColumnTypeInfos() = NKikimrProto::TTypeInfo();
            }
        }
        if (Settings.LastKeyAck) {
            auto* range = ev->Record.MutableRanges()->Add();
            TSerializedTableRange(Settings.LastKeyAck, "", false, false).Serialize(*range);
        }
        Send(PipeCacheId, new TEvPipeCache::TEvForward(ev.release(), Settings.ColumnTabletId, true),
            IEventHandle::FlagTrackDelivery, Settings.ColumnTabletId);
    }

    void Handle(TEvKqpCompute::TEvScanInitActor::TPtr& ev) {
        if (Finished || Failed) {
            return;
        }
        ScanGeneration = ev->Get()->Record.GetGeneration();
        ScanActor = ActorIdFromProto(ev->Get()->Record.GetScanActorId());
        Send(ScanActor, new TEvKqpCompute::TEvScanDataAck(8ull << 20, ScanGeneration));
    }

    void Handle(TEvKqpCompute::TEvScanData::TPtr& ev) {
        if (Finished || Failed || Committing) {
            return;
        }
        if (ScanActor && ev->Get()->Generation != ScanGeneration) {
            return;
        }

        std::vector<TOwnedCellVec> rows;
        auto consume = [&](TConstArrayRef<TCell> cells) {
            if (cells.size() != Settings.ScanColumns.size()) {
                Fail("Column fulltext seed scan returned unexpected column count");
                return;
            }
            rows.emplace_back(cells);
            if (!cells.empty()) {
                LastKey = TOwnedCellVec(cells.first(Settings.KeyColumnCount));
            }
        };

        if (!ev->Get()->Rows.empty()) {
            for (const auto& row : ev->Get()->Rows) {
                consume(row);
                if (Failed) {
                    return;
                }
            }
        } else if (ev->Get()->ArrowBatch && ev->Get()->ArrowBatch->num_rows() > 0) {
            std::vector<std::pair<TString, NScheme::TTypeInfo>> schema;
            schema.reserve(Settings.ScanColumns.size());
            for (const auto& column : Settings.ScanColumns) {
                schema.emplace_back(column.GetName(), ColumnType(column));
            }
            auto fields = NArrow::TStatusValidator::GetValid(NArrow::MakeArrowFields(schema, {}));
            auto renamedSchema = std::make_shared<arrow::Schema>(std::move(fields));
            for (const auto& batch : NArrow::SliceToRecordBatches(ev->Get()->ArrowBatch)) {
                auto renamed = arrow::RecordBatch::Make(renamedSchema, batch->num_rows(), batch->columns());
                std::function<void(TConstArrayRef<TCell>)> consumeFn = consume;
                struct TCopyRows : NArrow::IRowWriter {
                    std::function<void(TConstArrayRef<TCell>)>* Consume = nullptr;
                    void AddRow(const TConstArrayRef<TCell>& cells) override {
                        (*Consume)(cells);
                    }
                } writer;
                writer.Consume = &consumeFn;
                NArrow::TArrowToYdbConverter converter(schema, writer);
                TString error;
                if (!converter.Process(*renamed, error)) {
                    Fail(TStringBuilder() << "Cannot read column seed batch: " << error);
                    return;
                }
                if (Failed) {
                    return;
                }
            }
        }

        ScanFinished = ev->Get()->Finished;
        if (!rows.empty()) {
            PendingRows.insert(PendingRows.end(), std::make_move_iterator(rows.begin()), std::make_move_iterator(rows.end()));
        }
        if (PendingRows.size() >= Settings.MaxBatchRows || (ScanFinished && !PendingRows.empty())) {
            BeginCommitBatch();
            return;
        }
        if (ScanFinished) {
            ReplyDone();
            return;
        }
        if (ScanActor) {
            Send(ScanActor, new TEvKqpCompute::TEvScanDataAck(8ull << 20, ScanGeneration));
        }
    }

    void Handle(TEvKqpCompute::TEvScanError::TPtr& ev) {
        NYql::TIssues issues;
        NYql::IssuesFromMessage(ev->Get()->Record.GetIssues(), issues);
        Fail(issues.ToOneLineString());
    }

    void Handle(TEvPipeCache::TEvDeliveryProblem::TPtr& ev) {
        if (ev->Get()->TabletId == Settings.ColumnTabletId) {
            Fail(TStringBuilder() << "ColumnShard " << Settings.ColumnTabletId << " delivery problem");
        }
    }

    void BeginCommitBatch() {
        if (Committing || Failed || Finished) {
            return;
        }
        Committing = true;
        BatchRows = std::move(PendingRows);
        PendingRows.clear();
        if (BatchRows.empty()) {
            Committing = false;
            if (ScanFinished) {
                ReplyDone();
            }
            return;
        }

        EnsureStateLookup();
        if (Failed) {
            return;
        }
        Maintainer.ClearState();
        std::vector<TConstArrayRef<TCell>> keys;
        keys.reserve(BatchRows.size());
        for (const auto& row : BatchRows) {
            keys.push_back(row.first(Settings.KeyColumnCount));
        }
        StateLookup->AddLookupTask(/*cookie*/ 1, keys);
    }

    void EnsureStateLookup() {
        if (StateLookup) {
            return;
        }
        if (!Counters) {
            Counters = MakeIntrusive<TKqpCounters>(AppData()->Counters, nullptr);
        }
        auto [lookup, actor] = CreateKqpBufferTableLookup(TKqpBufferTableLookupSettings{
            .Callbacks = this,
            .TableId = TTableId(Settings.StateId, 0),
            .TablePath = Settings.StateTablePath,
            .LockTxId = Settings.BuildId,
            .LockNodeId = SelfId().NodeId(),
            .LockMode = NKikimrDataEvents::OPTIMISTIC,
            .TxManager = TxManager,
            .Alloc = Alloc,
            .TypeEnv = *TypeEnv,
            .HolderFactory = *HolderFactory,
            .SessionActorId = SelfId(),
            .Counters = Counters,
            .Database = Settings.Database,
            .IsOlap = false,
        });
        StateLookup = lookup;
        StateLookupActor = Register(actor);

        TVector<NKikimrKqp::TKqpColumnMetadataProto> keyColumns;
        TVector<NKikimrKqp::TKqpColumnMetadataProto> valueColumns;
        for (ui32 i = 0; i < Settings.KeyColumnCount; ++i) {
            keyColumns.push_back(Settings.ScanColumns[i]);
        }
        THashSet<TString> keyNames;
        for (const auto& column : keyColumns) {
            keyNames.insert(column.GetName());
        }
        for (const auto& column : Settings.StateColumns) {
            if (keyNames.contains(column.GetName())) {
                continue;
            }
            valueColumns.push_back(column);
        }
        StateLookup->SetLookupSettings(1, keyColumns.size(), keyColumns, valueColumns, std::nullopt, {});
    }

    void OnLookupTaskFinished() override {
        if (Failed || Finished || !Committing) {
            return;
        }
        if (!StateLookup->HasResult(1)) {
            return;
        }
        StateLookup->ExtractResult(1, [&](TConstArrayRef<TCell> row) {
            Maintainer.SetStateRow(Settings.StateId, row);
        });

        std::vector<TColumnFulltextMaintainer::TInputRow> input;
        input.reserve(BatchRows.size());
        for (const auto& row : BatchRows) {
            input.push_back(TColumnFulltextMaintainer::TInputRow{
                .Cells = row,
                .BaseExists = true,
                .BaseExistenceKnown = true,
            });
        }
        if (const auto error = Maintainer.PrepareSeed(input)) {
            Fail(error);
            return;
        }
        PendingSequences = Maintainer.Sequences();
        SequenceValues.clear();
        if (PendingSequences.empty()) {
            FinishBuildAndUpload();
            return;
        }
        RequestNextSequence();
    }

    void OnLookupError(
        NYql::NDqProto::StatusIds::StatusCode,
        NYql::EYqlIssueCode,
        const TString& message,
        const NYql::TIssues&) override
    {
        Fail(message);
    }

    void RequestNextSequence() {
        for (;;) {
            if (SequenceCursor >= PendingSequences.size()) {
                for (const auto& need : PendingSequences) {
                    Maintainer.AssignSequences(need.PathId, SequenceValues[need.PathId]);
                }
                FinishBuildAndUpload();
                return;
            }
            const auto& need = PendingSequences[SequenceCursor];
            if (SequenceValues[need.PathId].size() >= need.Count) {
                ++SequenceCursor;
                continue;
            }
            const TString path = need.PathId == Settings.PostingId
                ? Settings.GenSequencePath
                : Settings.DocIdSequencePath;
            if (path.empty()) {
                Fail("Column fulltext seed is missing a sequence path");
                return;
            }
            Send(NSequenceProxy::MakeSequenceProxyServiceID(),
                new NSequenceProxy::TEvSequenceProxy::TEvNextVal(Settings.Database, path),
                0, ++SequenceCookie);
            SequenceCookieToPath[SequenceCookie] = need.PathId;
            return;
        }
    }

    void Handle(NSequenceProxy::TEvSequenceProxy::TEvNextValResult::TPtr& ev) {
        if (Failed || Finished) {
            return;
        }
        auto it = SequenceCookieToPath.find(ev->Cookie);
        if (it == SequenceCookieToPath.end()) {
            return;
        }
        const auto pathId = it->second;
        SequenceCookieToPath.erase(it);
        if (ev->Get()->Status != Ydb::StatusIds::SUCCESS) {
            Fail(TStringBuilder() << "Failed to allocate seed sequence: " << ev->Get()->Issues.ToOneLineString());
            return;
        }
        SequenceValues[pathId].push_back(static_cast<ui64>(ev->Get()->Value));
        RequestNextSequence();
    }

    void FinishBuildAndUpload() {
        TGuard<NMiniKQL::TScopedAlloc> guard(*Alloc);
        std::vector<TColumnFulltextMaintainer::TWriteBatch> batches;
        TColumnFulltextMaintainer::TStats stats;
        if (const auto error = Maintainer.BuildSeedBatches(Alloc, batches, stats)) {
            Fail(error);
            return;
        }
        Y_UNUSED(stats);
        Metering.SetUploadRows(Metering.GetUploadRows() + 1);
        UploadsRemaining = 0;
        if (batches.empty()) {
            OnBatchCommitted();
            return;
        }
        for (auto& batch : batches) {
            if (batch.Batch->IsEmpty()) {
                continue;
            }
            if (!StartUpload(batch)) {
                return;
            }
        }
        if (UploadsRemaining == 0) {
            OnBatchCommitted();
        }
    }

    bool StartUpload(const TColumnFulltextMaintainer::TWriteBatch& batch) {
        TString path;
        ui32 keyColumns = 0;
        std::shared_ptr<const NTxProxy::TUploadTypes> types;
        if (batch.PathId == Settings.StateId) {
            path = Settings.StateTablePath;
            keyColumns = Settings.StateKeyColumnCount;
            types = MakeTypes(Settings.StateUploadColumns);
        } else if (batch.PathId == Settings.PostingId) {
            path = Settings.PostingTablePath;
            keyColumns = Settings.PostingKeyColumnCount;
            types = MakeTypes(Settings.PostingColumns);
        } else if (batch.PathId == Settings.DocsId) {
            path = Settings.DocsTablePath;
            keyColumns = Settings.DocsKeyColumnCount;
            types = MakeTypes(Settings.DocsColumns);
        } else if (batch.PathId == Settings.StatsId) {
            path = Settings.StatsTablePath;
            keyColumns = Settings.StatsKeyColumnCount;
            types = MakeTypes(Settings.StatsColumns);
        } else if (batch.PathId == Settings.MapId) {
            path = Settings.MapTablePath;
            keyColumns = Settings.MapKeyColumnCount;
            types = MakeTypes(Settings.MapUploadColumns);
        } else {
            Fail("Column fulltext seed produced a write for an unknown path");
            return false;
        }
        if (path.empty() || !types || keyColumns == 0) {
            Fail(TStringBuilder() << "Column fulltext seed is missing upload metadata for " << path);
            return false;
        }

        auto rows = std::make_shared<NTxProxy::TUploadRows>();
        const auto cellRows = GetRows(batch.Batch);
        for (const auto& cells : cellRows) {
            if (cells.size() < keyColumns) {
                Fail("Column fulltext seed batch has fewer cells than key columns");
                return false;
            }
            TVector<TCell> keyCells(cells.begin(), cells.begin() + keyColumns);
            TVector<TCell> valueCells(cells.begin() + keyColumns, cells.end());
            rows->emplace_back(TSerializedCellVec(keyCells), TSerializedCellVec::Serialize(valueCells));
        }
        if (rows->empty()) {
            return true;
        }
        ++UploadsRemaining;
        Register(NTxProxy::CreateUploadRowsInternal(
            SelfId(),
            Settings.Database,
            path,
            types,
            rows,
            NTxProxy::EUploadRowsMode::Normal,
            /*writeToPrivateTable=*/ true,
            /*writeToIndexImplTable=*/ true));
        return true;
    }

    static std::shared_ptr<const NTxProxy::TUploadTypes> MakeTypes(
        const TVector<std::pair<TString, Ydb::Type>>& columns)
    {
        auto types = std::make_shared<NTxProxy::TUploadTypes>();
        types->reserve(columns.size());
        for (const auto& [name, type] : columns) {
            types->emplace_back(name, type);
        }
        return types;
    }

    void Handle(TEvTxUserProxy::TEvUploadRowsResponse::TPtr& ev) {
        if (Failed || Finished || !Committing) {
            return;
        }
        if (ev->Get()->Status != Ydb::StatusIds::SUCCESS) {
            Fail(TStringBuilder() << "Column fulltext seed upload failed: " << ev->Get()->Issues.ToOneLineString());
            return;
        }
        if (UploadsRemaining > 0) {
            --UploadsRemaining;
        }
        if (UploadsRemaining == 0) {
            OnBatchCommitted();
        }
    }

    void OnBatchCommitted() {
        Committing = false;
        BatchRows.clear();
        if (LastKey) {
            ReplyProgress(TSerializedCellVec::Serialize(*LastKey));
        }
        if (ScanFinished && PendingRows.empty()) {
            ReplyDone();
            return;
        }
        if (!PendingRows.empty() && (PendingRows.size() >= Settings.MaxBatchRows || ScanFinished)) {
            BeginCommitBatch();
            return;
        }
        if (!ScanFinished && ScanActor) {
            Send(ScanActor, new TEvKqpCompute::TEvScanDataAck(8ull << 20, ScanGeneration));
        }
    }

    void ReplyProgress(const TString& lastKeyAck) {
        auto response = MakeHolder<TEvColumnFulltextSeed::TEvResponse>();
        response->BuildId = Settings.BuildId;
        response->TabletId = Settings.ColumnTabletId;
        response->RequestSeqNoGeneration = Settings.SeqNoGeneration;
        response->RequestSeqNoRound = Settings.SeqNoRound;
        response->Status = NKikimrIndexBuilder::EBuildStatus::IN_PROGRESS;
        response->LastKeyAck = lastKeyAck;
        response->MeteringStats = Metering;
        Send(Settings.Owner, response.Release());
    }

    void ReplyDone() {
        if (Finished) {
            return;
        }
        Finished = true;
        auto response = MakeHolder<TEvColumnFulltextSeed::TEvResponse>();
        response->BuildId = Settings.BuildId;
        response->TabletId = Settings.ColumnTabletId;
        response->RequestSeqNoGeneration = Settings.SeqNoGeneration;
        response->RequestSeqNoRound = Settings.SeqNoRound;
        response->Status = NKikimrIndexBuilder::EBuildStatus::DONE;
        if (LastKey) {
            response->LastKeyAck = TSerializedCellVec::Serialize(*LastKey);
        }
        response->MeteringStats = Metering;
        Send(Settings.Owner, response.Release());
        Cleanup();
        PassAway();
    }

    void Fail(const TString& message) {
        if (Failed || Finished) {
            return;
        }
        Failed = true;
        auto response = MakeHolder<TEvColumnFulltextSeed::TEvResponse>();
        response->BuildId = Settings.BuildId;
        response->TabletId = Settings.ColumnTabletId;
        response->RequestSeqNoGeneration = Settings.SeqNoGeneration;
        response->RequestSeqNoRound = Settings.SeqNoRound;
        response->Status = NKikimrIndexBuilder::EBuildStatus::BUILD_ERROR;
        response->Issues = message;
        response->MeteringStats = Metering;
        Send(Settings.Owner, response.Release());
        Cleanup();
        PassAway();
    }

    void HandlePoison(TEvents::TEvPoison::TPtr&) {
        Cleanup();
        PassAway();
    }

    void Cleanup() {
        if (StateLookup) {
            StateLookup->Terminate();
            StateLookup = nullptr;
        }
        if (StateLookupActor) {
            Send(StateLookupActor, new TEvents::TEvPoison);
            StateLookupActor = {};
        }
        if (ScanActor) {
            Send(ScanActor, new TEvents::TEvPoison);
            ScanActor = {};
        }
    }

    TColumnFulltextSeedSettings Settings;
    TColumnFulltextMaintainer::TIndex Index;
    const TActorId PipeCacheId;
    std::shared_ptr<NMiniKQL::TScopedAlloc> Alloc;
    std::shared_ptr<NMiniKQL::TTypeEnvironment> TypeEnv;
    NMiniKQL::TMemoryUsageInfo MemInfo;
    std::shared_ptr<NMiniKQL::THolderFactory> HolderFactory;
    IKqpTransactionManagerPtr TxManager;
    TIntrusivePtr<TKqpCounters> Counters;
    TColumnFulltextMaintainer Maintainer;

    TActorId ScanActor;
    ui32 ScanGeneration = 0;
    bool ScanFinished = false;
    bool Committing = false;
    bool Failed = false;
    bool Finished = false;

    std::vector<TOwnedCellVec> PendingRows;
    std::vector<TOwnedCellVec> BatchRows;
    std::optional<TOwnedCellVec> LastKey;

    IKqpBufferTableLookup* StateLookup = nullptr;
    TActorId StateLookupActor;
    std::vector<TColumnFulltextMaintainer::TSequenceNeed> PendingSequences;
    size_t SequenceCursor = 0;
    THashMap<TPathId, std::vector<ui64>> SequenceValues;
    ui64 SequenceCookie = 0;
    THashMap<ui64, TPathId> SequenceCookieToPath;
    ui32 UploadsRemaining = 0;
    NKikimrIndexBuilder::TMeteringStats Metering;
};

NActors::IActor* CreateColumnFulltextSeedActorImpl(TColumnFulltextSeedSettings&& settings) {
    return new TColumnFulltextSeedActor(std::move(settings));
}

} // anonymous namespace

void ForceLinkColumnFulltextSeed() {
    RegisterColumnFulltextSeedFactory(&CreateColumnFulltextSeedActorImpl);
}

}
