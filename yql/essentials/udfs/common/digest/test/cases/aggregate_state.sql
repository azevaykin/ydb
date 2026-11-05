$zero8 = "\x00\x00\x00\x00\x00\x00\x00\x00";
$ones8 = "\xff\xff\xff\xff\xff\xff\xff\xff";

$check = ($algorithm, $first, $second, $expected) -> {
    $empty = Digest::AggregateCreate($algorithm);
    $left = Digest::AggregateAddValue($empty, $first);
    $right = Digest::AggregateAddValue($empty, $second);
    $merged = Digest::AggregateFinalize(Digest::AggregateMerge($left, $right));
    RETURN Digest::AggregateFinalize($empty).count = 0ul
        AND Digest::AggregateFinalize($left).count = 1ul
        AND Digest::AggregateFinalize($right).count = 1ul
        AND $merged.count = 2ul
        AND $merged.hash = $expected;
};

SELECT
    $check("CityHash64", 18446744073709551615ul, 1ul, $zero8) AS city64,
    $check("CityHash128", AsTuple(18446744073709551615ul, 0ul), AsTuple(1ul, 0ul),
        "\x00\x00\x00\x00\x00\x00\x00\x01" || $zero8) AS city128,
    $check("Sha256", $zero8 || $zero8 || $zero8 || $ones8,
        $zero8 || $zero8 || $zero8 || "\x00\x00\x00\x00\x00\x00\x00\x01",
        $zero8 || $zero8 || "\x00\x00\x00\x00\x00\x00\x00\x01" || $zero8) AS sha256;
