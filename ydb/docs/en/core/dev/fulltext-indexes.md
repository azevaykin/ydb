# Fulltext Indexes

Fulltext indexes are specialized data structures that enable efficient text search within table columns. Unlike [secondary indexes](../concepts/glossary.md#secondary-index), which optimize searching by equality or range, fulltext indexes allow searching by words, phrases, and (with n-grams) by substrings.

For the general idea of fulltext search, see [Fulltext search](../concepts/fulltext_search.md).

## Characteristics of Fulltext Indexes {#characteristics}

Fulltext indexes in {{ ydb-short-name }} are built by tokenizing text and creating an inverted index. This enables:

* fast filtering with `FulltextContains()`
* relevance ranking ([BM25](https://en.wikipedia.org/wiki/Okapi_BM25)) with `FulltextScore()` when using `fulltext_relevance`
* case normalization, stemming, and n-gram matching via index filters

The current implementation supports two index layouts:

* `fulltext_plain` — basic fulltext index
* `fulltext_relevance` — fulltext index with [BM25](https://en.wikipedia.org/wiki/Okapi_BM25) statistics for relevance scoring

Internally, a fulltext index consists of hidden index tables (for example, `indexImplTable`). These tables can appear in [query statistics](query-plans-optimization.md). Direct writes to index implementation tables are not allowed.

## Types of Fulltext Indexes {#types}

A fulltext index can be **covering**, meaning it includes additional columns to enable reading from the index without accessing the main table:

```yql
ALTER TABLE my_table
  ADD INDEX ft_idx
  GLOBAL USING fulltext_plain
  ON (content) COVER (title)
  WITH (tokenizer=standard, use_filter_lowercase=true);
```

## Index parameters {#parameters}

Fulltext index parameters are specified in the `WITH (...)` clause.
For the full list, see:

* [FULLTEXT INDEX (CREATE TABLE)](../yql/reference/syntax/create_table/fulltext_index.md)

## Creating Fulltext Indexes {#creation}

Fulltext indexes can be created:

* during table creation using [CREATE TABLE ... FULLTEXT INDEX](../yql/reference/syntax/create_table/fulltext_index.md);
* added to an existing table using [ALTER TABLE ... ADD INDEX](../yql/reference/syntax/alter_table/indexes.md).

## Using Fulltext Indexes {#select}

Queries to fulltext indexes are executed using the `VIEW` syntax in YQL.

### Filtering with FulltextContains

```yql
SELECT id, title
FROM my_table VIEW ft_idx
WHERE FulltextContains(content, "search terms")
ORDER BY id;
```

### Ranking with FulltextScore

Relevance scoring requires `fulltext_relevance`:

```yql
SELECT id, title, FulltextScore(content, "search terms") AS relevance
FROM my_table VIEW ft_idx
ORDER BY relevance DESC
LIMIT 10;
```

For full details and additional parameters, see:

* [VIEW (Fulltext index)](../yql/reference/syntax/select/fulltext_index.md)
* [Fulltext built-in functions](../yql/reference/builtins/fulltext.md)

{% note info %}

If the `VIEW` expression is not used, `FulltextContains` / `FulltextScore` queries will fail.

{% endnote %}

## Updating Fulltext Indexes {#update}

Fulltext indexes are maintained automatically on data modifications. Tables with fulltext indexes support:

* `INSERT`
* `UPSERT`
* `REPLACE`
* `UPDATE`
* `DELETE`

## Dropping Fulltext Indexes {#drop}

```yql
ALTER TABLE my_table DROP INDEX ft_idx;
```

## Limitations {#limitations}

* `BulkUpsert` isn't supported for tables with fulltext indexes.
* Fulltext index access must be specified explicitly using `VIEW IndexName`.
* Only one text column can be indexed (per fulltext index). Use `COVER` for additional columns.
