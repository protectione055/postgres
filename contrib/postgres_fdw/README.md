# postgres_fdw 数据库链接（@dblink）

本文档描述 `CREATE DATABASE LINK` 与 `@dblink` 的基础用法和行为。

## 概述

数据库链接会创建一组依赖对象：

- 与链接同名的 foreign server
- 对应的 user mapping
- 一个内部使用的锚点 foreign table

链接解析后可以通过 `schema.table@linkname` 的形式访问远端对象。

## 创建数据库链接

```sql
CREATE DATABASE LINK dblink1
  CONNECT TO CURRENT_USER
  USING 'dbname=postgres port=15433';
```

`USING` 连接串会被解析为 server 选项（例如 `dbname`、`host`、`port`）。
以下参数不会作为 server 选项写入：`user`、`password`、`client_encoding`、`fallback_application_name`。

如果使用固定用户：

```sql
CREATE DATABASE LINK dblink1
  CONNECT TO user "remote_user" IDENTIFIED BY 'secret'
  USING 'dbname=postgres port=15433';
```

## 查询远端表

```sql
SELECT * FROM public.my_table@dblink1;
```

也可用于视图：

```sql
CREATE VIEW dblink_view AS
  SELECT id FROM public.my_table@dblink1 WHERE id <= 3;
```

## 修改连接参数

链接创建后可通过 `ALTER SERVER` 调整连接参数，例如：

```sql
ALTER SERVER dblink1 OPTIONS (SET dbname 'postgres', SET port '15433');
```

对应的认证信息可以使用 `ALTER USER MAPPING` 调整。

## 删除数据库链接

```sql
DROP DATABASE LINK dblink1;
```

删除会清理链接所创建的 server、user mapping 与锚点 foreign table。

## 限制

- 目前仅支持 postgres_fdw 提供的 `GetDblinkTableMetadata` 元数据接口。
- @dblink 需要远端对象存在且可被当前用户访问。
