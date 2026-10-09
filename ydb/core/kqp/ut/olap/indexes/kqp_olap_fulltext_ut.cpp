#include <ydb/core/kqp/ut/common/kqp_ut_common.h>
#include <ydb/core/kqp/ut/common/columnshard.h>
#include <ydb/core/tx/schemeshard/index/column_fulltext_seed.h>
#include <ydb/core/testlib/actors/block_events.h>
#include <ydb/core/base/fulltext.h>
#include <ydb/core/base/table_index.h>
#include <ydb/core/base/tablet_pipecache.h>
#include <ydb/core/testlib/tablet_helpers.h>
#include <ydb/core/testlib/test_client.h>
#include <ydb/library/actors/testlib/test_runtime.h>

#include <library/cpp/testing/unittest/registar.h>

namespace NKikimr::NKqp {

using namespace NYdb;
using namespace NYdb::NTable;

namespace {
bool IssuesContain(const auto& issues, TStringBuf needle) {
    return TString(issues.ToString()).Contains(needle);
}

void ExecScheme(TKikimrRunner& kikimr, const TString& query) {
    auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
    auto result = session.ExecuteSchemeQuery(query).GetValueSync();
    UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::SUCCESS, result.GetIssues().ToString());
}

auto ExecQuery(TKikimrRunner& kikimr, const TString& query) {
    auto session = kikimr.GetQueryClient().GetSession().GetValueSync().GetSession();
    auto result = session.ExecuteQuery(query, NYdb::NQuery::TTxControl::NoTx()).ExtractValueSync();
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
        SELECT Id FROM `/Root/LocalFtLogs` VIEW message_idx
        WHERE FulltextMatch(Message, "brown fox")
        ORDER BY Id;
    )");
    UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(result.GetResultSet(0)), "[[1u]]");
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
    TKikimrRunner kikimr(MakeColumnGlobalFulltextSettings());
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
    seedBlock.Stop();
    auto alter = alterFuture.GetValue();
    UNIT_ASSERT_VALUES_EQUAL_C(alter.GetStatus(), NYdb::EStatus::SUCCESS, alter.GetIssues().ToString());
    auto result = ExecQuery(kikimr, R"(
        SELECT Id FROM `/Root/GlobalFtRace` VIEW message_idx
        WHERE FulltextMatch(Message, "brown fox")
        ORDER BY Id;
    )");
    UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(result.GetResultSet(0)), "[]");
}

Y_UNIT_TEST(ColumnGlobalFulltextSeedResumeAfterSchemeShardReboot) {
    TKikimrRunner kikimr(MakeColumnGlobalFulltextSettings());
    auto& runtime = *kikimr.GetTestServer().GetRuntime();
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
    seedBlock.Stop();
    RebootTablet(runtime, TTestTxConfig::SchemeShard, runtime.AllocateEdgeActor());
    auto alter = alterFuture.GetValue();
    UNIT_ASSERT_VALUES_EQUAL_C(alter.GetStatus(), NYdb::EStatus::SUCCESS, alter.GetIssues().ToString());
    auto result = ExecQuery(kikimr, R"(
        SELECT Id FROM `/Root/GlobalFtReboot` VIEW message_idx
        WHERE FulltextMatch(Message, "brown fox")
        ORDER BY Id;
    )");
    UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(result.GetResultSet(0)), "[[1u]]");
}

} // Y_UNIT_TEST_SUITE(KqpOlapFulltext)

}
