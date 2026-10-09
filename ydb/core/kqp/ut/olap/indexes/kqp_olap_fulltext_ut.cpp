#include <ydb/core/kqp/ut/common/kqp_ut_common.h>
#include <ydb/core/kqp/ut/common/columnshard.h>
#include <ydb/core/base/fulltext.h>
#include <ydb/core/base/table_index.h>

#include <library/cpp/testing/unittest/registar.h>

namespace NKikimr::NKqp {

Y_UNIT_TEST_SUITE(KqpOlapFulltext) {

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
        UNIT_ASSERT_C(result.GetIssues().ToString().Contains(NKikimr::NFulltext::LocalFulltextIndexDisabled),
            result.GetIssues().ToString());
    }
}

Y_UNIT_TEST(LocalFulltextMatchTextFallback) {
    TKikimrRunner kikimr(MakeLocalFulltextSettings());
    auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
    {
        auto result = session.ExecuteSchemeQuery(R"(
            CREATE TABLE `/Root/LocalFtLogs` (
                Id Uint64 NOT NULL,
                Message Utf8,
                PRIMARY KEY (Id)
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
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::SUCCESS, result.GetIssues().ToString());
    }
    {
        auto result = session.ExecuteDataQuery(R"(
            UPSERT INTO `/Root/LocalFtLogs` (Id, Message) VALUES
                (1u, "quick brown fox"),
                (2u, "lazy dog");
        )", TTxControl::BeginTx().CommitTx()).GetValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::SUCCESS, result.GetIssues().ToString());
    }
    {
        auto result = session.ExecuteDataQuery(R"(
            SELECT Id FROM `/Root/LocalFtLogs` VIEW message_idx
            WHERE FulltextMatch(Message, "brown fox")
            ORDER BY Id;
        )", TTxControl::BeginTx().CommitTx()).GetValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::SUCCESS, result.GetIssues().ToString());
        UNIT_ASSERT_VALUES_EQUAL(FormatResultSetYson(result.GetResultSet(0)), "[[1u]]");
    }
}

Y_UNIT_TEST(ColumnGlobalFulltextBulkUpsertRejected) {
    TKikimrRunner kikimr(MakeColumnGlobalFulltextSettings());
    auto client = kikimr.GetTableClient();
    auto session = client.CreateSession().GetValueSync().GetSession();
    {
        auto result = session.ExecuteSchemeQuery(R"(
            CREATE TABLE `/Root/GlobalFtLogs` (
                Id Uint64 NOT NULL,
                Message Utf8,
                PRIMARY KEY (Id)
            ) WITH (STORE = COLUMN);
        )").GetValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::SUCCESS, result.GetIssues().ToString());
    }
    {
        auto result = session.ExecuteSchemeQuery(R"(
            ALTER TABLE `/Root/GlobalFtLogs`
              ADD INDEX message_idx GLOBAL USING fulltext_plain ON (Message)
              WITH (tokenizer = standard, use_filter_lowercase = true);
        )").GetValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::SUCCESS, result.GetIssues().ToString());
    }

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
        upsert.GetIssues().ToString().Contains(NKikimr::NTableIndex::ColumnTableGlobalFulltextBulkUpsertRejected),
        upsert.GetIssues().ToString());
}

Y_UNIT_TEST(LocalFulltextScoreRejected) {
    TKikimrRunner kikimr(MakeLocalFulltextSettings());
    auto session = kikimr.GetTableClient().CreateSession().GetValueSync().GetSession();
    {
        auto result = session.ExecuteSchemeQuery(R"(
            CREATE TABLE `/Root/LocalFtScore` (
                Id Uint64 NOT NULL,
                Message Utf8,
                PRIMARY KEY (Id)
            ) WITH (STORE = COLUMN);
        )").GetValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::SUCCESS, result.GetIssues().ToString());
    }
    {
        auto result = session.ExecuteSchemeQuery(R"(
            ALTER TABLE `/Root/LocalFtScore`
              ADD INDEX message_idx LOCAL USING fulltext ON (Message)
              WITH (tokenizer = standard, use_filter_lowercase = true);
        )").GetValueSync();
        UNIT_ASSERT_VALUES_EQUAL_C(result.GetStatus(), NYdb::EStatus::SUCCESS, result.GetIssues().ToString());
    }
    {
        auto result = session.ExecuteDataQuery(R"(
            SELECT Id, FulltextScore(Message, "fox") AS score
            FROM `/Root/LocalFtScore` VIEW message_idx
            WHERE FulltextScore(Message, "fox") > 0;
        )", TTxControl::BeginTx().CommitTx()).GetValueSync();
        UNIT_ASSERT_VALUES_UNEQUAL_C(result.GetStatus(), NYdb::EStatus::SUCCESS, result.GetIssues().ToString());
    }
}

} // Y_UNIT_TEST_SUITE(KqpOlapFulltext)

}
