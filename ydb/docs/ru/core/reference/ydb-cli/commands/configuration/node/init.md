# admin node config init

При развёртывании нового кластера {{ ydb-short-name }} или добавлении узлов в существующий (расширении) на каждом узле требуется директория для хранения его конфигурации. Команда `admin node config init` создаёт и подготавливает эту директорию, помещая в неё заданный конфигурационный файл или получая его с другого узла кластера (seed node).

Общий вид команды:

```bash
ydb [global options...] admin node config init [options...]
```

* `global options` — глобальные параметры.
* `options` — [параметры подкоманды](#options).

Посмотрите описание команды инициализации конфигурации узла:

```bash
ydb admin node config init --help
```

## Параметры подкоманды {#options}

Имя | Описание
---|---
`-d`, `--config-dir` | **Обязательный**. Путь к директории для хранения файла конфигурации.
`-f`, `--from-config` | Путь к файлу с начальной конфигурацией. Необходим при первоначальном развёртывании кластера. Можно использовать и при расширении, если файл с актуальной конфигурацией кластера был предварительно доставлен на узел.
`-s`, `--seed-node` | Эндпоинт узла-источника (seed node), с которого будет получена конфигурация. Используется при расширении кластера.

## Примеры {#examples}

Инициализируйте конфигурационную директорию узла, используя указанный файл конфигурации:

```bash
ydb admin node config init --config-dir /opt/ydb/cfg-dir --from-config config.yaml
```

Инициализируйте конфигурацию узла, получив её с узла-источника:

```bash
ydb admin node config init --config-dir /opt/ydb/cfg-dir --seed-node <node.ydb.tech>:2135
```

## Использование

После успешной инициализации директории для конфигурации узла вы можете запустить процесс `ydbd` на данном узле, добавив параметр `--config-dir` с указанием пути до директории. С этого момента при обновлении конфигурации кластера система автоматически актуализирует конфигурационный файл в указанной директории, что избавляет от необходимости ручного обновления файла конфигурации на узле.

При перезапуске узла он автоматически загружает актуальную конфигурацию из этой директории.
