# Generated by devtools/yamaker from nixpkgs 24.05.

LIBRARY()

LICENSE(BSL-1.0)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

VERSION(1.88.0)

ORIGINAL_SOURCE(https://github.com/boostorg/date_time/archive/boost-1.88.0.tar.gz)

PEERDIR(
    contrib/restricted/boost/algorithm
    contrib/restricted/boost/assert
    contrib/restricted/boost/config
    contrib/restricted/boost/core
    contrib/restricted/boost/io
    contrib/restricted/boost/lexical_cast
    contrib/restricted/boost/numeric_conversion
    contrib/restricted/boost/range
    contrib/restricted/boost/smart_ptr
    contrib/restricted/boost/static_assert
    contrib/restricted/boost/throw_exception
    contrib/restricted/boost/tokenizer
    contrib/restricted/boost/type_traits
    contrib/restricted/boost/utility
    contrib/restricted/boost/winapi
)

ADDINCL(
    GLOBAL contrib/restricted/boost/date_time/include
)

NO_COMPILER_WARNINGS()

NO_UTIL()

IF (DYNAMIC_BOOST)
    CFLAGS(
        GLOBAL -DBOOST_DATE_TIME_DYN_LINK
    )
ENDIF()

END()
