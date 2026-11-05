YQL_UDF_MINITEST()

DEPENDS(
    yql/essentials/udfs/common/digest/impl
)

DATA(
    arcadia/yql/essentials/udfs/common/digest/aggregate.sql
)

TIMEOUT(300)

SIZE(MEDIUM)

END()
