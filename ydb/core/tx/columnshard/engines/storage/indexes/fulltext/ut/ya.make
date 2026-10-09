UNITTEST_FOR(ydb/core/tx/columnshard/engines/storage/indexes/fulltext)

SIZE(MEDIUM)

SRCS(
    ut_fulltext_format.cpp
    ut_fulltext_posting_match.cpp
)

PEERDIR(
    contrib/libs/apache/arrow
    ydb/core/formats/arrow
    ydb/core/formats/arrow/accessor/plain
    ydb/core/formats/arrow/serializer
    library/cpp/testing/common
    yql/essentials/public/udf/service/stub
    yql/essentials/sql/pg_dummy
)

YQL_LAST_ABI_VERSION()

DATA(
    arcadia/ydb/core/tx/columnshard/engines/storage/indexes/fulltext/ut/format_v1.bin
)

END()
