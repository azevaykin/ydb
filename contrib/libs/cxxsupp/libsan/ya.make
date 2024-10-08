LIBRARY()

WITHOUT_LICENSE_TEXTS()

VERSION(Service-proxy-version)

LICENSE(YandexOpen)

NO_PLATFORM()

NO_SANITIZE()

NO_SANITIZE_COVERAGE()

SUBSCRIBER(somov)

RUN_PYTHON3(
    generate_symbolizer.py ${CXX_COMPILER}
    STDOUT symbolizer.c
)

CFLAGS(-fPIC)

SRCS(
    GLOBAL inject.c
)

END()
