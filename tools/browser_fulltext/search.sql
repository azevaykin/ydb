-- A nonempty, case-sensitive literal on one source line.
DECLARE $needle AS Utf8;

-- A 127-character overlap preserves every 128-character substring of a line.
$probe = Unicode::Substring($needle, 0u, 128u);
$whole_needle = Unicode::GetLength($needle) <= 128u;

-- Block IDs follow source order and are stored in the postings.
$first_keys = SELECT block_id
    FROM browser_source_blocks VIEW block_trigrams
    WHERE FulltextMatch(text, $probe)
    ORDER BY block_id LIMIT 20;

$page = SELECT COUNT(*) AS key_count, AGGREGATE_LIST_DISTINCT(IF(String::Contains(text, $probe),
        AsStruct(AsTuple(path, first_line) AS location))) AS hits
    FROM browser_source_blocks WHERE block_id IN $first_keys;

-- A nonempty page below the limit contains every candidate. A full page is
-- enough when 20 distinct groups match the whole needle. An empty page may
-- mean the probe has no indexable trigram, so let LIKE handle that case.
$verified_locations = SELECT IF(key_count > 0 AND
    (key_count < 20 OR (ListLength(hits) = 20 AND $whole_needle)), hits) FROM $page;

-- Escape LIKE metacharacters so the fallback also searches for a literal.
$literal = Unicode::ReplaceAll(Unicode::ReplaceAll(Unicode::ReplaceAll(
    $probe, "!"u, "!!"u), "%"u, "!%"u), "_"u, "!_"u);
$pattern = "%"u || $literal || "%"u;

$matches = SELECT block_id FROM browser_source_blocks VIEW block_trigrams
    WHERE text LIKE $pattern ESCAPE "!";
$fallback = SELECT DISTINCT AsTuple(path, first_line) AS location
    FROM browser_source_blocks WHERE block_id IN $matches
    ORDER BY location LIMIT IF($verified_locations IS NOT NULL, 0ul,
        IF($whole_needle, 20ul, NULL));

$candidates = SELECT location FROM AS_TABLE(COALESCE($verified_locations, []))
    UNION ALL SELECT location FROM $fallback;

$groups = SELECT path, first_line, MIN(block_id) AS first_id,
        AGGREGATE_LIST(AsTuple(block_id, text)) AS parts
    FROM browser_source_blocks VIEW by_location WHERE (path, first_line) IN $candidates
    GROUP BY path, first_line;

$lines = ($parts, $first_id) -> {
    $body = ($part) -> (IF($part.0 = $first_id, $part.1, Unicode::Substring($part.1, 127u)));
    $text = Unicode::JoinFromList(ListMap(ListSort($parts), $body), ""u);
    $normalized = Unicode::ReplaceAll($text, "\r\n"u, "\n"u);
    RETURN ListEnumerate(Unicode::SplitToList($normalized, "\r\n"u, false));
};

SELECT path, first_line + line.0 AS line_number, line.1 AS source_line
FROM $groups FLATTEN LIST BY ($lines(parts, first_id) AS line)
WHERE String::Contains(line.1, $needle)
ORDER BY path, line_number
LIMIT 20;
