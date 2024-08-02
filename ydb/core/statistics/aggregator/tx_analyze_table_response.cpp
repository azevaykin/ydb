#include "aggregator_impl.h"

#include <ydb/core/tx/datashard/datashard.h>

namespace NKikimr::NStat {

struct TStatisticsAggregator::TTxAnalyzeTableResponse : public TTxBase {
    const NKikimrStat::TEvAnalyze& Record;
    TActorId ReplyToActorId;    

    TTxAnalyzeTableResponse(TSelf* self, const NKikimrStat::TEvAnalyze& record, TActorId replyToActorId)
        : TTxBase(self)
        , Record(record)
        , ReplyToActorId(replyToActorId)
    {}

    TTxType GetTxType() const override { return TXTYPE_ANALYZE_TABLE_RESPONSE; }

    bool Execute(TTransactionContext& txc, const TActorContext&) override {
        SA_LOG_D("[" << Self->TabletID() << "] TTxAnalyzeTableResponse::Execute");

        if (!Self->EnableColumnStatistics) {
            return true;
        }

        NIceDb::TNiceDb db(txc.DB);
        Self->PersistTraversalOperationIdAndCookie(db);

        const ui64 cookie = Record.GetCookie();
        
        for (const auto& table : Record.GetTables()) {
            const TPathId pathId = PathIdFromPathId(table.GetPathId());

            // drop request with the same cookie and path from this sender
            if (std::any_of(Self->ForceTraversals.begin(), Self->ForceTraversals.end(), 
                [this, &pathId, &cookie](const TForceTraversal& elem) { 
                    return elem.PathId == pathId 
                        && elem.Cookie == cookie
                        && elem.ReplyToActorId == ReplyToActorId
                    ;})) {
                return true;
            }

            // create new force trasersal
            TForceTraversal operation {
                .OperationId = Self->NextForceTraversalOperationId,
                .Cookie = cookie,
                .PathId = pathId,
                .ReplyToActorId = ReplyToActorId
            };
            Self->ForceTraversals.emplace_back(operation);


            db.Table<Schema::ForceTraversals>().Key(Self->NextForceTraversalOperationId, pathId.OwnerId, pathId.LocalPathId).Update(
                NIceDb::TUpdate<Schema::ForceTraversals::OperationId>(Self->NextForceTraversalOperationId),
                NIceDb::TUpdate<Schema::ForceTraversals::OwnerId>(pathId.OwnerId),
                NIceDb::TUpdate<Schema::ForceTraversals::LocalPathId>(pathId.LocalPathId),
                NIceDb::TUpdate<Schema::ForceTraversals::Cookie>(cookie));
        }

        Self->PersistNextForceTraversalOperationId(db);

        return true;
    }

    void Complete(const TActorContext& /*ctx*/) override {
        SA_LOG_D("[" << Self->TabletID() << "] TTxAnalyzeTableResponse::Complete");
    }
};

void TStatisticsAggregator::Handle(TEvStatistics::TEvAnalyzeTableResponse::TPtr& ev) {
    ++NextForceTraversalOperationId;

    Execute(new TTxAnalyzeTableResponse(this, ev->Get()->Record, ev->Sender), TActivationContext::AsActorContext());
}

} // NKikimr::NStat
