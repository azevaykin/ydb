# Fulltext search

The `FulltextContains` and `FulltextScore` functions are intended for fulltext search in {{ ydb-short-name }} **via a fulltext index**.

{% note info %}

These functions use a fulltext index only when the query is executed with `VIEW IndexName`. See [{#T}](../syntax/select/fulltext_index.md) and [{#T}](../../../../dev/fulltext-indexes.md).

{% endnote %}

## FulltextContains {#fulltext-contains}

`FulltextContains(text, query)` filters rows by whether the text matches a fulltext query.

```yql
SELECT id, body
FROM articles VIEW ft_idx
WHERE FulltextContains(body, "machine learning");
```

Only the first two arguments can be positional. Additional parameters must be passed as **named arguments**.

Supported named arguments:

* `Mode` (String): `"or"` or `"and"`
* `MinimumShouldMatch` (String): for `"or"` mode, minimum number of matched terms (absolute like `"3"` or percent like `"50%"`)

### MinimumShouldMatch example {#minimum-should-match-example}

An 8-term query where at least half of the terms must match:

```yql
SELECT id
FROM articles VIEW ft_idx
WHERE FulltextContains(
  body,
  "machine learning neural networks deep learning large scale data recommendations search",
  "or" AS Mode,
  "50%" AS MinimumShouldMatch
);
```

In this example, `Mode="or"` allows matching any subset of terms, while `MinimumShouldMatch="50%"` requires at least 4 out of 8 terms to match. This helps filter out documents that contain only one or two words from a long query.

## FulltextScore {#fulltext-score}

`FulltextScore(text, query)` returns a relevance score based on the [BM25](https://en.wikipedia.org/wiki/Okapi_BM25) algorithm and can be used for ranking results.

Requires the `fulltext_relevance` index type.

```yql
SELECT id, FulltextScore(body, "quick fox") AS relevance
FROM articles VIEW ft_idx
ORDER BY relevance DESC
LIMIT 10;
```

Only the first two arguments can be positional. Additional parameters must be passed as **named arguments**.

Supported named arguments:

* `Mode` (String): `"or"` or `"and"`
* `MinimumShouldMatch` (String): for `"or"` mode, minimum number of matched terms (absolute or percentage)
* `K1` (Double): [BM25](https://en.wikipedia.org/wiki/Okapi_BM25) \(k_1\) parameter
* `B` (Double): [BM25](https://en.wikipedia.org/wiki/Okapi_BM25) \(b\) parameter

Example:

```yql
SELECT id,
       FulltextScore(body, "quick fox", "or" AS Mode, "1" AS MinimumShouldMatch, 0.75 AS K1, 1.2 AS B) AS relevance
FROM articles VIEW ft_idx
ORDER BY relevance DESC;
```

{% note info %}

`MinimumShouldMatch` is supported for `"or"` mode. For `"and"` mode, use `Mode="and"` without `MinimumShouldMatch`.

{% endnote %}
