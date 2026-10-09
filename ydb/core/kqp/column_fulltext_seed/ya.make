YQL_LIBRARY()

SRCS(
    kqp_column_fulltext_seed.cpp
)

PEERDIR(
    ydb/core/base
    ydb/core/engine/minikql
    ydb/core/formats
    ydb/core/kqp/common
    ydb/core/kqp/compute_actor
    ydb/core/kqp/counters
    ydb/core/kqp/runtime
    ydb/core/protos
    ydb/core/scheme
    ydb/core/tx/datashard
    ydb/core/tx/scheme_cache
    ydb/core/tx/schemeshard/index
    ydb/core/tx/sequenceproxy/public
    ydb/core/tx/tx_proxy
    ydb/library/actors/core
    ydb/library/formats/arrow
    yql/essentials/minikql
    yql/essentials/minikql/computation
    yql/essentials/public/issue
)

END()
