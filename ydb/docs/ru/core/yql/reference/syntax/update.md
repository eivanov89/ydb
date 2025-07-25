
# UPDATE

{% if oss == true and backend_name == "YDB" %}

{% note warning %}

{% include [OLAP_not_allow_text](../../../_includes/not_allow_for_olap_text.md) %}

{% include [ways_add_data_to_olap.md](../../../_includes/ways_add_data_to_olap.md) %}

{% endnote %}

{% endif %}

`UPDATE` не может менять значение колонок, входящих в состав первичного ключа.

Изменяет данные в строковой таблице.{% if feature_mapreduce %} Таблица по имени ищется в базе данных, заданной оператором [USE](use.md).{% endif %} После ключевого слова `SET` указываются столбцы, значение которых необходимо заменить, и сами новые значения. Список строк задается с помощью условия `WHERE`. Если `WHERE` отсутствует, изменения будут применены ко всем строкам таблицы.

`UPDATE` не может менять значение `PRIMARY_KEY`.

## Пример

```yql
UPDATE my_table
SET Value1 = YQL::ToString(Value2 + 1), Value2 = Value2 - 1
WHERE Key1 > 1;
```

## UPDATE ON {#update-on}

Используется для обновления данных на основе результатов подзапроса. Набор колонок, возвращаемых подзапросом, должен быть подмножеством колонок обновляемой таблицы. В составе возвращаемых подзапросом колонок обязательно должны присутствовать все колонки первичного ключа таблицы. Типы данных возвращаемых подзапросом колонок должны совпадать с типами данных соответствующих колонок таблицы.

Для поиска обновляемых записей используется значение первичного ключа. В каждой найденной записи при выполнении оператора изменяются значения неключевых колонок, указанных в подзапросе. Значения колонок таблицы, которые отсутствуют в возвращаемых колонках подзапроса, остаются неизменными.

### Пример

```yql
$to_update = (
    SELECT Key, SubKey, "Updated" AS Value FROM my_table
    WHERE Key = 1
);

UPDATE my_table ON
SELECT * FROM $to_update;
```

{% if feature_batch_operations %}

## См. также

* [BATCH UPDATE](batch-update.md)

{% endif %}
