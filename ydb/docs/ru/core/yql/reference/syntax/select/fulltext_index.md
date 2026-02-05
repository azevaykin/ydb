# VIEW (Полнотекстовый индекс)

Для выполнения запроса `SELECT` с использованием [полнотекстового индекса](../../../../dev/fulltext-indexes.md) в строковой таблице используйте выражение `VIEW`:

```yql
SELECT ...
FROM TableName VIEW IndexName
WHERE FulltextContains(TextColumn, "query")
ORDER BY ...
```

{% note info %}

Полнотекстовый индекс не будет автоматически выбран [оптимизатором](../../../../concepts/glossary.md#optimizer), поэтому его нужно указывать явно с помощью `VIEW IndexName`.

Функции полнотекстового поиска (`FulltextContains`, `FulltextScore`) требуют `VIEW`. Если `VIEW` не используется, запрос завершится с ошибкой.

Описание функций см. в разделе [{#T}](../../builtins/fulltext.md), включая
[`FulltextContains`](../../builtins/fulltext.md#fulltext-contains) и
[`FulltextScore`](../../builtins/fulltext.md#fulltext-score).

{% endnote %}

## FulltextContains

`FulltextContains(text, query)` фильтрует строки по совпадению текста с полнотекстовым запросом:

```yql
SELECT id, title
FROM articles VIEW ft_idx
WHERE FulltextContains(body, "машинное обучение")
ORDER BY id;
```

### Подстановочный знак `*` (требуются N-граммы)

Если индекс создан с фильтрацией N-грамм, в термах запроса можно использовать подстановочный знак `*`:

```yql
SELECT id, title
FROM articles VIEW ft_idx
WHERE FulltextContains(body, "маш* обу*ние")
ORDER BY id;
```

## FulltextScore (релевантность [BM25](https://en.wikipedia.org/wiki/Okapi_BM25))

`FulltextScore(text, query)` возвращает оценку релевантности и может использоваться для ранжирования.
Ранжирование требует индекса типа `fulltext_relevance`.

```yql
SELECT id, title, FulltextScore(body, "быстрый лис") AS relevance
FROM articles VIEW ft_idx
ORDER BY relevance DESC
LIMIT 10;
```

### Дополнительные параметры

Дополнительные параметры нужно передавать как **именованные аргументы**.

* `Mode` (String): `"or"` или `"and"`
* `MinimumShouldMatch` (String): для режима `"or"` — минимальное число совпавших термов (абсолютное, например `"1"`, или процент, например `"50%"`)
* `K1` (Double): параметр K1 алгоритма [BM25](https://en.wikipedia.org/wiki/Okapi_BM25)
* `B` (Double): параметр B алгоритма [BM25](https://en.wikipedia.org/wiki/Okapi_BM25)

Пример:

```yql
SELECT id, FulltextScore(body, "быстрый лис", "or" AS Mode, "50%" AS MinimumShouldMatch) AS relevance
FROM articles VIEW ft_idx
ORDER BY relevance DESC;
```

{% note info %}

Только первые два аргумента `FulltextContains` / `FulltextScore` могут быть позиционными. Для дополнительных параметров используйте именованные аргументы.

{% endnote %}

