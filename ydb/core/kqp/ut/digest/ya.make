UNITTEST_FOR(ydb/core/kqp)

FORK_SUBTESTS()
SIZE(MEDIUM)
REQUIREMENTS(cpu:1)

SRCS(
    native_yql_ut.cpp
    digest_ut.cpp
)

PEERDIR(
    ydb/core/kqp
    ydb/core/kqp/ut/common
    ydb/core/scheme/table_digest
    contrib/libs/openssl
    library/cpp/resource
    yql/essentials/udfs/common/digest/lib
    yql/essentials/udfs/common/digest
    yql/essentials/sql/pg_dummy
    yql/essentials/types/binary_json
    yql/essentials/types/dynumber
)

RESOURCE(
    yql/essentials/udfs/common/digest/aggregate.sql digest_factory.sql
)

YQL_LAST_ABI_VERSION()

END()
