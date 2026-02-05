# Полнотекстовый поиск

Функции `FulltextContains` и `FulltextScore` предназначены для выполнения полнотекстового поиска в {{ ydb-short-name }} **через полнотекстовый индекс**.

{% note info %}

Эти функции используют полнотекстовый индекс только при выполнении запроса через `VIEW IndexName`. См. [{#T}](../syntax/select/fulltext_index.md) и [{#T}](../../../../dev/fulltext-indexes.md).

{% endnote %}

## FulltextContains {#fulltext-contains}

`FulltextContains(text, query)` фильтрует строки по совпадению текста с полнотекстовым запросом.

```yql
SELECT id, body
FROM articles VIEW ft_idx
WHERE FulltextContains(body, "машинное обучение");
```

Только первые два аргумента могут быть позиционными. Дополнительные параметры нужно передавать как **именованные аргументы**.

Поддерживаемые именованные аргументы:

* `Mode` (String): `"or"` или `"and"`
* `MinimumShouldMatch` (String): для режима `"or"` — минимальное число совпавших термов (абсолютное, например `"3"`, или процент, например `"50%"`)

### Пример для MinimumShouldMatch {#minimum-should-match-example}

Запрос из 8 слов, где требуется совпадение **не менее половины** слов:

```yql
SELECT id
FROM articles VIEW ft_idx
WHERE FulltextContains(
  body,
  "машинное обучение нейронные сети глубокое обучение большие данные рекомендации поиск",
  "or" AS Mode,
  "50%" AS MinimumShouldMatch
);
```

В этом примере `Mode="or"` разрешает совпадение по любому подмножеству термов, а `MinimumShouldMatch="50%"` требует, чтобы совпали **минимум 4 терма из 8**. Это помогает отсеять документы, где встретилось лишь одно-два слова из длинного запроса.

## FulltextScore {#fulltext-score}

`FulltextScore(text, query)` возвращает оценку релевантности на основе алгоритма [BM25](https://en.wikipedia.org/wiki/Okapi_BM25) и может использоваться для ранжирования результатов.

Требует индекс типа `fulltext_relevance`.

```yql
SELECT id, FulltextScore(body, "быстрый лис") AS relevance
FROM articles VIEW ft_idx
ORDER BY relevance DESC
LIMIT 10;
```

Только первые два аргумента могут быть позиционными. Дополнительные параметры нужно передавать как **именованные аргументы**.

Поддерживаемые именованные аргументы:

* `Mode` (String): `"or"` или `"and"`
* `MinimumShouldMatch` (String): для режима `"or"` — минимальное число совпавших термов (абсолютное или процент)
* `K1` (Double): параметр \(k_1\) алгоритма [BM25](https://en.wikipedia.org/wiki/Okapi_BM25)
* `B` (Double): параметр \(b\) алгоритма [BM25](https://en.wikipedia.org/wiki/Okapi_BM25)

Пример:

```yql
SELECT id,
       FulltextScore(body, "быстрый лис", "or" AS Mode, "1" AS MinimumShouldMatch, 0.75 AS K1, 1.2 AS B) AS relevance
FROM articles VIEW ft_idx
ORDER BY relevance DESC;
```

{% note info %}

`MinimumShouldMatch` поддерживается для режима `"or"`. Для режима `"and"` используйте `Mode="and"` без `MinimumShouldMatch`.

{% endnote %}
