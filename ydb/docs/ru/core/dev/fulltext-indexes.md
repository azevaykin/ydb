# Полнотекстовые индексы

Полнотекстовые индексы — это специализированные структуры данных, которые позволяют эффективно выполнять поиск по текстовому содержимому в колонках таблицы: по словам, фразам и (с N-граммами) по подстрокам.

Общее описание полнотекстового поиска см. в разделе [Полнотекстовый поиск](../concepts/fulltext_search.md).

## Характеристики полнотекстовых индексов {#characteristics}

Полнотекстовые индексы в {{ ydb-short-name }} строятся путём токенизации текста и создания инвертированного индекса. Это позволяет:

* быстро фильтровать строки через `FulltextContains()`;
* ранжировать результаты по релевантности ([BM25](https://en.wikipedia.org/wiki/Okapi_BM25)) через `FulltextScore()` при использовании `fulltext_relevance`;
* применять нормализацию регистра, стемминг и N-граммы с помощью фильтров индекса.

В текущей реализации доступны два варианта индекса:

* `fulltext_plain` — базовый полнотекстовый индекс;
* `fulltext_relevance` — полнотекстовый индекс со статистикой [BM25](https://en.wikipedia.org/wiki/Okapi_BM25) для расчёта релевантности.

Внутри полнотекстовый индекс состоит из скрытых индексных таблиц (например, `indexImplTable`). Эти таблицы могут отображаться в [статистике запросов](query-plans-optimization.md). Прямая запись в таблицы реализации индекса запрещена.

## Виды полнотекстовых индексов {#types}

Полнотекстовый индекс может быть **покрывающим**, то есть включать дополнительные колонки для чтения из индекса без обращения к основной таблице:

```yql
ALTER TABLE my_table
  ADD INDEX ft_idx
  GLOBAL USING fulltext_relevance
  ON (content) COVER (title)
  WITH (tokenizer=standard, use_filter_lowercase=true);
```

## Параметры индекса {#parameters}

Параметры полнотекстового индекса задаются в секции `WITH (...)`.
Полный список параметров см. здесь:

* [FULLTEXT INDEX (CREATE TABLE)](../yql/reference/syntax/create_table/fulltext_index.md)

## Создание полнотекстовых индексов {#creation}

Полнотекстовые индексы можно создавать:

* при создании таблицы с помощью [CREATE TABLE ... FULLTEXT INDEX](../yql/reference/syntax/create_table/fulltext_index.md);
* добавлять к существующей таблице с помощью [ALTER TABLE ... ADD INDEX](../yql/reference/syntax/alter_table/indexes.md).

## Использование полнотекстовых индексов {#select}

Запросы к полнотекстовым индексам выполняются с использованием синтаксиса `VIEW` в YQL.

### Фильтрация через FulltextContains

```yql
SELECT id, title
FROM my_table VIEW ft_idx
WHERE FulltextContains(content, "поисковые термы")
ORDER BY id;
```

### Ранжирование через FulltextScore

Оценка релевантности требует `fulltext_relevance`:

```yql
SELECT id, title, FulltextScore(content, "поисковые термы") AS relevance
FROM my_table VIEW ft_idx
ORDER BY relevance DESC
LIMIT 10;
```

Подробности и дополнительные параметры:

* [VIEW (Полнотекстовый индекс)](../yql/reference/syntax/select/fulltext_index.md)
* [Базовые функции полнотекстового поиска](../yql/reference/builtins/fulltext.md)

{% note info %}

Если не использовать выражение `VIEW`, запросы с `FulltextContains` / `FulltextScore` завершатся с ошибкой.

{% endnote %}

## Обновление полнотекстовых индексов {#update}

Полнотекстовые индексы автоматически поддерживаются при модификации данных. Таблицы с полнотекстовыми индексами поддерживают:

* `INSERT`
* `UPSERT`
* `REPLACE`
* `UPDATE`
* `DELETE`

## Удаление полнотекстовых индексов {#drop}

```yql
ALTER TABLE my_table DROP INDEX ft_idx;
```

## Ограничения {#limitations}

* `BulkUpsert` не поддерживается для таблиц с полнотекстовыми индексами.
* Использование полнотекстового индекса необходимо задавать явно с помощью `VIEW IndexName`.
* В одном полнотекстовом индексе индексируется одна текстовая колонка.
