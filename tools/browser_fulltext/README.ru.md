# MVP для полнотекстового поиска по исходному коду browser

Нужно возвращать путь, номер строки и её исходный текст.

## Индексация

Рабочая копия занимает 28 ГБ. Индексатор выбирает исходники по расширению. При обходе он пропускает скрытые каталоги и символьные ссылки. В индекс вошло 8.5 ГБ исходного текста.

## Схема таблиц

| Таблица | Число записей | Что хранит |
|---|---:|---|
| `browser_source_blocks` | 1 651 504 | Блоки исходного текста для 728 300 файлов. Файл до 8 КиБ хранится целиком. В более крупных файлах индексатор группирует целые строки в блоки с целевым размером 8 КиБ. |

Отдельная строка может занимать больше 8 КиБ. Индексатор делит её на фрагменты при длине свыше 1 МБ (1 000 000 байт). В файлах больше 8 КиБ он также делит последовательности длиннее 2 048 символов без пробелов и табуляции.

Таблица блоков использует сжатие LZ4 и синхронный индекс `block_trigrams` по тройкам символов из `text`. Первичный ключ `block_id` типа `Uint64` служит идентификатором документа в индексе триграмм. Старшие 32 бита задают порядок файлов по пути, младшие — порядковый номер блока внутри файла. Индекс `by_location` по `(path, first_line)` используется для обновления файла и сборки найденных строк.

## Поиск

SQL ищет подстроку с учётом регистра по индексу триграмм.

Если запрос длиннее 128 символов, SQL отбирает блоки по первым 128 символам. Перекрытие в 127 символов гарантирует, что это начало целиком попадёт в один блок. Затем SQL собирает исходную строку и проверяет полную подстроку. Результат — до 20 совпавших строк с путями и номерами.

{% cut "SQL запрос для поиска" %}

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


{% endcut %}

Для запуска [search.sql](search.sql) передайте искомую подстроку в параметре `$needle`.

```bash
~/github/ydb/apps/ydb/ydb -e grpc://localhost:61452 -d /local --no-discovery \
  sql --stats=basic -f ~/github/tools/browser_fulltext/search.sql \
  --param '$needle="GetFilteringDetailsString("'
```

Запрос вернул три строки из `browser/client/src/net/dns/opt_record_rdata.cc`.

| Строка | Исходный текст |
|---:|---|
| 48 | `std::optional<std::string> GetFilteringDetailsString(` |
| 80 | `auto db = GetFilteringDetailsString(entry, "db");` |
| 81 | `auto id = GetFilteringDetailsString(entry, "id");` |

Медиана серверного времени с прогретым планом составила 130 мс.

{% cut "Статистика выполнения запроса" %}

query_phases {
  duration_us: 609
  cpu_time_us: 162
}
query_phases {
  duration_us: 133760
  table_access {
    name: "/local/browser_source_blocks"
    reads {
      rows: 6
      bytes: 1921603
    }
    partitions_count: 3
  }
  table_access {
    name: "/local/browser_source_blocks/block_trigrams/indexImplTable"
    reads {
      rows: 719
      bytes: 29644449
    }
  }
  cpu_time_us: 1237
  affected_shards: 3
}
query_phases {
  duration_us: 3021
  table_access {
    name: "/local/browser_source_blocks"
    reads {
      rows: 1
      bytes: 8220
    }
    partitions_count: 1
  }
  table_access {
    name: "/local/browser_source_blocks/block_trigrams/indexImplTable"
  }
  table_access {
    name: "/local/browser_source_blocks/by_location/indexImplTable"
    reads {
      rows: 1
      bytes: 8
    }
    partitions_count: 1
  }
  cpu_time_us: 2600
  affected_shards: 2
}
compilation {
  from_cache: true
}
process_cpu_time_us: 1213
total_duration_us: 138975
total_cpu_time_us: 5212

{% endcut %}


## Стенд

Стенд - 1 виртуальная машина ydb-fulltext-test.klg.yp-c.yandex.net: 128 CPU, 256 GB RAM, 160 GB SSD

Кластер развёрнут через утилиту `local_ydb`. База `/local` доступна по адресу `grpc://localhost:61452` или через WebUI http://ydb-fulltext-test.klg.yp-c.yandex.net:26327

| Место в YDB | ГБ |
|---|---:|
| Размер таблицы и индексов | 5,28 |
| В том числе таблица блоков | 3,35 |
| В том числе индекс триграмм | 1,89 |
| В том числе индекс по расположению блока | 0,04 |
