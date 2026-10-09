LIBRARY()

SRCS(
    GLOBAL constructor.cpp
    GLOBAL meta.cpp
    GLOBAL format.cpp
    GLOBAL builder.cpp
    posting_match.cpp
)

PEERDIR(
    ydb/core/base
    ydb/core/protos
    ydb/public/lib/scheme_types
    ydb/core/formats/arrow
    ydb/core/formats/arrow/filter
    ydb/core/formats/arrow/accessor/abstract
    ydb/core/tx/columnshard/engines/scheme
    ydb/core/tx/columnshard/engines/scheme/indexes/abstract
    ydb/core/tx/columnshard/engines/storage/chunks
    ydb/core/tx/schemeshard/olap/schema
)

END()

RECURSE_FOR_TESTS(
    ut
)
