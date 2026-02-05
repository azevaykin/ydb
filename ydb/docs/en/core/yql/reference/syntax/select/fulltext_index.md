# VIEW (Fulltext index)

To select data from a row-oriented table using a [fulltext index](../../../../dev/fulltext-indexes.md), use the `VIEW` expression:

```yql
SELECT ...
FROM TableName VIEW IndexName
WHERE FulltextContains(TextColumn, "query")
ORDER BY ...
```

{% note info %}

A fulltext index isn't automatically selected by the [optimizer](../../../../concepts/glossary.md#optimizer) and must be specified explicitly using `VIEW IndexName`.

Fulltext search functions (`FulltextContains`, `FulltextScore`) require `VIEW`. If `VIEW` isn't used, the query fails.

For function details, see [{#T}](../../builtins/fulltext.md), including
[`FulltextContains`](../../builtins/fulltext.md#fulltext-contains) and
[`FulltextScore`](../../builtins/fulltext.md#fulltext-score).

{% endnote %}

## FulltextContains

`FulltextContains(text, query)` filters rows by whether the text matches the fulltext query:

```yql
SELECT id, title
FROM articles VIEW ft_idx
WHERE FulltextContains(body, "machine learning")
ORDER BY id;
```

### Wildcards (requires n-grams)

If the index is created with n-gram filtering, the `*` wildcard can be used inside query terms:

```yql
SELECT id, title
FROM articles VIEW ft_idx
WHERE FulltextContains(body, "mach* learn*")
ORDER BY id;
```

## FulltextScore ([BM25](https://en.wikipedia.org/wiki/Okapi_BM25) relevance)

`FulltextScore(text, query)` returns a relevance score and can be used for ranking.
Relevance scoring requires the `fulltext_relevance` index type.

```yql
SELECT id, title, FulltextScore(body, "quick fox") AS relevance
FROM articles VIEW ft_idx
ORDER BY relevance DESC
LIMIT 10;
```

### Optional parameters

Additional parameters must be passed as **named arguments**.

* `Mode` (String): `"or"` or `"and"`
* `MinimumShouldMatch` (String): for `"or"` mode, minimum number of matched terms (absolute like `"1"` or percent like `"50%"`)
* `K1` (Double): [BM25](https://en.wikipedia.org/wiki/Okapi_BM25) K1 parameter
* `B` (Double): [BM25](https://en.wikipedia.org/wiki/Okapi_BM25) B parameter

Example:

```yql
SELECT id, FulltextScore(body, "quick fox", "or" AS Mode, "50%" AS MinimumShouldMatch) AS relevance
FROM articles VIEW ft_idx
ORDER BY relevance DESC;
```

{% note info %}

Only the first two arguments of `FulltextContains` / `FulltextScore` can be positional. Use named arguments for all additional parameters.

{% endnote %}
