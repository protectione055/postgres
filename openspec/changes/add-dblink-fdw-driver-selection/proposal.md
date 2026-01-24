# Change: CREATE DATABASE LINK 支持选择 FDW 驱动

## Why
当前 `CREATE DATABASE LINK` 的实现/设想在很多场景下隐含绑定到 `postgres_fdw`（主要用于 demo/回归测试）。这会限制该能力的可扩展性：用户无法在建链时选择其它 FDW（例如 `oracle_fdw`、`mysql_fdw` 等）作为底层驱动。

通过在 `CREATE DATABASE LINK` 语法中加入显式的 FDW 选择参数，我们可以：
- 让同一套 `@dblink` 语法复用不同 FDW 的连接与下推能力
- 明确 link 与驱动绑定关系，降低“默认驱动”带来的歧义
- 为后续扩展非 `postgres_fdw` 驱动打下接口与测试基础

## What Changes
- 为 `CREATE DATABASE LINK`（以及必要时 `ALTER DATABASE LINK`）增加“选择 FDW 驱动”的参数，形式为在 `USING '<connect_string>'` 中携带保留键 `fdw=<fdw_name>`。
- 明确默认行为与兼容性策略（例如：未指定 `fdw=` 时默认使用 `postgres_fdw`，或强制要求显式指定）。
- 增加权限与错误处理规则：
  - FDW 不存在 / 未安装 / 无 USAGE 权限
  - FDW 不支持 dblink 所需的元数据/执行钩子

## Non-Goals
- 不在本提案中保证所有 FDW 都可无缝工作；允许先支持 `postgres_fdw`，其它 FDW 需要逐步适配。
- 不在本提案中定义 FDW 选项的标准化（不同 FDW 的连接参数结构差异较大）。

## Impact
- Affected specs: `database-link`
- Affected code (预计): Parser/DDL 语法与解析、`pg_dblink` 元数据（存储 FDW 名称）、创建/更新 `FOREIGN SERVER` 与 `USER MAPPING` 的逻辑、FDW 能力检查。

## Compatibility
- 目标：对现有 demo/测试尽量兼容。
- 建议：若 `USING` 的 connect string 未提供 `fdw=<fdw_name>`，默认 `postgres_fdw`（与当前回归/开发默认一致）。
- 若项目希望避免隐式默认，也可以改为“必须显式指定”，但这将是 **BREAKING** 行为，需要额外迁移说明。

## Implementation Status
- 已实现：`USING '<connect_string>'` 中支持 `fdw=<fdw_name>`，未指定时默认 `postgres_fdw`。
- 验证：回归覆盖远端 DDL 变更、视图与 `SELECT *` 路径（见 contrib/postgres_fdw/sql/database_link.sql）。
- 参考提交：ccc04b435e6
