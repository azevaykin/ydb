SELECT Digest::AggregateFinalize(Digest::AggregateAddValue(
    Digest::AggregateCreate("CityHash128"), AsTuple(1ul, 2u)));
