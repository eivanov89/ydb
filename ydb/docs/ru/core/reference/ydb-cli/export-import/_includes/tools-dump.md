# Выгрузка в файловую систему

## Кластер {#cluster}

Команда `admin сluster dump` выгружает в клиентскую файловую систему метаданные кластера, в описанном в статье [{#T}](../file-structure.md) формате:

```bash
{{ ydb-cli }} [connection options] admin cluster dump [options]
```

{% include [conn_options_ref.md](../../commands/_includes/conn_options_ref.md) %}

{% include [dump-options.md](./dump-options.md) %}

[Конфигурация кластера](../../../../devops/configuration-management/configuration-v2/config-overview.md) выгружается отдельно с помощью команды `{{ ydb-cli }} admin cluster config fetch`.

## База данных {#db}

Команда `admin database dump` выгружает в клиентскую файловую систему данные и метаданные базы данных, в описанном в статье [{#T}](../file-structure.md) формате:

```bash
{{ ydb-cli }} [connection options] admin database dump [options]
```

{% include [conn_options_ref.md](../../commands/_includes/conn_options_ref.md) %}

{% include [dump-options.md](./dump-options.md) %}

[Конфигурация базы данных](../../../../devops/configuration-management/configuration-v2/config-overview.md) выгружается отдельно с помощью команды `{{ ydb-cli }} admin database config fetch`.

## Объекты схемы данных {#schema-objects}

Команда `tools dump` выгружает в клиентскую файловую систему данные и информацию об объектах схемы данных, в описанном в статье [{#T}](../file-structure.md) формате:

```bash
{{ ydb-cli }} [connection options] tools dump [options]
```

{% include [conn_options_ref.md](../../commands/_includes/conn_options_ref.md) %}

{% include [dump-options.md](./dump-options.md) %}

- `-p <PATH>` или `--path <PATH>`: Путь к директории в базе данных, объекты внутри которой должны быть выгружены, либо путь к таблице. По умолчанию – корневая директория базы данных. В выгрузку будут включены все поддиректории, имена которых не начинаются с точки, и таблицы внутри них, имена которых не начинаются с точки. Для выгрузки таких таблиц или содержимого таких директорий можно явно указать их имена в данном параметре.

- `--exclude <STRING>`: Шаблон ([PCRE](https://www.pcre.org/original/doc/html/pcrepattern.html)) для исключения путей из выгрузки. Данный параметр может быть указан несколько раз, для разных шаблонов.

- `--scheme-only`: Выгружать только информацию об объектах схемы данных, без выгрузки данных.

- `--consistency-level <VAL>`: Уровень консистентности. Возможные варианты:

    - `database` - Полностью консистентная выгрузка, со снятием одного снапшота перед началом выгрузки. Применяется по умолчанию.
    - `table` - Консистентность в пределах каждой выгружаемой таблицы, со снятием отдельных независимых снапшотов для каждой выгружаемой таблицы. Может работать быстрее и оказывать меньшее влияние на обработку текущей нагрузки на базу данных.

- `--avoid-copy`: Не применять создание снапшота для выгрузки. Применяемый по умолчанию для обеспечения консистентности снапшот может быть неприменим для некоторых случаев (например, для таблиц со внешними блобами).{% if feature_serial %} Для корректной выгрузки таблиц с [серийными](../../../../yql/reference/types/serial.md) типами требуется не выставлять этот параметр. Иначе актуальное значение генератора последовательности не будет скопировано, и новые значения будут выдаваться со стартового значения, что может привести к конфликтам по первичному ключу.{% endif %}

- `--save-partial-result`: Не удалять результат частично выполненной выгрузки. Без включения данной опции результат выгрузки будет удален, если в процессе её выполнения произойдет ошибка.

- `--preserve-pool-kinds`: Если эта опция активна, в дамп будут сохранены имена типов устройств хранения, заданные для групп колонок таблиц (см. параметр `DATA` в статье [{#T}](../../../../yql/reference/syntax/create_table/family.md)). Если при восстановлении данных в базе не окажется [пулов хранения](../../../../concepts/glossary.md#storage-pool) с указанными именами, восстановление завершится ошибкой. По умолчанию данная опция деактивирована и при восстановлении используется пул хранения, заданный для базы данных при ее создании (см. [{#T}](../../../../devops/deployment-options/manual/initial-deployment.md#create-db)).

- `--ordered`: Строки в выгруженных таблицах будут отсортированы по первичному ключу.

## Примеры

{% include [ydb-cli-profile.md](../../../../_includes/ydb-cli-profile.md) %}

### Выгрузка кластера

С автоматическим созданием директории `backup_...` в текущей директории:

```bash
{{ ydb-cli }} -e <endpoint> admin cluster dump
```

В заданную директорию:

```bash
{{ ydb-cli }} -e <endpoint> admin cluster dump -o ~/backup_cluster
```

### Выгрузка базы данных

С автоматическим созданием директории `backup_...` в текущей директории:

```bash
{{ ydb-cli }} -e <endpoint> -d <database> admin database dump
```

В заданную директорию:

```bash
{{ ydb-cli }} -e <endpoint> -d <database> admin database dump -o ~/backup_db
```

### Выгрузка объектов схемы данных базы данных

С автоматическим созданием директории `backup_...` в текущей директории:

```bash
{{ ydb-cli }} --profile quickstart tools dump
```

В заданную директорию:

```bash
{{ ydb-cli }} --profile quickstart tools dump -o ~/backup_quickstart
```

### Выгрузка структуры таблиц в заданной директории базы данных и её поддиректориях

```bash
{{ ydb-cli }} --profile quickstart tools dump -p dir1 --scheme-only
```


