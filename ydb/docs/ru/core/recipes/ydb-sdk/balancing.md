# Балансировка

{{ ydb-short-name }} использует клиентскую балансировку, потому что клиентская балансировка эффективнее, когда на базу данных приходит большой трафик от множества клиентских приложений.
В большинстве случаев она просто работает в {{ ydb-short-name }} SDK. Однако иногда нужны специфичные настройки клиентской балансировки, например, для уменьшения серверных хопов, сокращения времени запроса или распределения нагрузки по зонам доступности.

Следует отметить, что клиентская балансировка работает в ограниченном режиме в отношении сессий {{ ydb-short-name }}. Клиентская балансировка в {{ ydb-short-name }} SDK осуществляется только в момент создания  новой сессии {{ ydb-short-name }} на конкретной ноде. После того, как сессия создана, все запросы на этой сессии направляются на ту ноду, на которой была создана сессия. Балансировка запросов на одной и той же сессии {{ ydb-short-name }} между разными нодами {{ ydb-short-name }} не происходит.

В данном разделе содержатся рецепты кода с настройкой клиентской балансировки в разных {{ ydb-short-name }} SDK

Содержание:

- [Равномерный случайный выбор](balancing-random-choice.md)
- [Предпочитать ближайший дата-центр](balancing-prefer-local.md)
- [Предпочитать конкретную зону доступности](balancing-prefer-location.md)
