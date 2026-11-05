LIBRARY()

SRCS(
    table_digest.cpp
)

PEERDIR(
    contrib/libs/openssl
    ydb/core/scheme
    yql/essentials/minikql
    yql/essentials/minikql/computation
    yql/essentials/public/decimal
    yql/essentials/udfs/common/digest/lib
)

YQL_LAST_ABI_VERSION()

END()

RECURSE_FOR_TESTS(
    ut
)
