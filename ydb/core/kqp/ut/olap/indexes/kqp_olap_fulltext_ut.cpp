#include <ydb/core/kqp/ut/common/kqp_ut_common.h>
#include <ydb/core/kqp/ut/common/columnshard.h>
#include <ydb/core/tx/schemeshard/index/column_fulltext_seed.h>
#include <ydb/core/testlib/actors/block_events.h>
#include <ydb/core/base/fulltext.h>
#include <ydb/core/base/counters.h>
#include <ydb/core/base/table_index.h>
#include <ydb/core/base/tablet_pipecache.h>
#include <ydb/core/testlib/tablet_helpers.h>
#include <ydb/core/testlib/test_client.h>
#include <ydb/library/actors/testlib/test_runtime.h>
#include <ydb/core/kqp/ut/olap/helpers/plan_step.h>
#include <ydb/core/tx/columnshard/test_helper/controllers.h>

#include <library/cpp/testing/unittest/registar.h>
#include <util/generic/scope.h>

namespace NKikimr::NKqp {

using namespace NYdb;
using namespace NYdb::NTable;

namespace {
bool IssuesContain(const auto& issues, TStringBuf needle) {
    return TString(issues.ToString()).Contains(needle);
}

void ExecScheme(TKikimrRunner& kikimr, const TString& query) {
    auto execute = [&] {
        auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
        return session.ExecuteSchemeQuery(query).GetValueSync();
    };
    auto result = kikimr.GetTestServer().GetRuntime()->IsRealThreads() ? execute() : kikimr.RunCall(execute);
    UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::SUCCESS, result.GetIssues().ToString());
}

auto ExecQuery(TKikimrRunner& kikimr, const TString& query) {
    auto execute = [&] {
        auto session = kikimr.GetQueryClient().GetSession().GetValueSync().GetSession();
        return session.ExecuteQuery(query, NYdb::NQuery::TTxControl::NoTx(),
            NYdb::NQuery::TExecuteQuerySettings().ClientTimeout(TDuration::Seconds(60))).ExtractValueSync();
    };
    auto result = kikimr.GetTestServer().GetRuntime()->IsRealThreads() ? execute() : kikimr.RunCall(execute);
    UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::SUCCESS, result.GetIssues().ToString());
    return result;
}
}

Y_UNIT_TEST_SUITE(KqpOlapFulltext) {

namespace {
struct TForceLinkColumnFulltextSeedInUt {
    TForceLinkColumnFulltextSeedInUt() {
        ForceLinkColumnFulltextSeed();
    }
} ForceLinkColumnFulltextSeedInUt;
}

static TKikimrSettings MakeLocalFulltextSettings() {
    auto settings = TKikimrSettings().SetColumnShardAlterObjectEnabled(true);
    settings.AppConfig.MutableFeatureFlags()->SetEnableLocalFulltextIndex(true);
    return settings;
}

static TKikimrSettings MakeColumnGlobalFulltextSettings() {
    auto settings = TKikimrSettings().SetColumnShardAlterObjectEnabled(true);
    settings.AppConfig.MutableFeatureFlags()->SetEnableColumnTableGlobalFulltextIndex(true);
    settings.AppConfig.MutableFeatureFlags()->SetEnableCompactFulltextIndex(true);
    return settings;
}

namespace {
class TFulltextReadController: public NOlap::TWaitCompactionController {
public:
    TAtomicCounter TextBlobBytes;

    void OnColumnBlobRead(const ui32 columnId, const ui64 bytes) override {
        // Id and Message are declared in this order in the lifecycle test.
        if (columnId == 2) {
            TextBlobBytes.Add(bytes);
        }
    }
};

void CheckLocalFulltextLifecycle(const TString& reader, const bool forceText) {
    auto settings = MakeLocalFulltextSettings();
    settings.AppConfig.MutableColumnShardConfig()->SetReaderClassName(reader);
    settings.AppConfig.MutableFeatureFlags()->SetForceLocalFulltextTextFallback(forceText);
    TKikimrRunner kikimr(settings);
    auto controller = NYDBTest::TControllers::RegisterCSControllerGuard<TFulltextReadController>();
    controller->SetCompactionControl(NYDBTest::EOptimizerCompactionWeightControl::Disable);
    const auto counters = GetServiceCounters(kikimr.GetTestServer().GetRuntime()->GetAppData().Counters, "tablets")
        ->GetSubgroup("subsystem", "columnshard")->GetSubgroup("module_id", "FulltextIndex");
    const auto indexed = counters->GetCounter("Deriviative/Read/IndexedChunks", true);
    const auto fallback = counters->GetCounter("Deriviative/Read/FallbackRows", true);
    ExecScheme(kikimr, R"(
        CREATE TABLE `/Root/LocalFtLifecycle` (
            Id Uint64 NOT NULL, Message Utf8, PRIMARY KEY (Id)
        ) WITH (STORE = COLUMN);
    )");
    ExecQuery(kikimr, R"(
        REPLACE INTO `/Root/LocalFtLifecycle` (Id, Message) VALUES
            (1u, "alpha old"), (2u, "beta");
    )");
    ExecScheme(kikimr, R"(
        ALTER TABLE `/Root/LocalFtLifecycle`
            ADD INDEX message_idx LOCAL USING fulltext ON (Message) WITH (tokenizer = whitespace);
        ALTER OBJECT `/Root/LocalFtLifecycle` (TYPE TABLE) SET (
            ACTION=UPSERT_OPTIONS, `INSERT_OPTIONS.BUILD_INDEXES_ENABLED`=`true`,
            `INSERT_OPTIONS.BUILD_INDEXES_MIN_BLOB_BYTES`=`0`);
    )");
    const auto check = [&](const TString& expected, const bool allIndexed) {
        const auto textBefore = controller->TextBlobBytes.Val();
        const auto indexedBefore = indexed->Val();
        const auto fallbackBefore = fallback->Val();
        const auto result = ExecQuery(kikimr, R"(
            SELECT Id FROM `/Root/LocalFtLifecycle` VIEW message_idx
            WHERE FulltextMatch(Message, "alpha") ORDER BY Id;
        )");
        UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(result.GetResultSet(0)), expected);
        if (allIndexed && !forceText) {
            UNIT_ASSERT_C(indexed->Val() > indexedBefore, "The scan must use stored postings");
            UNIT_ASSERT_VALUES_EQUAL(fallback->Val(), fallbackBefore);
            UNIT_ASSERT_VALUES_EQUAL(controller->TextBlobBytes.Val(), textBefore);
        } else {
            UNIT_ASSERT_C(fallback->Val() > fallbackBefore, "The scan must evaluate the text fallback");
            UNIT_ASSERT_C(controller->TextBlobBytes.Val() > textBefore, "Fallback must request text blob ranges");
        }
    };
    check("[[1u]]", false);

    // Mix an old portion without an index and a new indexed portion. Visibility
    // must resolve the repeated key before applying its fulltext predicate.
    ExecQuery(kikimr, R"(
        REPLACE INTO `/Root/LocalFtLifecycle` (Id, Message) VALUES
            (1u, "beta new"), (3u, "alpha new"), (4u, NULL);
    )");
    check("[[3u]]", false);
    ExecScheme(kikimr, R"(
        ALTER OBJECT `/Root/LocalFtLifecycle` (TYPE TABLE) SET (
            ACTION=UPSERT_OPTIONS, SCHEME_NEED_ACTUALIZATION=`true`);
    )");
    controller->WaitActualization(TDuration::Seconds(20), true);
    AdvancePlanStep(kikimr);
    check("[[3u]]", true);

    const auto compacted = controller->GetCompactionFinishedCounter().Val();
    // The default planner waits for larger batches. Make these deliberately
    // tiny portions eligible so the test observes a real completed compaction.
    ExecScheme(kikimr, R"(
        ALTER OBJECT `/Root/LocalFtLifecycle` (TYPE TABLE) SET (
            ACTION=UPSERT_OPTIONS, `COMPACTION_PLANNER.CLASS_NAME`=`lc-buckets`,
            `COMPACTION_PLANNER.FEATURES`=`{
                "levels": [{"class_name": "Zero", "portions_live_duration": "10s",
                            "expected_blobs_size": 2048000, "portions_count_available": 1},
                           {"class_name": "Zero"}]}`);
    )");
    controller->SetCompactionControl(NYDBTest::EOptimizerCompactionWeightControl::Force);
    controller->WaitCondition(TDuration::Seconds(30), [&] {
        return controller->GetCompactionFinishedCounter().Val() > compacted;
    });
    controller->WaitCompactions(TDuration::Seconds(5));
    controller->WaitCondition(TDuration::Seconds(30), [&] {
        return controller->GetCompactionFinishedCounter().Val() == controller->GetCompactionStartedCounter().Val();
    });
    AdvancePlanStep(kikimr);
    check("[[3u]]", true);
}
} // namespace

Y_UNIT_TEST_TWIN(LocalFulltextLifecycleSimple, forceText) {
    CheckLocalFulltextLifecycle("SIMPLE", forceText);
}

Y_UNIT_TEST_TWIN(LocalFulltextLifecycleTrivial, forceText) {
    CheckLocalFulltextLifecycle("TRIVIAL", forceText);
}

Y_UNIT_TEST(LocalFulltextDdlDisabledByDefault) {
    TKikimrRunner kikimr(TKikimrSettings().SetColumnShardAlterObjectEnabled(true));
    auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
    {
        auto result = session.ExecuteSchemeQuery(R"(
            CREATE TABLE `/Root/LocalFtLogs` (
                Ts Timestamp NOT NULL,
                Message Utf8,
                PRIMARY KEY (Ts)
            ) WITH (STORE = COLUMN);
        )").GetValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::SUCCESS, result.GetIssues().ToString());
    }
    {
        auto result = session.ExecuteSchemeQuery(R"(
            ALTER TABLE `/Root/LocalFtLogs`
              ADD INDEX message_idx LOCAL USING fulltext ON (Message)
              WITH (tokenizer = standard, use_filter_lowercase = true);
        )").GetValueSync();
        UNIT_ASSERT_VALUES_UNEQUAL_C(result.GetStatus(), NYdb::EStatus::SUCCESS, result.GetIssues().ToString());
        UNIT_ASSERT_C(IssuesContain(result.GetIssues(), NKikimr::NFulltext::LocalFulltextIndexDisabled),
            result.GetIssues().ToString());
    }
}

Y_UNIT_TEST(LocalFulltextMatchTextFallback) {
    TKikimrRunner kikimr(MakeLocalFulltextSettings());
    ExecScheme(kikimr, R"(
        CREATE TABLE `/Root/LocalFtLogs` (
            Id Uint64 NOT NULL,
            Message Utf8,
            PRIMARY KEY (Id)
        ) WITH (STORE = COLUMN);
    )");
    ExecScheme(kikimr, R"(
        ALTER TABLE `/Root/LocalFtLogs`
          ADD INDEX message_idx LOCAL USING fulltext ON (Message)
          WITH (tokenizer = standard, use_filter_lowercase = true);
    )");
    ExecQuery(kikimr, R"(
        UPSERT INTO `/Root/LocalFtLogs` (Id, Message) VALUES
            (1u, "quick brown fox"),
            (2u, "lazy dog");
    )");
    auto result = ExecQuery(kikimr, R"(
        PRAGMA kikimr.OptEnableOlapPushdown="false";
        SELECT Id FROM `/Root/LocalFtLogs` VIEW message_idx
        WHERE FulltextMatch(Message, "brown fox")
        ORDER BY Id;
    )");
    UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(result.GetResultSet(0)), "[[1u]]");
    auto session = kikimr.GetQueryClient().GetSession().GetValueSync().GetSession();
    const auto missingPredicate = session.ExecuteQuery(R"(
        SELECT DISTINCT Message FROM `/Root/LocalFtLogs` VIEW message_idx;
    )", NYdb::NQuery::TTxControl::NoTx()).ExtractValueSync();
    UNIT_ASSERT_C(!missingPredicate.IsSuccess(), "A fulltext VIEW requires a matching predicate");
    UNIT_ASSERT_C(IssuesContain(missingPredicate.GetIssues(), "requires FulltextMatch"), missingPredicate.GetIssues().ToString());
}

Y_UNIT_TEST(LocalFulltextBoundOptionsAndDictionaryDistinct) {
    for (const TString& reader : {TString("SIMPLE"), TString("TRIVIAL")}) {
        for (const bool forceText : {false, true}) {
            auto settings = MakeLocalFulltextSettings();
            settings.AppConfig.MutableColumnShardConfig()->SetReaderClassName(reader);
            settings.AppConfig.MutableFeatureFlags()->SetEnableCsDictionaryEncoding(true);
            settings.AppConfig.MutableFeatureFlags()->SetForceLocalFulltextTextFallback(forceText);
            TKikimrRunner kikimr(settings);
            ExecScheme(kikimr, R"(
                CREATE TABLE `/Root/LocalFtOptions` (
                    Id Uint64 NOT NULL,
                    Message Utf8 ENCODING(DICT),
                    PRIMARY KEY (Id)
                ) WITH (STORE = COLUMN);
                ALTER TABLE `/Root/LocalFtOptions`
                    ADD INDEX message_idx LOCAL USING fulltext ON (Message)
                    WITH (tokenizer = whitespace);
                ALTER OBJECT `/Root/LocalFtOptions` (TYPE TABLE) SET (
                    ACTION=UPSERT_OPTIONS, `INSERT_OPTIONS.BUILD_INDEXES_ENABLED`=`true`,
                    `INSERT_OPTIONS.BUILD_INDEXES_MIN_BLOB_BYTES`=`0`);
            )");
            ExecQuery(kikimr, R"(
                REPLACE INTO `/Root/LocalFtOptions` (Id, Message) VALUES
                    (1u, "alpha beta"), (2u, "alpha gamma"), (3u, "gamma"),
                    (4u, "alpha beta"), (5u, NULL);
            )");
            auto session = kikimr.GetQueryClient().GetSession().GetValueSync().GetSession();
            NYdb::TParamsBuilder params;
            params.AddParam("$query").Utf8("+alpha beta gamma").Build();
            params.AddParam("$operator").Utf8("Or").Build();
            params.AddParam("$threshold").Utf8("50%").Build();
            auto result = session.ExecuteQuery(R"(
                DECLARE $query AS Utf8;
                DECLARE $operator AS Utf8;
                DECLARE $threshold AS Utf8;
                SELECT DISTINCT Message FROM `/Root/LocalFtOptions` VIEW message_idx
                WHERE FulltextMatch(Message, $query, $operator AS DefaultOperator, $threshold AS MinimumShouldMatch)
                ORDER BY Message;
            )", NYdb::NQuery::TTxControl::NoTx(), params.Build()).ExtractValueSync();
            UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::SUCCESS, result.GetIssues().ToString());
            UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(result.GetResultSet(0)),
                "[[[\"alpha beta\"]];[[\"alpha gamma\"]]]");

            // A newer value must shadow old postings before the fulltext filter.
            ExecQuery(kikimr, R"(UPSERT INTO `/Root/LocalFtOptions` (Id, Message) VALUES (1u, "gamma"))");
            const auto updated = ExecQuery(kikimr, R"(
                SELECT Id FROM `/Root/LocalFtOptions` VIEW message_idx
                WHERE FulltextMatch(Message, "alpha beta") ORDER BY Id;
            )");
            UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(updated.GetResultSet(0)), "[[4u]]");

            NYdb::TParamsBuilder modeParams;
            modeParams.AddParam("$mode").Utf8("Keywords").Build();
            const auto invalidMode = session.ExecuteQuery(R"(
                DECLARE $mode AS Utf8;
                SELECT Id FROM `/Root/LocalFtOptions` VIEW message_idx
                WHERE FulltextMatch(Message, "alpha", $mode AS Mode);
            )", NYdb::NQuery::TTxControl::NoTx(), modeParams.Build()).ExtractValueSync();
            UNIT_ASSERT(!invalidMode.IsSuccess());
            UNIT_ASSERT_C(IssuesContain(invalidMode.GetIssues(), "Parameterized fulltext mode"), invalidMode.GetIssues().ToString());
        }
    }
}

Y_UNIT_TEST_TWIN(ColumnGlobalFulltextFetchesPayloadAndMaintainsChanges, synthetic) {
    TKikimrRunner kikimr(MakeColumnGlobalFulltextSettings());
    ExecScheme(kikimr, TStringBuilder() << R"(
        CREATE TABLE `/Root/GlobalFtFetch` (
            Id Uint64 NOT NULL,
            Tenant Utf8 NOT NULL,
            Message Utf8,
            PRIMARY KEY ()" << (synthetic ? "Tenant, Id" : "Id") << R"()
        ) WITH (STORE = COLUMN);
        ALTER TABLE `/Root/GlobalFtFetch`
            ADD INDEX message_idx GLOBAL USING fulltext_plain ON (Message)
            WITH (tokenizer = whitespace);
    )");
    ExecQuery(kikimr, R"(
        UPSERT INTO `/Root/GlobalFtFetch` (Id, Tenant, Message) VALUES
            (1u, "east", "alpha beta"), (2u, "west", "alpha gamma");
        UPSERT INTO `/Root/GlobalFtFetch` (Id, Tenant, Message) VALUES
            (3u, "north", "alpha");
        UPSERT INTO `/Root/GlobalFtFetch` (Id, Tenant, Message) VALUES
            (3u, "north", "epsilon");
    )");
    const auto result = ExecQuery(kikimr, R"(
        SELECT Id, Message FROM `/Root/GlobalFtFetch` VIEW message_idx
        WHERE FulltextMatch(Message, "alpha") ORDER BY Id;
    )");
    UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(result.GetResultSet(0)),
        "[[1u;[\"alpha beta\"]];[2u;[\"alpha gamma\"]]]");
    ExecQuery(kikimr, R"(
        UPSERT INTO `/Root/GlobalFtFetch` (Id, Tenant, Message) VALUES (1u, "east", "delta");
    )");
    const auto changed = ExecQuery(kikimr, R"(
        SELECT Id FROM `/Root/GlobalFtFetch` VIEW message_idx
        WHERE FulltextMatch(Message, "alpha") ORDER BY Id;
    )");
    UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(changed.GetResultSet(0)), "[[2u]]");
    ExecQuery(kikimr, R"(DELETE FROM `/Root/GlobalFtFetch` WHERE Id = 2u AND Tenant = "west")");
    const auto removed = ExecQuery(kikimr, R"(
        SELECT Id FROM `/Root/GlobalFtFetch` VIEW message_idx
        WHERE FulltextMatch(Message, "alpha") ORDER BY Id;
    )");
    UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(removed.GetResultSet(0)), "[]");
}

Y_UNIT_TEST(ColumnGlobalFulltextReadOwnWritesAndRollback) {
    TKikimrRunner kikimr(MakeColumnGlobalFulltextSettings());
    ExecScheme(kikimr, R"(
        CREATE TABLE `/Root/GlobalFtTransaction` (
            Id Uint64 NOT NULL,
            Message Utf8,
            PRIMARY KEY (Id)
        ) WITH (STORE = COLUMN);
        ALTER TABLE `/Root/GlobalFtTransaction`
            ADD INDEX message_idx GLOBAL USING fulltext_plain ON (Message)
            WITH (tokenizer = whitespace);
    )");
    ExecQuery(kikimr, R"(
        UPSERT INTO `/Root/GlobalFtTransaction` (Id, Message) VALUES (1u, "alpha");
    )");
    auto session = kikimr.GetQueryClient().GetSession().GetValueSync().GetSession();
    const auto querySettings = NYdb::NQuery::TExecuteQuerySettings().ClientTimeout(TDuration::Seconds(60));
    auto write = session.ExecuteQuery(R"(
        UPSERT INTO `/Root/GlobalFtTransaction` (Id, Message) VALUES (1u, "temporary");
        UPSERT INTO `/Root/GlobalFtTransaction` (Id, Message) VALUES (1u, "beta"), (2u, "beta");
    )", NYdb::NQuery::TTxControl::BeginTx(), querySettings).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL_C(write.GetStatus(), EStatus::SUCCESS, write.GetIssues().ToString());
    auto tx = write.GetTransaction();
    UNIT_ASSERT(tx);
    const auto ownWrites = session.ExecuteQuery(R"(
        SELECT Id, Message FROM `/Root/GlobalFtTransaction` VIEW message_idx
        WHERE FulltextMatch(Message, "beta") ORDER BY Id;
    )", NYdb::NQuery::TTxControl::Tx(*tx), querySettings).ExtractValueSync();
    UNIT_ASSERT_VALUES_EQUAL_C(ownWrites.GetStatus(), EStatus::SUCCESS, ownWrites.GetIssues().ToString());
    UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(ownWrites.GetResultSet(0)),
        "[[1u;[\"beta\"]];[2u;[\"beta\"]]]");
    const auto rollback = tx->Rollback().GetValueSync();
    UNIT_ASSERT_VALUES_EQUAL_C(rollback.GetStatus(), EStatus::SUCCESS, rollback.GetIssues().ToString());
    const auto afterRollback = ExecQuery(kikimr, R"(
        SELECT Id, Message FROM `/Root/GlobalFtTransaction` ORDER BY Id;
        SELECT Id FROM `/Root/GlobalFtTransaction` VIEW message_idx
        WHERE FulltextMatch(Message, "alpha") ORDER BY Id;
        SELECT Id FROM `/Root/GlobalFtTransaction` VIEW message_idx
        WHERE FulltextMatch(Message, "beta") ORDER BY Id;
    )");
    UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(afterRollback.GetResultSet(0)), "[[1u;[\"alpha\"]]]");
    UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(afterRollback.GetResultSet(1)), "[[1u]]");
    UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(afterRollback.GetResultSet(2)), "[]");
}

Y_UNIT_TEST(ColumnGlobalFulltextBulkUpsertRejected) {
    TKikimrRunner kikimr(MakeColumnGlobalFulltextSettings());
    auto client = kikimr.GetTableClient();
    ExecScheme(kikimr, R"(
        CREATE TABLE `/Root/GlobalFtLogs` (
            Id Uint64 NOT NULL,
            Message Utf8,
            PRIMARY KEY (Id)
        ) WITH (STORE = COLUMN);
    )");
    ExecScheme(kikimr, R"(
        ALTER TABLE `/Root/GlobalFtLogs`
          ADD INDEX message_idx GLOBAL USING fulltext_plain ON (Message)
          WITH (tokenizer = standard, use_filter_lowercase = true);
    )");

    NYdb::TValueBuilder rows;
    rows.BeginList();
    rows.AddListItem()
        .BeginStruct()
        .AddMember("Id").Uint64(1)
        .AddMember("Message").OptionalUtf8("hello")
        .EndStruct();
    rows.EndList();
    auto upsert = client.BulkUpsert("/Root/GlobalFtLogs", rows.Build()).GetValueSync();
    UNIT_ASSERT_VALUES_UNEQUAL_C(upsert.GetStatus(), NYdb::EStatus::SUCCESS, upsert.GetIssues().ToString());
    UNIT_ASSERT_C(
        IssuesContain(upsert.GetIssues(), NKikimr::NTableIndex::ColumnTableGlobalFulltextBulkUpsertRejected),
        upsert.GetIssues().ToString());
}

Y_UNIT_TEST(LocalFulltextScoreRejected) {
    TKikimrRunner kikimr(MakeLocalFulltextSettings());
    ExecScheme(kikimr, R"(
        CREATE TABLE `/Root/LocalFtScore` (
            Id Uint64 NOT NULL,
            Message Utf8,
            PRIMARY KEY (Id)
        ) WITH (STORE = COLUMN);
    )");
    ExecScheme(kikimr, R"(
        ALTER TABLE `/Root/LocalFtScore`
          ADD INDEX message_idx LOCAL USING fulltext ON (Message)
          WITH (tokenizer = standard, use_filter_lowercase = true);
    )");
    auto session = kikimr.GetQueryClient().GetSession().GetValueSync().GetSession();
    auto result = session.ExecuteQuery(R"(
        SELECT Id, FulltextScore(Message, "fox") AS score
        FROM `/Root/LocalFtScore` VIEW message_idx
        WHERE FulltextScore(Message, "fox") > 0;
    )", NYdb::NQuery::TTxControl::NoTx()).ExtractValueSync();
    UNIT_ASSERT_VALUES_UNEQUAL_C(result.GetStatus(), NYdb::EStatus::SUCCESS, result.GetIssues().ToString());
}

Y_UNIT_TEST(ColumnGlobalFulltextPreFenceSeed) {
    TKikimrRunner kikimr(MakeColumnGlobalFulltextSettings());
    ExecScheme(kikimr, R"(
        CREATE TABLE `/Root/GlobalFtSeed` (
            Id Uint64 NOT NULL,
            Message Utf8,
            PRIMARY KEY (Id)
        ) WITH (STORE = COLUMN);
    )");
    ExecQuery(kikimr, R"(
        UPSERT INTO `/Root/GlobalFtSeed` (Id, Message) VALUES
            (1u, "quick brown fox"),
            (2u, "lazy dog");
    )");
    ExecScheme(kikimr, R"(
        ALTER TABLE `/Root/GlobalFtSeed`
          ADD INDEX message_idx GLOBAL USING fulltext_plain ON (Message)
          WITH (tokenizer = standard, use_filter_lowercase = true);
    )");
    auto result = ExecQuery(kikimr, R"(
        SELECT Id FROM `/Root/GlobalFtSeed` VIEW message_idx
        WHERE FulltextMatch(Message, "brown fox")
        ORDER BY Id;
    )");
    UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(result.GetResultSet(0)), "[[1u]]");
}

Y_UNIT_TEST(ColumnGlobalFulltextDeleteRaceDuringSeed) {
    TKikimrRunner kikimr(MakeColumnGlobalFulltextSettings().SetUseRealThreads(false));
    auto& runtime = *kikimr.GetTestServer().GetRuntime();
    auto tableClient = kikimr.GetTableClient();
    ExecScheme(kikimr, R"(
        CREATE TABLE `/Root/GlobalFtRace` (
            Id Uint64 NOT NULL,
            Message Utf8,
            PRIMARY KEY (Id)
        ) WITH (STORE = COLUMN);
    )");
    ExecQuery(kikimr, R"(
        UPSERT INTO `/Root/GlobalFtRace` (Id, Message) VALUES
            (1u, "quick brown fox"),
            (2u, "lazy dog");
    )");

    NActors::TBlockEvents<TEvColumnFulltextSeed::TEvResponse> seedBlock(runtime);
    auto alterFuture = kikimr.RunInThreadPool([&] {
        auto s = tableClient.CreateSession().GetValueSync().GetSession();
        return s.ExecuteSchemeQuery(R"(
            ALTER TABLE `/Root/GlobalFtRace`
              ADD INDEX message_idx GLOBAL USING fulltext_plain ON (Message)
              WITH (tokenizer = standard, use_filter_lowercase = true);
        )").GetValueSync();
    });
    runtime.WaitFor("column fulltext seed progress", [&] {
        return !seedBlock.empty() || alterFuture.HasValue();
    }, TDuration::Seconds(60));
    UNIT_ASSERT_C(!seedBlock.empty(), "seed actor did not report progress");
    ExecQuery(kikimr, R"(
        DELETE FROM `/Root/GlobalFtRace` WHERE Id = 1u;
    )");
    seedBlock.Stop().Unblock();
    auto alter = runtime.WaitFuture(alterFuture);
    UNIT_ASSERT_VALUES_EQUAL_C(alter.GetStatus(), NYdb::EStatus::SUCCESS, alter.GetIssues().ToString());
    auto result = ExecQuery(kikimr, R"(
        SELECT Id FROM `/Root/GlobalFtRace` VIEW message_idx
        WHERE FulltextMatch(Message, "brown fox")
        ORDER BY Id;
    )");
    UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(result.GetResultSet(0)), "[]");
}

Y_UNIT_TEST(ColumnGlobalFulltextSeedResumeAfterSchemeShardReboot) {
    TKikimrRunner kikimr(MakeColumnGlobalFulltextSettings().SetUseRealThreads(false));
    auto& runtime = *kikimr.GetTestServer().GetRuntime();
    // Leave an event budget for SDK/session cleanup if the recovery wait fails.
    Y_DEFER { runtime.ResetScheduledCount(); };
    auto tableClient = kikimr.GetTableClient();
    ExecScheme(kikimr, R"(
        CREATE TABLE `/Root/GlobalFtReboot` (
            Id Uint64 NOT NULL,
            Message Utf8,
            PRIMARY KEY (Id)
        ) WITH (STORE = COLUMN);
    )");
    ExecQuery(kikimr, R"(
        UPSERT INTO `/Root/GlobalFtReboot` (Id, Message) VALUES
            (1u, "quick brown fox"),
            (2u, "lazy dog");
    )");

    NActors::TBlockEvents<TEvColumnFulltextSeed::TEvResponse> seedBlock(runtime);
    auto alterFuture = kikimr.RunInThreadPool([&] {
        auto s = tableClient.CreateSession().GetValueSync().GetSession();
        return s.ExecuteSchemeQuery(R"(
            ALTER TABLE `/Root/GlobalFtReboot`
              ADD INDEX message_idx GLOBAL USING fulltext_plain ON (Message)
              WITH (tokenizer = standard, use_filter_lowercase = true);
        )").GetValueSync();
    });
    runtime.WaitFor("column fulltext seed progress before reboot", [&] {
        return !seedBlock.empty() || alterFuture.HasValue();
    }, TDuration::Seconds(60));
    UNIT_ASSERT_C(!seedBlock.empty(), "seed actor did not report progress");
    const ui64 buildId = seedBlock.front()->Get()->BuildId;
    seedBlock.Stop();
    seedBlock.clear();
    const ui64 schemeShard = Tests::ChangeStateStorage(Tests::SchemeRoot, kikimr.GetTestServer().GetSettings().Domain);
    const auto edge = runtime.AllocateEdgeActor();
    RebootTablet(runtime, schemeShard, edge);
    auto alter = runtime.WaitFuture(alterFuture);
    UNIT_ASSERT_C(alter.IsSuccess() || IssuesContain(alter.GetIssues(), "Connection to tablet was lost"),
        alter.GetIssues().ToString());
    runtime.SendToPipe(schemeShard, edge, new NSchemeShard::TEvSchemeShard::TEvNotifyTxCompletion(buildId),
        0, GetPipeConfigWithRetries());
    runtime.GrabEdgeEventRethrow<NSchemeShard::TEvSchemeShard::TEvNotifyTxCompletionResult>(edge);
    auto result = ExecQuery(kikimr, R"(
        SELECT Id FROM `/Root/GlobalFtReboot` VIEW message_idx
        WHERE FulltextMatch(Message, "brown fox")
        ORDER BY Id;
    )");
    UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(result.GetResultSet(0)), "[[1u]]");
}

} // Y_UNIT_TEST_SUITE(KqpOlapFulltext)

}
