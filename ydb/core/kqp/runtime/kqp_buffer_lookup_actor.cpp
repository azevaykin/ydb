#include "kqp_buffer_lookup_actor.h"

#include <ydb/core/kqp/tracing/kqp_query_rendering.h>
#include <ydb/core/kqp/tracing/kqp_shard_rendering.h>
#include <ydb/core/base/tablet_pipecache.h>
#include <ydb/core/formats/arrow/arrow_batch_builder.h>
#include <ydb/core/formats/arrow/arrow_helpers.h>
#include <ydb/core/formats/arrow/converter.h>
#include <ydb/core/kqp/common/kqp_locks_tli_helpers.h>
#include <ydb/core/kqp/compute_actor/kqp_compute_events.h>
#include <ydb/core/kqp/gateway/kqp_gateway.h>
#include <ydb/core/kqp/runtime/kqp_arrow_memory_pool.h>
#include <ydb/core/kqp/runtime/kqp_read_iterator_common.h>
#include <ydb/core/kqp/runtime/kqp_stream_lookup_worker.h>
#include <ydb/core/protos/kqp_stats.pb.h>
#include <ydb/core/scheme/scheme_types_proto.h>
#include <ydb/core/tx/scheme_cache/scheme_cache.h>
#include <ydb/core/tx/schemeshard/olap/schema/schema.h>
#include <ydb/core/tx/sharding/sharding.h>
#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/core/interconnect.h>
#include <ydb/library/formats/arrow/arrow_helpers.h>
#include <ydb/library/formats/arrow/validation/validation.h>
#include <ydb/library/wilson_ids/wilson.h>
#include <ydb/library/yql/dq/actors/compute/dq_compute_actor_log.h>
#include <ydb/library/yql/dq/actors/protos/dq_stats.pb.h>
#include <yql/essentials/public/issue/yql_issue_message.h>

#define YDB_LOG_THIS_FILE_COMPONENT NKikimrServices::KQP_COMPUTE


namespace NKikimr {
namespace NKqp {

namespace {

class TKqpBufferLookupActor : public NActors::TActorBootstrapped<TKqpBufferLookupActor>, public IKqpBufferTableLookup {
private:
    struct TEvPrivate {
        enum EEv {
            EvRetryRead = EventSpaceBegin(TKikimrEvents::ES_PRIVATE),
            EvSchemeCacheRequestTimeout
        };

        struct TEvSchemeCacheRequestTimeout : public TEventLocal<TEvSchemeCacheRequestTimeout, EvSchemeCacheRequestTimeout> {
        };

        struct TEvRetryRead : public TEventLocal<TEvRetryRead, EvRetryRead> {
            explicit TEvRetryRead(ui64 readId, ui64 lastSeqNo)
                : ReadId(readId)
                , LastSeqNo(lastSeqNo) {
            }

            const ui64 ReadId;
            const ui64 LastSeqNo;
        };
    };

    struct TLookupState {
        std::unique_ptr<TKqpStreamLookupWorker> Worker;
        ui64 ReadsInflight = 0;
        ui64 LookupColumnsCount = 0;
        std::optional<NKikimrDataEvents::TMvccSnapshot> MvccSnapshot;

        bool ResolvePending = false;
        bool IsUniqueCheck = false;
        bool FailOnUniqueCheck = false;

        bool IsAllReadsFinished() const {
            return ReadsInflight == 0;
        }
    };

    struct TShardState {
        bool HasPipe = false;
    };

    struct TReadState {
        const ui64 LookupCookie;
        const ui64 ShardId;
        ui64 LastSeqNo = 0;
        const bool IsUniqueCheck;
        const bool FailOnUniqueCheck;
        bool Blocked = false;

        ui64 RetryAttempts = 0;
    };

public:
    TKqpBufferLookupActor(TKqpBufferTableLookupSettings&& settings)
        : Settings(std::move(settings))
        , Partitioning(Settings.TxManager->GetPartitioning(Settings.TableId))
        , LogPrefix(TStringBuilder() << "Table: `" << Settings.TablePath << "` (" << Settings.TableId << "), "
            << "SessionActorId: " << Settings.SessionActorId) {
    }

    ~TKqpBufferLookupActor() {
        ClearAllWorkerResults();
    }

    void Bootstrap() {
        YDB_LOG_DEBUG("Starting buffer lookup actor",
            {"logPrefix", this->LogPrefix});

        Settings.Counters->StreamLookupActorsCount->Inc();
        Become(&TKqpBufferLookupActor::StateFunc);
    }

    static constexpr char ActorName[] = "KQP_BUFFER_LOOKUP_ACTOR";

    void PassAway() final {
        FinishLookupTrace(Ydb::StatusIds::STATUS_CODE_UNSPECIFIED);
        Settings.Counters->StreamLookupActorsCount->Dec();

        ClearAllWorkerResults();

        for (const auto& [readId, state] : ReadIdToState) {
            Settings.Counters->SentIteratorCancels->Inc();
            auto cancel = MakeHolder<TEvDataShard::TEvReadCancel>();
            cancel->Record.SetReadId(readId);
            Send(PipeCacheId, new TEvPipeCache::TEvForward(cancel.Release(), state.ShardId, false));
        }
        ReadIdToState.clear();

        Unlink();

        TActorBootstrapped<TKqpBufferLookupActor>::PassAway();
    }

    void Terminate() override {
        PassAway();
    }

    void Unlink() override {
        AFL_ENSURE(ReadIdToState.empty());

        for (auto& [_, state] : ShardToState) {
            state.HasPipe = false;
        }
        Send(PipeCacheId, new TEvPipeCache::TEvUnlink(0));
    }

    STFUNC(StateFunc) {
        try {
            switch (ev->GetTypeRewrite()) {
                hFunc(TEvTxProxySchemeCache::TEvResolveKeySetResult, Handle);
                hFunc(TEvDataShard::TEvReadResult, Handle);
                hFunc(TEvPipeCache::TEvDeliveryProblem, Handle);
                hFunc(TEvPrivate::TEvRetryRead, Handle);
            default:
                RuntimeError(
                    NYql::NDqProto::StatusIds::INTERNAL_ERROR,
                    NYql::TIssuesIds::KIKIMR_INTERNAL_ERROR,
                    TStringBuilder() << "Unexpected event: " << ev->GetTypeRewrite());
            }
        } catch (const NKikimr::TMemoryLimitExceededException& e) {
            RuntimeError(
                NYql::NDqProto::StatusIds::PRECONDITION_FAILED,
                NYql::TIssuesIds::KIKIMR_PRECONDITION_FAILED,
                "Memory limit exceeded at stream lookup");
        } catch (const yexception& e) {
            RuntimeError(
                NYql::NDqProto::StatusIds::INTERNAL_ERROR,
                NYql::TIssuesIds::KIKIMR_INTERNAL_ERROR,
                e.what());
        }
    }

    void SetLookupSettings(
            ui64 cookie,
            size_t lookupKeyPrefix,
            TConstArrayRef<NKikimrKqp::TKqpColumnMetadataProto> keyColumns,
            TConstArrayRef<NKikimrKqp::TKqpColumnMetadataProto> lookupColumns,
            const std::optional<NKikimrDataEvents::TMvccSnapshot>& mvccSnapshot,
            const NWilson::TTraceId& traceId) override {
        if (!LookupActorSpan) {
            LookupActorSpan = MakeQueryPhaseTraceSpan(TWilsonKqp::LookupActor,
                NWilson::TTraceId(traceId), {
                    .Name = "Check rows",
                    .Phase = "BufferLookup",
                    .ActorType = "TKqpBufferLookupActor",
                    .Component = "KqpBufferLookup",
                    .PeerActorType = "DataShard",
                });
        }
        TLookupSettings settings {
            .TablePath = Settings.TablePath,
            .TableId = Settings.TableId,
            .Database = Settings.Database,
            .PoolId = Settings.PoolId,

            .AllowNullKeysPrefixSize = 0,
            .KeepRowsOrder = false,
            .LookupStrategy = NKqpProto::EStreamLookupStrategy::LOOKUP,

            .KeyColumns = {},
            .InputColumns = {},
            .Columns = {},
        };

        if (KeyColumnTypes.empty()) {
            for (const auto& keyColumn : keyColumns) {
                NScheme::TTypeInfo typeInfo = NScheme::TypeInfoFromProto(keyColumn.GetTypeId(), keyColumn.GetTypeInfo());
                KeyColumnTypes.push_back(typeInfo);
            }
        } else {
            AFL_ENSURE(KeyColumnTypes.size() == keyColumns.size());
        }

        {
            settings.KeyColumns.reserve(keyColumns.size());
            i32 keyOrder = 0;
            for (const auto& keyColumn : keyColumns) {
                NScheme::TTypeInfo typeInfo = NScheme::TypeInfoFromProto(keyColumn.GetTypeId(), keyColumn.GetTypeInfo());
                settings.KeyColumns.emplace(
                    keyColumn.GetName(),
                    TSysTables::TTableColumnInfo{
                        keyColumn.GetName(),
                        keyColumn.GetId(),
                        typeInfo,
                        keyColumn.GetTypeInfo().GetPgTypeMod(),
                        keyOrder++
                    }
                );
            }
        }

        {
            AFL_ENSURE(lookupKeyPrefix <= keyColumns.size());
            settings.InputColumns.reserve(lookupKeyPrefix);
            for (size_t index = 0; index < lookupKeyPrefix; ++index) {
                const auto& keyColumn = keyColumns.at(index);
                NScheme::TTypeInfo typeInfo = NScheme::TypeInfoFromProto(keyColumn.GetTypeId(), keyColumn.GetTypeInfo());
                settings.InputColumns.push_back(
                    TSysTables::TTableColumnInfo{
                        keyColumn.GetName(),
                        keyColumn.GetId(),
                        typeInfo,
                        keyColumn.GetTypeInfo().GetPgTypeMod(),
                        static_cast<i32>(index)
                    }
                );
            }
        }

        {
            settings.Columns.reserve(keyColumns.size() + lookupColumns.size());
            for (size_t index = 0; index < keyColumns.size(); ++index) {
                const auto& keyColumn = keyColumns.at(index);
                NScheme::TTypeInfo typeInfo = NScheme::TypeInfoFromProto(keyColumn.GetTypeId(), keyColumn.GetTypeInfo());
                settings.Columns.push_back(
                    TSysTables::TTableColumnInfo{
                        keyColumn.GetName(),
                        keyColumn.GetId(),
                        typeInfo,
                        keyColumn.GetTypeInfo().GetPgTypeMod(),
                    }
                );
            }
            for (const auto& lookupColumn : lookupColumns) {
                NScheme::TTypeInfo typeInfo = NScheme::TypeInfoFromProto(lookupColumn.GetTypeId(), lookupColumn.GetTypeInfo());
                settings.Columns.push_back(
                    TSysTables::TTableColumnInfo{
                        lookupColumn.GetName(),
                        lookupColumn.GetId(),
                        typeInfo,
                        lookupColumn.GetTypeInfo().GetPgTypeMod(),
                    }
                );
            }
        }

        AFL_ENSURE(CookieToLookupState.emplace(
                cookie,
                TLookupState{
                    .Worker = CreateLookupWorker(std::move(settings), Settings.TypeEnv, Settings.HolderFactory),
                    .ReadsInflight = 0,
                    .LookupColumnsCount = lookupColumns.size(),
                    .MvccSnapshot = mvccSnapshot,
                }
            ).second);
    }

    void AddLookupTask(ui64 cookie, const std::vector<TConstArrayRef<TCell>>& keys) override {
        AddTask(cookie, keys, false, false);
    }

    void AddUniqueCheckTask(ui64 cookie, const std::vector<TConstArrayRef<TCell>>& keys, bool immediateFail) override {
        AddTask(cookie, keys, true, immediateFail);
    }

    void AddTask(ui64 cookie, const std::vector<TConstArrayRef<TCell>>& keys, bool isUnique, bool immediateFail) {
        auto& state = CookieToLookupState.at(cookie);
        auto& worker = state.Worker;

        AFL_ENSURE(state.ReadsInflight == 0);
        AFL_ENSURE(state.Worker->AllRowsProcessed());

        state.IsUniqueCheck = isUnique;
        state.FailOnUniqueCheck = immediateFail;

        for (const auto& key : keys) {
            worker->AddInputRow(key);
        }

        if (!Partitioning) {
            state.ResolvePending = !state.Worker->AllRowsProcessed();
            return;
        }

        StartLookupTask(cookie, state, isUnique, immediateFail);
    }

    void StartLookupTask(ui64 cookie, TLookupState& state, bool isUnique, bool uniqueFailOnRead) {
        auto& worker = state.Worker;
        worker->BuildRequests(Partitioning, ReadId);

        while(true) {
            auto [shardId, read] = worker->PopNextRequest();
            if (!read) {
                break;
            }

            ++state.ReadsInflight;
            StartTableRead(cookie, shardId, isUnique, uniqueFailOnRead, std::move(read));
        }
    }

    bool HasResult(ui64 cookie) override {
        const auto& state = CookieToLookupState.at(cookie);
        return !state.ResolvePending && state.ReadsInflight == 0 && !state.Worker->AllRowsProcessed();
    }

    bool IsEmpty(ui64 cookie) override {
        const auto& state = CookieToLookupState.at(cookie);
        return !state.ResolvePending && state.ReadsInflight == 0 && state.Worker->AllRowsProcessed();
    }

    void ExtractResult(ui64 cookie, std::function<void(TConstArrayRef<TCell>)>&& callback) override {
        AFL_ENSURE(HasResult(cookie) || IsEmpty(cookie));
        auto& state = CookieToLookupState.at(cookie);
        auto& worker = state.Worker;

        const auto stats = worker->ReadAllResult(callback);
        ReadRowsCount += stats.ReadRowsCount;
        ReadBytesCount += stats.ReadBytesCount;
        AFL_ENSURE(worker->AllRowsProcessed());
    }

    TTableId GetTableId() const override {
        return Settings.TableId;
    }

    const TVector<NScheme::TTypeInfo>& GetKeyColumnTypes() const override {
        return KeyColumnTypes;
    }

    ui32 LookupColumnsCount(ui64 cookie) const override {
        return CookieToLookupState.at(cookie).LookupColumnsCount;
    }

    void StartTableRead(ui64 cookie, ui64 shardId, bool isUniqueCheck, bool failOnUniqueCheck, THolder<TEvDataShard::TEvRead> request) {
        Settings.Counters->CreatedIterators->Inc();
        auto& record = request->Record;

        Y_UNUSED(cookie);

        auto& worker = CookieToLookupState.at(cookie).Worker;

        YDB_LOG_DEBUG("Starting table read for buffer lookup",
            {"logPrefix", this->LogPrefix},
            {"table", worker->GetTablePath()},
            {"readId", record.GetReadId()},
            {"shardId", shardId});

        Settings.TxManager->AddShard(shardId, false, worker->GetTablePath());
        Settings.TxManager->AddAction(shardId, IKqpTransactionManager::EAction::READ);

        const auto& lookupState = CookieToLookupState.at(cookie);
        if (lookupState.MvccSnapshot) {
            record.MutableSnapshot()->SetStep(lookupState.MvccSnapshot->GetStep());
            record.MutableSnapshot()->SetTxId(lookupState.MvccSnapshot->GetTxId());
        }

        AFL_ENSURE(Settings.LockTxId && Settings.LockNodeId);
        record.SetLockTxId(Settings.LockTxId);
        record.SetLockNodeId(Settings.LockNodeId);

        record.SetLockMode((isUniqueCheck && Settings.LockMode == NKikimrDataEvents::OPTIMISTIC_SNAPSHOT_ISOLATION)
            ? NKikimrDataEvents::OPTIMISTIC // Workaround for Snapshot Isolation with Unique Index
            : Settings.LockMode);
        if (Settings.QuerySpanId) {
            record.SetQuerySpanId(Settings.QuerySpanId);
        }

        AFL_ENSURE(!failOnUniqueCheck || isUniqueCheck);

        if (failOnUniqueCheck) {
            record.SetTotalRowsLimit(1);
        }

        auto defaultSettings = GetDefaultReadSettings()->Record;
        record.SetMaxRows(defaultSettings.GetMaxRows());
        record.SetMaxBytes(defaultSettings.GetMaxBytes());
        record.SetResultFormat(NKikimrDataEvents::FORMAT_CELLVEC);

        YDB_LOG_DEBUG("Sending EvRead request for buffer lookup",
            {"logPrefix", this->LogPrefix},
            {"shardId", shardId},
            {"readId", record.GetReadId()},
            {"tablePath", worker->GetTablePath()},
            {"snapshotTxId", record.GetSnapshot().GetTxId()},
            {"step", record.GetSnapshot().GetStep()},
            {"lockTxId", record.GetLockTxId()},
            {"lockNodeId", record.GetLockNodeId()});

        auto& shardState = ShardToState[shardId];

        const bool needToCreatePipe = !shardState.HasPipe;

        const auto readId = record.GetReadId();

        Send(PipeCacheId,
            new TEvPipeCache::TEvForward(
                request.Release(),
                shardId,
                TEvPipeCache::TEvForwardOptions{
                    .AutoConnect = needToCreatePipe,
                    .Subscribe = needToCreatePipe,
                }),
            IEventHandle::FlagTrackDelivery,
            0,
            ShardReadTrace.Start(LookupActorSpan, shardId, readId));

        shardState.HasPipe = true;

        AFL_ENSURE(ReadIdToState.emplace(
            readId,
            TReadState {
                .LookupCookie = cookie,
                .ShardId = shardId,
                .LastSeqNo = 0,
                .IsUniqueCheck = isUniqueCheck,
                .FailOnUniqueCheck = failOnUniqueCheck,
                .Blocked = false,
            }).second);
    }

    void Handle(TEvDataShard::TEvReadResult::TPtr& ev) {
        const auto& record = ev->Get()->Record;

        auto readIt = ReadIdToState.find(record.GetReadId());
        if (readIt == ReadIdToState.end() || readIt->second.Blocked) {
            YDB_LOG_DEBUG("Dropping read because it is already completed or blocked",
                {"logPrefix", this->LogPrefix},
                {"readId", record.GetReadId()});
            return;
        }

        Settings.TxManager->AddParticipantNode(ev->Sender.NodeId());

        auto& read = readIt->second;
        ShardReadTrace.ReadResult(LookupActorSpan, read.ShardId, ev->Sender.NodeId(),
            record.GetReadId(), record.GetRowCount(), record.GetStatus().GetCode(), record.GetFinished());
        const auto shardId = read.ShardId;
        const auto cookie = read.LookupCookie;

        auto& shardState = ShardToState.at(shardId);

        auto& lookupState = CookieToLookupState.at(cookie);
        AFL_ENSURE(lookupState.Worker);
        AFL_ENSURE(lookupState.ReadsInflight > 0);

        TStringBuilder txLocks;
        for (const auto& lock : record.GetTxLocks()) {
            txLocks << lock.ShortDebugString();
        }

        TStringBuilder brokenTxLocks;
        for (const auto& lock : record.GetBrokenTxLocks()) {
            brokenTxLocks << lock.ShortDebugString();
        }

        YDB_LOG_DEBUG("Received TEvReadResult for buffer lookup",
            {"logPrefix", this->LogPrefix},
            {"shardID", shardId},
            {"tablePath", lookupState.Worker->GetTablePath()},
            {"readId", record.GetReadId()},
            {"currentReadId", ReadId},
            {"seqNo", record.GetSeqNo()},
            {"status", Ydb::StatusIds::StatusCode_Name(record.GetStatus().GetCode())},
            {"finished", record.GetFinished()},
            {"rowCount", record.GetRowCount()},
            {"txLocks", txLocks},
            {"brokenTxLocks", brokenTxLocks});

        if (!record.GetBrokenTxLocks().empty()) {
            BrokenLocksCount += record.GetBrokenTxLocks().size();
            Settings.TxManager->SetError(shardId);
            if (record.HasDeferredVictimQuerySpanId()) {
                Settings.TxManager->SetVictimQuerySpanId(record.GetDeferredVictimQuerySpanId());
            } else {
                SetVictimQuerySpanIdFromBrokenLocks(shardId, record.GetBrokenTxLocks(), Settings.TxManager);
            }
            RuntimeError(NYql::NDqProto::StatusIds::ABORTED,
                NYql::TIssuesIds::KIKIMR_LOCKS_INVALIDATED,
                MakeLockInvalidatedMessage(Settings.TxManager, lookupState.Worker->GetTablePath()));
            return;
        }

        for (const auto& lock : record.GetTxLocks()) {
            if (!Settings.TxManager->AddLock(shardId, lock, Settings.QuerySpanId)) {
                RuntimeError(NYql::NDqProto::StatusIds::ABORTED,
                    NYql::TIssuesIds::KIKIMR_LOCKS_INVALIDATED,
                    MakeLockInvalidatedMessage(Settings.TxManager, lookupState.Worker->GetTablePath()));
                return;
            }
        }

        auto getIssues = [&record]() {
            NYql::TIssues issues;
            NYql::IssuesFromMessage(record.GetStatus().GetIssues(), issues);
            return issues;
        };

        switch (record.GetStatus().GetCode()) {
            case Ydb::StatusIds::SUCCESS:
                break;
            case Ydb::StatusIds::NOT_FOUND:
            {
                YDB_LOG_DEBUG("Received NOT_FOUND status from datashard",
                    {"logPrefix", this->LogPrefix},
                    {"tablet", shardId},
                    {"issues", getIssues().ToOneLineString()});
                if (!RetryTableRead(record.GetReadId(), false)) {
                    return RuntimeError(
                        NYql::NDqProto::StatusIds::UNAVAILABLE,
                        NYql::TIssuesIds::KIKIMR_TEMPORARILY_UNAVAILABLE,
                        TStringBuilder() << "Table: `"
                            << lookupState.Worker->GetTablePath() << "` not found.",
                        getIssues());
                }
                return;
            }
            case Ydb::StatusIds::OVERLOADED: {
                YDB_LOG_DEBUG("Received OVERLOADED status from datashard",
                    {"logPrefix", this->LogPrefix},
                    {"tablet", shardId},
                    {"issues", getIssues().ToOneLineString()});
                const std::optional<TDuration> throttleDelay = record.HasThrottleDelayMs()
                    ? std::make_optional(TDuration::MilliSeconds(record.GetThrottleDelayMs()))
                    : std::nullopt;
                if (!RetryTableRead(record.GetReadId(), false, throttleDelay)) {
                    return RuntimeError(
                        NYql::NDqProto::StatusIds::OVERLOADED,
                        NYql::TIssuesIds::KIKIMR_OVERLOADED,
                        TStringBuilder() << "Table: `"
                            << lookupState.Worker->GetTablePath() << "` retry limit exceeded.",
                        getIssues());
                }
                return;
            }
            case Ydb::StatusIds::INTERNAL_ERROR: {
                YDB_LOG_DEBUG("Received INTERNAL_ERROR status from datashard",
                    {"logPrefix", this->LogPrefix},
                    {"tablet", shardId},
                    {"issues", getIssues().ToOneLineString()});
                if (!RetryTableRead(record.GetReadId(), true)) {
                    return RuntimeError(
                        NYql::NDqProto::StatusIds::INTERNAL_ERROR,
                        NYql::TIssuesIds::KIKIMR_INTERNAL_ERROR,
                        TStringBuilder() << "Table: `"
                            << lookupState.Worker->GetTablePath() << "` retry limit exceeded.",
                        getIssues());
                }
                return;
            }
            default: {
                return RuntimeError(
                        NYql::NDqProto::StatusIds::ABORTED,
                        NYql::TIssuesIds::KIKIMR_OPERATION_ABORTED,
                        "Read request aborted",
                        getIssues());
            }
        }

        Settings.Counters->DataShardIteratorMessages->Inc();
        if (record.GetStatus().GetCode() != Ydb::StatusIds::SUCCESS) {
            Settings.Counters->DataShardIteratorFails->Inc();
        }

        AFL_ENSURE(read.LastSeqNo < record.GetSeqNo());
        read.LastSeqNo = record.GetSeqNo();

        // Save values before potentially erasing the read state
        const bool failOnUniqueCheck = read.FailOnUniqueCheck;
        const bool finished = record.GetFinished();

        if (finished) {
            --lookupState.ReadsInflight;
            ReadIdToState.erase(readIt);
        } else {
            AFL_ENSURE(record.HasContinuationToken());
            NKikimrTxDataShard::TReadContinuationToken continuationToken;
            AFL_ENSURE(continuationToken.ParseFromString(record.GetContinuationToken()));
            AFL_ENSURE(!continuationToken.HasLastProcessedKey()); // can't read more than 1 row per range

            Settings.Counters->SentIteratorAcks->Inc();
            THolder<TEvDataShard::TEvReadAck> request(new TEvDataShard::TEvReadAck());
            request->Record.SetReadId(record.GetReadId());
            request->Record.SetSeqNo(record.GetSeqNo());

            auto defaultSettings = GetDefaultReadAckSettings()->Record;
            request->Record.SetMaxRows(defaultSettings.GetMaxRows());
            request->Record.SetMaxBytes(defaultSettings.GetMaxBytes());

            const bool needToCreatePipe = !shardState.HasPipe;

            Send(PipeCacheId,
                new TEvPipeCache::TEvForward(
                    request.Release(), shardId, TEvPipeCache::TEvForwardOptions{
                        .AutoConnect = needToCreatePipe,
                        .Subscribe = needToCreatePipe,
                    }),
                IEventHandle::FlagTrackDelivery,
                0,
                LookupActorSpan.GetTraceId());

            shardState.HasPipe = true;
            YDB_LOG_DEBUG("Sent TEvReadAck to datashard",
                {"logPrefix", this->LogPrefix},
                {"shard", shardId});
        }

        if (failOnUniqueCheck && record.GetRowCount() != 0) {
            return RuntimeError(
                    NYql::NDqProto::StatusIds::PRECONDITION_FAILED,
                    NYql::TIssuesIds::KIKIMR_PRECONDITION_FAILED,
                    "Conflict with existing key.");
        }

        {
            const auto guard = Settings.TypeEnv.BindAllocator();
            lookupState.Worker->AddResult(TStreamLookupShardReadResult(
                shardId, THolder<TEventHandle<TEvDataShard::TEvReadResult>>(ev.Release()), &guard.GetMutex()->Ref()
            ));
        }

        Settings.Callbacks->OnLookupTaskFinished();
    }

    void Handle(TEvPipeCache::TEvDeliveryProblem::TPtr& ev) {
        YDB_LOG_DEBUG("Received TEvDeliveryProblem from datashard",
            {"logPrefix", this->LogPrefix},
            {"tablet", ev->Get()->TabletId});
        ShardToState.at(ev->Get()->TabletId).HasPipe = false;

        TVector<ui64> toRetry;
        for (const auto& [readId, readState] : ReadIdToState) {
            if (readState.ShardId == ev->Get()->TabletId && !readState.Blocked) {
                Settings.Counters->IteratorDeliveryProblems->Inc();
                toRetry.push_back(readId);
            }
        }

        for (const auto& readId : toRetry) {
            if (!RetryTableRead(readId, true)) {
                return RuntimeError(
                    NYql::NDqProto::StatusIds::UNAVAILABLE,
                    NYql::TIssuesIds::KIKIMR_TEMPORARILY_UNAVAILABLE,
                    TStringBuilder() << "Table: `" << Settings.TablePath << "` retry limit exceeded.",
                    {});
            }
        }
    }

    void Handle(TEvPrivate::TEvRetryRead::TPtr& ev) {
        const ui64 failedReadId = ev->Get()->ReadId;
        auto readIt = ReadIdToState.find(failedReadId);
        if (readIt == ReadIdToState.end()) {
            YDB_LOG_DEBUG("Received retry request for already finished/non-existing read",
                {"logPrefix", this->LogPrefix},
                {"readId", failedReadId});
            return;
        }

        auto& failedRead = readIt->second;
        auto& lookupState = CookieToLookupState.at(failedRead.LookupCookie);
        YQL_ENSURE(!failedRead.Blocked || failedRead.LastSeqNo == ev->Get()->LastSeqNo);
        if (failedRead.LastSeqNo <= ev->Get()->LastSeqNo) {
            DoRetryTableRead(failedReadId, lookupState, failedRead);
        }
    }

    bool RetryTableRead(const ui64 failedReadId, bool allowInstantRetry, std::optional<TDuration> throttleDelay = std::nullopt) {
        auto& failedRead = ReadIdToState.at(failedReadId);
        auto& lookupState = CookieToLookupState.at(failedRead.LookupCookie);
        YDB_LOG_DEBUG("Retrying table read for buffer lookup",
            {"logPrefix", this->LogPrefix},
            {"table", lookupState.Worker->GetTablePath()},
            {"failedReadId", failedReadId},
            {"shardId", failedRead.ShardId});
        failedRead.Blocked = true;

        TDuration delay;
        if (!throttleDelay) {
            if (failedRead.RetryAttempts >= MaxShardRetries()) {
                YDB_LOG_DEBUG("Retry limit exceeded, resolving table shards",
                    {"logPrefix", this->LogPrefix},
                    {"table", lookupState.Worker->GetTablePath()},
                    {"failedReadId", failedReadId},
                    {"shardId", failedRead.ShardId});
                return HandleReadRetryExceeded(failedReadId, lookupState);
            }
            ++failedRead.RetryAttempts;
            delay = CalcDelay(failedRead.RetryAttempts, allowInstantRetry);
        } else {
            delay = *throttleDelay;
        }

        if (delay == TDuration::Zero()) {
            DoRetryTableRead(failedReadId, lookupState, failedRead);
        } else {
            TlsActivationContext->Schedule(
                delay, new IEventHandle(SelfId(), SelfId(), new TEvPrivate::TEvRetryRead(failedReadId, failedRead.LastSeqNo))
            );
        }

        return true;
    }

    void DoRetryTableRead(const ui64 failedReadId, TLookupState& lookupState, TReadState& failedRead) {
        AFL_ENSURE(failedRead.Blocked);
        ShardReadTrace.Retry(LookupActorSpan, failedRead.ShardId, failedReadId);
        --lookupState.ReadsInflight;
        const auto guard = Settings.TypeEnv.BindAllocator();
        lookupState.Worker->RebuildRequest(failedRead.ShardId, failedReadId, ReadId);
        while(true) {
            auto [shardId, request] = lookupState.Worker->PopNextRequest();
            if (!request)  {
                break;
            }

            const ui64 newReadId = request->Record.GetReadId();
            ++lookupState.ReadsInflight;
            StartTableRead(failedRead.LookupCookie, failedRead.ShardId, failedRead.IsUniqueCheck, failedRead.FailOnUniqueCheck, std::move(request));
            ReadIdToState.at(newReadId).RetryAttempts = failedRead.RetryAttempts;
        }
        ReadIdToState.erase(failedReadId);
    }

    bool HandleReadRetryExceeded(ui64 failedReadId, TLookupState& lookupState) {
        ShardReadTrace.Retry(LookupActorSpan, ReadIdToState.at(failedReadId).ShardId, failedReadId);
        --lookupState.ReadsInflight;
        const auto guard = Settings.TypeEnv.BindAllocator();
        lookupState.Worker->ResetRowsProcessing(failedReadId);
        ReadIdToState.erase(failedReadId);
        lookupState.ResolvePending = true;
        return ResolveTableShards();
    }

    bool ResolveTableShards() {
        if (ResolveShardsInProgress) {
            return true;
        }

        if (++TotalResolveShardsAttempts > MaxShardResolves()) {
            return false;
        }

        YDB_LOG_DEBUG("Resolve table shards",
            {"logPrefix", this->LogPrefix},
            {"table", Settings.TablePath});
        ResolveShardsInProgress = true;

        Partitioning.reset();

        auto request = MakeHolder<NSchemeCache::TSchemeCacheRequest>();
        request->DatabaseName = Settings.Database;

        TVector<TCell> minusInf(KeyColumnTypes.size());
        TVector<TCell> plusInf;
        TTableRange range(minusInf, true, plusInf, true, false);

        request->ResultSet.emplace_back(MakeHolder<TKeyDesc>(Settings.TableId, range, TKeyDesc::ERowOperation::Read,
            KeyColumnTypes, TVector<TKeyDesc::TColumnOp>{}));

        Settings.Counters->IteratorsShardResolve->Inc();

        Send(MakeSchemeCacheID(), new TEvTxProxySchemeCache::TEvResolveKeySet(request), 0, 0, LookupActorSpan.GetTraceId());
        return true;
    }

    void Handle(TEvTxProxySchemeCache::TEvResolveKeySetResult::TPtr& ev) {
        YDB_LOG_DEBUG("Received TEvResolveKeySetResult",
            {"logPrefix", this->LogPrefix},
            {"table", Settings.TablePath});
        if (!ResolveShardsInProgress) {
            return;
        }
        ResolveShardsInProgress = false;
        if (ev->Get()->Request->ErrorCount > 0) {
            TString errorMsg = TStringBuilder() << "Failed to get partitioning for table: " << Settings.TablePath;
            return RuntimeError(
                NYql::NDqProto::StatusIds::SCHEME_ERROR,
                NYql::TIssuesIds::KIKIMR_SCHEME_MISMATCH,
                errorMsg);
        }

        auto& resultSet = ev->Get()->Request->ResultSet;
        YQL_ENSURE(resultSet.size() == 1, "Expected one result for range [NULL, +inf)");
        Partitioning = resultSet[0].KeyDescription->Partitioning;

        ProcessResolveResult();
    }

    void ProcessResolveResult() {
        for (auto& [cookie, state] : CookieToLookupState) {
            if (state.ResolvePending) {
                state.ResolvePending = false;
                const auto guard = Settings.TypeEnv.BindAllocator();
                StartLookupTask(cookie, state, state.IsUniqueCheck, state.FailOnUniqueCheck);

                if (state.ReadsInflight == 0) {
                    RuntimeError(
                        NYql::NDqProto::StatusIds::INTERNAL_ERROR,
                        NYql::TIssuesIds::KIKIMR_INTERNAL_ERROR,
                        TStringBuilder() << "Table: `" << Settings.TablePath
                            << "` re-resolve dispatched no read requests.");
                    return;
                }
            }
        }
    }

    void RuntimeError(
            NYql::NDqProto::StatusIds::StatusCode statusCode,
            NYql::EYqlIssueCode id,
            const TString& message,
            const NYql::TIssues& subIssues = {}) {
        FinishLookupTrace(Ydb::StatusIds::GENERIC_ERROR, &message);
        Settings.Callbacks->OnLookupError(statusCode, id, message, subIssues);
    }

    void FinishLookupTrace(Ydb::StatusIds::StatusCode status, const TString* errorMessage = nullptr) {
        if (!LookupActorSpan) {
            return;
        }
        ShardReadTrace.Finish(LookupActorSpan);
        if (errorMessage) {
            LookupActorSpan.EndError(*errorMessage);
        } else {
            EndQueryTraceSpan(LookupActorSpan, status);
        }
    }

    void FillStats(NYql::NDqProto::TDqTaskStats* stats) override {
        NYql::NDqProto::TDqTableStats* tableStats = nullptr;
        for (size_t i = 0; i < stats->TablesSize(); ++i) {
            auto* table = stats->MutableTables(i);
            if (table->GetTablePath() == Settings.TablePath) {
                tableStats = table;
            }
        }
        if (!tableStats) {
            tableStats = stats->AddTables();
            tableStats->SetTablePath(Settings.TablePath);
        }

        tableStats->SetReadRows(tableStats->GetReadRows() + ReadRowsCount);
        tableStats->SetReadBytes(tableStats->GetReadBytes() + ReadBytesCount);

        ReadRowsCount = 0;
        ReadBytesCount = 0;

        // Add lock stats for broken locks
        if (BrokenLocksCount > 0) {
            NKqpProto::TKqpTaskExtraStats extraStats;
            if (stats->HasExtra()) {
                stats->GetExtra().UnpackTo(&extraStats);
            }
            extraStats.MutableLockStats()->SetBrokenAsVictim(
                extraStats.GetLockStats().GetBrokenAsVictim() + BrokenLocksCount);
            stats->MutableExtra()->PackFrom(extraStats);
            BrokenLocksCount = 0;
        }
    }

private:
    void ClearAllWorkerResults() {
        AFL_ENSURE(Settings.Alloc);
        TGuard<NMiniKQL::TScopedAlloc> allocGuard(*Settings.Alloc);
        for (auto& [cookie, state] : CookieToLookupState) {
            if (state.Worker) {
                state.Worker->ClearResults(Settings.Alloc->Ref());
            }
        }
        CookieToLookupState.clear();
    }

    TKqpBufferTableLookupSettings Settings;
    TPartitioning::TCPtr Partitioning;
    const TString LogPrefix;
    TVector<NScheme::TTypeInfo> KeyColumnTypes;

    const TActorId PipeCacheId = NKikimr::MakePipePerNodeCacheID(false);

    THashMap<ui64, TLookupState> CookieToLookupState;
    THashMap<ui64, TShardState> ShardToState;
    THashMap<ui64, TReadState> ReadIdToState;

    ui64 ReadId = 0;

    ui64 TotalResolveShardsAttempts = 0;
    bool ResolveShardsInProgress = false;

    // stats
    ui64 ReadRowsCount = 0;
    ui64 ReadBytesCount = 0;
    ui64 BrokenLocksCount = 0;

    TShardReadTrace ShardReadTrace;
    NWilson::TSpan LookupActorSpan;
};

// Full-primary-key ColumnShard read used as the old-image input for index maintenance.
// The read carries the transaction snapshot (when the caller has one) and the same
// lock id/mode the write uses, and the acquired locks are registered on the transaction manager.
class TKqpBufferOlapLookupActor : public NActors::TActorBootstrapped<TKqpBufferOlapLookupActor>, public IKqpBufferTableLookup {
    struct TRequest {
        TVector<NKikimrKqp::TKqpColumnMetadataProto> KeyColumns;
        TVector<NKikimrKqp::TKqpColumnMetadataProto> LookupColumns;
        std::optional<NKikimrDataEvents::TMvccSnapshot> MvccSnapshot;
        std::vector<TOwnedCellVec> Keys;
        TOwnedCellVecBatch Rows;
        ui64 Inflight = 0;
        bool Started = false;
        bool Extracted = false;
        ui32 LookupColumnsCount = 0;
    };

    struct TScan {
        ui64 Cookie = 0;
        ui64 ShardId = 0;
        ui32 Generation = 1;
        TActorId ScanActor;
        bool Finished = false;
    };

public:
    explicit TKqpBufferOlapLookupActor(TKqpBufferTableLookupSettings&& settings)
        : Settings(std::move(settings))
        , LogPrefix(TStringBuilder() << "Table: `" << Settings.TablePath << "` (" << Settings.TableId << "), "
            << "SessionActorId: " << Settings.SessionActorId)
    {
        AFL_ENSURE(Settings.IsOlap);
    }

    void Bootstrap() {
        Become(&TKqpBufferOlapLookupActor::StateFunc);
    }

    static constexpr char ActorName[] = "KQP_BUFFER_OLAP_LOOKUP_ACTOR";

    void SetLookupSettings(
            ui64 cookie,
            size_t lookupKeyPrefix,
            TConstArrayRef<NKikimrKqp::TKqpColumnMetadataProto> keyColumns,
            TConstArrayRef<NKikimrKqp::TKqpColumnMetadataProto> lookupColumns,
            const std::optional<NKikimrDataEvents::TMvccSnapshot>& mvccSnapshot,
            const NWilson::TTraceId& traceId) override {
        Y_UNUSED(lookupKeyPrefix);
        Y_UNUSED(traceId);
        AFL_ENSURE(!Failed);
        auto& request = Requests[cookie];
        AFL_ENSURE(!request.Started);
        request.KeyColumns.assign(keyColumns.begin(), keyColumns.end());
        request.LookupColumns.assign(lookupColumns.begin(), lookupColumns.end());
        request.MvccSnapshot = mvccSnapshot;
        request.LookupColumnsCount = request.LookupColumns.size();
        if (KeyColumnTypes.empty()) {
            for (const auto& column : request.KeyColumns) {
                KeyColumnTypes.push_back(NScheme::TypeInfoFromProto(column.GetTypeId(), column.GetTypeInfo()));
            }
        }
        AFL_ENSURE(KeyColumnTypes.size() == request.KeyColumns.size());
    }

    void AddLookupTask(ui64 cookie, const std::vector<TConstArrayRef<TCell>>& keys) override {
        AFL_ENSURE(!Failed);
        auto& request = Requests.at(cookie);
        AFL_ENSURE(!request.Started);
        request.Started = true;
        request.Keys.reserve(keys.size());
        for (const auto& key : keys) {
            AFL_ENSURE(key.size() == request.KeyColumns.size());
            request.Keys.emplace_back(TOwnedCellVec::Make(key));
        }
        if (request.Keys.empty()) {
            return;
        }
        if (!Sharding) {
            PendingCookies.push_back(cookie);
            if (!ResolveInProgress) {
                ResolveTable();
            }
            return;
        }
        StartScans(cookie);
    }

    void AddUniqueCheckTask(ui64, const std::vector<TConstArrayRef<TCell>>&, bool) override {
        RuntimeError(
            NYql::NDqProto::StatusIds::INTERNAL_ERROR,
            NYql::TIssuesIds::KIKIMR_INTERNAL_ERROR,
            "Unique-index checks are DataShard reads; a column base lookup is a full-primary-key read.");
    }

    bool IsRequestReady(const TRequest& request, ui64 cookie) const {
        if (!request.Started || request.Inflight != 0) {
            return false;
        }
        for (const ui64 pending : PendingCookies) {
            if (pending == cookie) {
                return false;
            }
        }
        return true;
    }

    bool HasResult(ui64 cookie) override {
        if (Failed) {
            return false;
        }
        const auto& request = Requests.at(cookie);
        return IsRequestReady(request, cookie) && !request.Extracted && !request.Rows.Empty();
    }

    bool IsEmpty(ui64 cookie) override {
        if (Failed) {
            return true;
        }
        const auto& request = Requests.at(cookie);
        return IsRequestReady(request, cookie) && (request.Extracted || request.Rows.Empty());
    }

    void ExtractResult(ui64 cookie, std::function<void(TConstArrayRef<TCell>)>&& callback) override {
        AFL_ENSURE(HasResult(cookie) || IsEmpty(cookie));
        auto& request = Requests.at(cookie);
        for (const auto& row : request.Rows) {
            callback(row);
            ++ReadRowsCount;
        }
        request.Rows = TOwnedCellVecBatch();
        request.Extracted = true;
    }

    TTableId GetTableId() const override {
        return Settings.TableId;
    }

    const TVector<NScheme::TTypeInfo>& GetKeyColumnTypes() const override {
        return KeyColumnTypes;
    }

    ui32 LookupColumnsCount(ui64 cookie) const override {
        return Requests.at(cookie).LookupColumnsCount;
    }

    void FillStats(NYql::NDqProto::TDqTaskStats* stats) override {
        auto* tableStats = stats->AddTables();
        tableStats->SetTablePath(Settings.TablePath);
        tableStats->SetReadRows(ReadRowsCount);
        ReadRowsCount = 0;
    }

    void Terminate() override {
        PassAway();
    }

    void Unlink() override {
        Send(PipeCacheId, new TEvPipeCache::TEvUnlink(0));
    }

private:
    STFUNC(StateFunc) {
        try {
            switch (ev->GetTypeRewrite()) {
                hFunc(TEvTxProxySchemeCache::TEvNavigateKeySetResult, Handle);
                hFunc(TEvKqpCompute::TEvScanInitActor, Handle);
                hFunc(TEvKqpCompute::TEvScanData, Handle);
                hFunc(TEvKqpCompute::TEvScanError, Handle);
                hFunc(TEvPipeCache::TEvDeliveryProblem, Handle);
                IgnoreFunc(TEvKqpCompute::TEvScanPing);
                IgnoreFunc(TEvInterconnect::TEvNodeConnected);
                IgnoreFunc(TEvInterconnect::TEvNodeDisconnected);
                hFunc(TEvents::TEvUndelivered, Handle);
            default:
                RuntimeError(
                    NYql::NDqProto::StatusIds::INTERNAL_ERROR,
                    NYql::TIssuesIds::KIKIMR_INTERNAL_ERROR,
                    TStringBuilder() << "Unexpected event in column lookup: " << ev->GetTypeRewrite());
            }
        } catch (...) {
            RuntimeError(
                NYql::NDqProto::StatusIds::INTERNAL_ERROR,
                NYql::TIssuesIds::KIKIMR_INTERNAL_ERROR,
                CurrentExceptionMessage());
        }
    }

    void ResolveTable() {
        ResolveInProgress = true;
        TAutoPtr<NSchemeCache::TSchemeCacheNavigate> request(new NSchemeCache::TSchemeCacheNavigate());
        request->DatabaseName = Settings.Database;
        NSchemeCache::TSchemeCacheNavigate::TEntry entry;
        entry.TableId = Settings.TableId;
        entry.RequestType = NSchemeCache::TSchemeCacheNavigate::TEntry::ERequestType::ByTableId;
        entry.Operation = NSchemeCache::TSchemeCacheNavigate::OpTable;
        entry.SyncVersion = false;
        entry.ShowPrivatePath = true;
        request->ResultSet.emplace_back(entry);
        Send(MakeSchemeCacheID(), new TEvTxProxySchemeCache::TEvNavigateKeySet(request));
    }

    void Handle(TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr& ev) {
        if (Failed) {
            return;
        }
        auto* response = ev->Get()->Request.Get();
        if (response->ErrorCount > 0 || response->ResultSet.size() != 1) {
            RuntimeError(
                NYql::NDqProto::StatusIds::SCHEME_ERROR,
                NYql::TIssuesIds::KIKIMR_SCHEME_ERROR,
                TStringBuilder() << "Failed to resolve column table `" << Settings.TablePath << "`.");
            return;
        }
        const auto& entry = response->ResultSet[0];
        if (entry.Kind != NSchemeCache::TSchemeCacheNavigate::KindColumnTable || !entry.ColumnTableInfo) {
            RuntimeError(
                NYql::NDqProto::StatusIds::SCHEME_ERROR,
                NYql::TIssuesIds::KIKIMR_SCHEME_ERROR,
                TStringBuilder() << "Table `" << Settings.TablePath << "` is not a column table.");
            return;
        }
        const auto& description = entry.ColumnTableInfo->Description;
        if (!description.HasSchema() || !description.HasSharding()) {
            RuntimeError(
                NYql::NDqProto::StatusIds::SCHEME_ERROR,
                NYql::TIssuesIds::KIKIMR_SCHEME_ERROR,
                TStringBuilder() << "Column table `" << Settings.TablePath << "` has no sharding.");
            return;
        }
        NSchemeShard::TOlapSchema olapSchema;
        olapSchema.ParseFromLocalDB(description.GetSchema());
        auto sharding = NSharding::IShardingBase::BuildFromProto(olapSchema, description.GetSharding());
        if (sharding.IsFail()) {
            RuntimeError(
                NYql::NDqProto::StatusIds::SCHEME_ERROR,
                NYql::TIssuesIds::KIKIMR_SCHEME_ERROR,
                TStringBuilder() << "Failed to build column sharding for `" << Settings.TablePath << "`: "
                    << sharding.GetErrorMessage());
            return;
        }
        Sharding = sharding.DetachResult();
        if (!Sharding) {
            RuntimeError(
                NYql::NDqProto::StatusIds::SCHEME_ERROR,
                NYql::TIssuesIds::KIKIMR_SCHEME_ERROR,
                TStringBuilder() << "Column table `" << Settings.TablePath << "` has empty sharding.");
            return;
        }
        ResolveInProgress = false;
        auto pending = std::move(PendingCookies);
        for (const ui64 cookie : pending) {
            StartScans(cookie);
        }
        if (!Failed) {
            Settings.Callbacks->OnLookupTaskFinished();
        }
    }

    void StartScans(ui64 cookie) {
        if (Failed) {
            return;
        }
        auto& request = Requests.at(cookie);
        AFL_ENSURE(Sharding);
        AFL_ENSURE(request.Inflight == 0);
        if (request.Keys.empty()) {
            Settings.Callbacks->OnLookupTaskFinished();
            return;
        }

        std::vector<std::pair<TString, NScheme::TTypeInfo>> keySchema;
        std::set<std::string> notNull;
        keySchema.reserve(request.KeyColumns.size());
        for (const auto& column : request.KeyColumns) {
            keySchema.emplace_back(column.GetName(), NScheme::TypeInfoFromProto(column.GetTypeId(), column.GetTypeInfo()));
            notNull.insert(column.GetName());
        }

        NArrow::TArrowBatchBuilder builder(arrow::Compression::UNCOMPRESSED, notNull, arrow::default_memory_pool());
        TString error;
        if (!builder.Start(keySchema, 0, 0, error)) {
            RuntimeError(NYql::NDqProto::StatusIds::INTERNAL_ERROR, NYql::TIssuesIds::KIKIMR_INTERNAL_ERROR, error);
            return;
        }
        for (const auto& key : request.Keys) {
            builder.AddRow(TConstArrayRef<TCell>(key));
        }
        auto keyBatch = builder.FlushBatch(true, true);
        auto shards = Sharding->MakeSharding(keyBatch);
        std::vector<bool> assigned(request.Keys.size(), false);
        for (const auto& [shardId, indexes] : shards) {
            std::vector<TConstArrayRef<TCell>> shardKeys;
            shardKeys.reserve(indexes.size());
            for (const ui32 index : indexes) {
                if (index >= request.Keys.size() || assigned[index]) {
                    RuntimeError(
                        NYql::NDqProto::StatusIds::INTERNAL_ERROR,
                        NYql::TIssuesIds::KIKIMR_INTERNAL_ERROR,
                        "Column sharding returned an unexpected key.");
                    return;
                }
                assigned[index] = true;
                shardKeys.emplace_back(request.Keys[index]);
            }
            if (!shardKeys.empty()) {
                SendScan(cookie, shardId, shardKeys);
            }
        }
        for (const bool seen : assigned) {
            if (!seen) {
                RuntimeError(
                    NYql::NDqProto::StatusIds::INTERNAL_ERROR,
                    NYql::TIssuesIds::KIKIMR_INTERNAL_ERROR,
                    TStringBuilder() << "Column sharding did not cover a primary key of `" << Settings.TablePath << "`.");
                return;
            }
        }
        AFL_ENSURE(request.Inflight > 0);
    }

    void SendScan(ui64 cookie, ui64 shardId, const std::vector<TConstArrayRef<TCell>>& keys) {
        auto& request = Requests.at(cookie);
        const ui64 scanId = ++NextScanId;
        auto ev = std::make_unique<TEvDataShard::TEvKqpScan>();
        ev->Record.SetLocalPathId(Settings.TableId.PathId.LocalPathId);
        ev->Record.SetTablePath(Settings.TablePath);
        ev->Record.SetSchemaVersion(Settings.TableId.SchemaVersion);
        ev->Record.SetScanId(scanId);
        ev->Record.SetGeneration(1);
        ev->Record.SetTxId(Settings.LockTxId);
        AFL_ENSURE(Settings.LockTxId && Settings.LockNodeId);
        ev->Record.SetLockTxId(Settings.LockTxId);
        ev->Record.SetLockNodeId(Settings.LockNodeId);
        ev->Record.SetLockMode(Settings.LockMode);
        if (request.MvccSnapshot) {
            ev->Record.MutableSnapshot()->SetStep(request.MvccSnapshot->GetStep());
            ev->Record.MutableSnapshot()->SetTxId(request.MvccSnapshot->GetTxId());
        }
        ev->Record.SetDataFormat(NKikimrDataEvents::FORMAT_ARROW);

        auto addColumn = [&](const NKikimrKqp::TKqpColumnMetadataProto& column) {
            ev->Record.AddColumnTags(column.GetId());
            ev->Record.AddColumnTypes(column.GetTypeId());
            if (column.HasTypeInfo()) {
                *ev->Record.AddColumnTypeInfos() = column.GetTypeInfo();
            } else {
                *ev->Record.AddColumnTypeInfos() = NKikimrProto::TTypeInfo();
            }
        };
        for (const auto& column : request.KeyColumns) {
            addColumn(column);
        }
        for (const auto& column : request.LookupColumns) {
            addColumn(column);
        }

        auto* ranges = ev->Record.MutableRanges();
        for (const auto& key : keys) {
            TSerializedTableRange range(key, true, key, true);
            range.Point = true;
            range.Serialize(*ranges->Add());
        }

        Settings.TxManager->AddShard(shardId, true, Settings.TablePath);
        Settings.TxManager->AddAction(shardId, IKqpTransactionManager::EAction::READ, Settings.QuerySpanId);

        Scans.emplace(scanId, TScan{.Cookie = cookie, .ShardId = shardId});
        ++request.Inflight;
        Send(PipeCacheId, new TEvPipeCache::TEvForward(ev.release(), shardId, true), IEventHandle::FlagTrackDelivery, scanId);
    }

    void Handle(TEvKqpCompute::TEvScanInitActor::TPtr& ev) {
        if (Failed) {
            return;
        }
        auto scanIt = Scans.find(ev->Get()->Record.GetScanId());
        if (scanIt == Scans.end() || scanIt->second.Finished) {
            return;
        }
        scanIt->second.Generation = ev->Get()->Record.GetGeneration();
        scanIt->second.ScanActor = ActorIdFromProto(ev->Get()->Record.GetScanActorId());
        Send(scanIt->second.ScanActor, new TEvKqpCompute::TEvScanDataAck(1ull << 30, scanIt->second.Generation));
    }

    void Handle(TEvKqpCompute::TEvScanData::TPtr& ev) {
        if (Failed) {
            return;
        }
        auto scanIt = Scans.find(ev->Get()->ScanId);
        if (scanIt == Scans.end() || scanIt->second.Finished) {
            return;
        }
        auto& scan = scanIt->second;
        if (scan.ScanActor != TActorId() && ev->Get()->Generation != scan.Generation) {
            return;
        }
        auto& request = Requests.at(scan.Cookie);
        try {
            ConsumeScanRows(request, *ev->Get());
            ConsumeScanLocks(scan.ShardId, *ev->Get());
        } catch (const std::exception& ex) {
            RuntimeError(NYql::NDqProto::StatusIds::INTERNAL_ERROR, NYql::TIssuesIds::KIKIMR_INTERNAL_ERROR, ex.what());
            return;
        }
        if (!ev->Get()->Finished) {
            Send(ev->Sender, new TEvKqpCompute::TEvScanDataAck(1ull << 30, ev->Get()->Generation));
            return;
        }
        FinishScan(scanIt);
    }

    void Handle(TEvKqpCompute::TEvScanError::TPtr& ev) {
        if (Failed) {
            return;
        }
        NYql::TIssues issues;
        NYql::IssuesFromMessage(ev->Get()->Record.GetIssues(), issues);
        RuntimeError(
            NYql::NDqProto::StatusIds::ABORTED,
            NYql::TIssuesIds::KIKIMR_OPERATION_ABORTED,
            TStringBuilder() << "ColumnShard read failed for `" << Settings.TablePath << "`.",
            issues);
    }

    void Handle(TEvPipeCache::TEvDeliveryProblem::TPtr& ev) {
        if (Failed) {
            return;
        }
        RuntimeError(
            NYql::NDqProto::StatusIds::UNAVAILABLE,
            NYql::TIssuesIds::KIKIMR_TEMPORARILY_UNAVAILABLE,
            TStringBuilder() << "ColumnShard " << ev->Get()->TabletId << " is unavailable for `" << Settings.TablePath << "`.");
    }

    void Handle(TEvents::TEvUndelivered::TPtr& ev) {
        if (Failed) {
            return;
        }
        RuntimeError(
            NYql::NDqProto::StatusIds::UNAVAILABLE,
            NYql::TIssuesIds::KIKIMR_TEMPORARILY_UNAVAILABLE,
            TStringBuilder() << "ColumnShard read was not delivered, reason " << ev->Get()->Reason << ".");
    }

    void ConsumeScanRows(TRequest& request, TEvKqpCompute::TEvScanData& data) {
        if (!data.Rows.empty()) {
            for (const auto& row : data.Rows) {
                AFL_ENSURE(row.size() == request.KeyColumns.size() + request.LookupColumns.size());
                request.Rows.Append(row);
            }
            return;
        }
        if (!data.ArrowBatch || data.ArrowBatch->num_rows() == 0) {
            return;
        }
        std::vector<std::pair<TString, NScheme::TTypeInfo>> schema;
        auto add = [&](const NKikimrKqp::TKqpColumnMetadataProto& column) {
            schema.emplace_back(column.GetName(), NScheme::TypeInfoFromProto(column.GetTypeId(), column.GetTypeInfo()));
        };
        for (const auto& column : request.KeyColumns) {
            add(column);
        }
        for (const auto& column : request.LookupColumns) {
            add(column);
        }
        auto fields = NArrow::TStatusValidator::GetValid(NArrow::MakeArrowFields(schema, {}));
        auto renamedSchema = std::make_shared<arrow::Schema>(std::move(fields));
        for (const auto& batch : NArrow::SliceToRecordBatches(data.ArrowBatch)) {
            AFL_ENSURE(static_cast<size_t>(batch->num_columns()) == schema.size());
            auto renamed = arrow::RecordBatch::Make(renamedSchema, batch->num_rows(), batch->columns());
            AFL_ENSURE(renamed);
            struct TCopyRows : NArrow::IRowWriter {
                explicit TCopyRows(TOwnedCellVecBatch& out)
                    : Out(out)
                {
                }

                TOwnedCellVecBatch& Out;
                void AddRow(const TConstArrayRef<TCell>& cells) override {
                    Out.Append(cells);
                }
            } writer(request.Rows);
            NArrow::TArrowToYdbConverter converter(schema, writer, false, false);
            TString error;
            if (!converter.Process(*renamed, error)) {
                ythrow yexception() << "Cannot read column batch: " << error;
            }
        }
    }

    void ConsumeScanLocks(ui64 shardId, const TEvKqpCompute::TEvScanData& data) {
        Settings.TxManager->AddShard(shardId, true, Settings.TablePath);
        auto accept = [&](const NKikimrDataEvents::TLock& lock) {
            if (!Settings.TxManager->AddLock(shardId, lock, Settings.QuerySpanId)) {
                ythrow yexception() << "ColumnShard lock was invalidated for `" << Settings.TablePath << "`.";
            }
        };
        for (const auto& lock : data.LocksInfo.Locks) {
            accept(lock);
        }
        for (const auto& lock : data.LocksInfo.BrokenLocks) {
            accept(lock);
        }
    }

    void FinishScan(THashMap<ui64, TScan>::iterator scanIt) {
        scanIt->second.Finished = true;
        auto& request = Requests.at(scanIt->second.Cookie);
        AFL_ENSURE(request.Inflight > 0);
        --request.Inflight;
        Scans.erase(scanIt);
        if (request.Inflight == 0) {
            Settings.Callbacks->OnLookupTaskFinished();
        }
    }

    void RuntimeError(
            NYql::NDqProto::StatusIds::StatusCode statusCode,
            NYql::EYqlIssueCode id,
            const TString& message,
            const NYql::TIssues& subIssues = {}) {
        if (Failed) {
            return;
        }
        Failed = true;
        Settings.Callbacks->OnLookupError(statusCode, id, message, subIssues);
    }

    void PassAway() override {
        Scans.clear();
        Unlink();
        TActorBootstrapped<TKqpBufferOlapLookupActor>::PassAway();
    }

    TKqpBufferTableLookupSettings Settings;
    const TString LogPrefix;
    const TActorId PipeCacheId = MakePipePerNodeCacheID(false);
    TVector<NScheme::TTypeInfo> KeyColumnTypes;
    std::unique_ptr<NSharding::IShardingBase> Sharding;
    bool ResolveInProgress = false;
    bool Failed = false;
    std::vector<ui64> PendingCookies;
    THashMap<ui64, TRequest> Requests;
    THashMap<ui64, TScan> Scans;
    ui64 NextScanId = 0;
    ui64 ReadRowsCount = 0;
};

}

std::pair<IKqpBufferTableLookup*, NActors::IActor*> CreateKqpBufferTableLookup(TKqpBufferTableLookupSettings&& settings) {
    if (settings.IsOlap) {
        auto* ptr = new TKqpBufferOlapLookupActor(std::move(settings));
        return {ptr, ptr};
    }
    auto* ptr = new TKqpBufferLookupActor(std::move(settings));
    return {ptr, ptr};
}

}
}
