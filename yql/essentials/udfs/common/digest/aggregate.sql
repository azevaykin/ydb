$DigestFactory = ($algorithm) -> {
    $add = ($state, $row) -> {
        $bytes = StablePickle($row);
        RETURN CASE $algorithm
            WHEN "CityHash64" THEN Digest::AggregateAddValue($state, Digest::CityHash($bytes))
            WHEN "CityHash128" THEN Digest::AggregateAddValue($state, Digest::CityHash128($bytes))
            ELSE Digest::AggregateAddValue($state, Digest::Sha256($bytes))
        END;
    };

    RETURN AggregationFactory(
        "UDAF",
        ($row, $_parent) -> {
            RETURN $add(Digest::AggregateCreate($algorithm), $row);
        },
        ($state, $row, $_parent) -> { RETURN $add($state, $row); },
        Digest::AggregateMerge,
        Digest::AggregateFinalize,
        Digest::AggregateSerialize,
        Digest::AggregateDeserialize,
        Digest::AggregateFinalize(Digest::AggregateCreate($algorithm))
    );
};

EXPORT $DigestFactory;
