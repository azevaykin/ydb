SELECT Digest::AggregateFinalize(Digest::AggregateAddValue(
    Digest::AggregateCreate("CityHash64"), AsTuple(1ul, 0ul)));
