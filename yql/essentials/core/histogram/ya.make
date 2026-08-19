LIBRARY()

SRCS(
    eq_width_histogram.h
    eq_width_histogram.cpp
    eq_height_histogram.h
    eq_height_histogram.cpp
)

END()

RECURSE_FOR_TESTS(
    ut
)
