# Блокировки

Блокировки используются, чтобы упорядочить одновременный (конкурентный) доступ нескольких процессов к одним и тем же ресурсам. Ресурсом может быть объект, с которым работает СУБД:

1. Списки первичных ключей, условия или даташард целиком (если блокировок становится очень много);
2. Структура в памяти, такая как хеш-таблица, буфер и т. п. (идентифицируется заранее присвоенным номером);
3. Абстрактные ресурсы, не имеющие никакого физического смысла (идентифицируются просто уникальным числом).

Чтобы процесс мог обратиться к ресурсу – он обязан захватить (acquire) блокировку, ассоциированную с этим ресурсом. Блокировка – это участок разделяемой памяти, в котором хранится информация о том свободна ли блокировка или захвачена. Блокировка сама по себе тоже является ресурсом, к которому возможен конкурентный доступ. Для доступа к блокировкам используются специальные примитивы синхронизации: семафоры или мьютексы. Смысл их в том, чтобы код, обращающийся к разделяемому ресурсу, одновременно выполнялся только в одном процессе. Если ресурс занят и блокировка не возможна – процесс завершится ошибкой.

Блокировки можно классифицировать по принципу их реализации:

- **Пессимистическая блокировка** до модификации данных накладывается блокировка на все потенциально затрагиваемые строки. Это исключает возможность их изменения другими сессиями до завершения текущей операции. После модификации гарантировано, что запись данных будет непротиворечивой;
- **Оптимистическая блокировка** не ограничивает доступ к данным в процессе работы, но использует специальный атрибут (например: `VERSION`) для контроля изменений. Атрибут - это поле из метаданных о строке, которое не видно пользователям, оно относится к деталям реализации механизма блокировок. Перед фиксацией изменений проверяется установленный атрибут. Если он не изменился – изменения зафиксируются (`COMMIT`), иначе транзакция будет откатана (`ROLLBACK`).

В совместимости {{ ydb-short-name }} с PostgreSQL используются оптимистические блокировки – это значит, что транзакции в конце своей работы проверяют условия выполнения блокировок. Если за время выполнения транзакции блокировка была нарушена – такая транзакция завершится ошибкой:

```text
Error: Transaction locks invalidated. Table: <table name>, code: 2001
```

С ошибкой могут завершаться транзакции, в которых исполняются SQL-инструкции чтения и записи. Транзакции, где исполняются только SQL-инструкции чтения или записи завершаются корректно. Приведём пример транзакции, которая завершится с ошибкой в случае внесения изменений в данные таблице параллельной транзакцией:

```sql
BEGIN;
SELECT * FROM people;
-- Если тут будет выполнен INSERT в другой транзакции, эта транзакция завершится ошибкой
UPDATE people SET age = 27
WHERE name = 'JOHN';
COMMIT;
```

В результате транзакция завершится ошибкой `Error: Transaction locks invalidated` и будет откачена (`ROLLBACK`). В случае ошибки `Error: Transaction locks invalidated` – можно попытаться выполнить транзакцию снова.