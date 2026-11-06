YQL_UDF_YDB(digest_aggregate_udf)

YQL_ABI_VERSION(
    2
    43
    0
)

SRCS(
    udf.cpp
)

PEERDIR(
    ydb/library/table_digest
)

END()

RECURSE_FOR_TESTS(
    test
)
