# Tasks: CREATE DATABASE LINK 支持选择 FDW 驱动

## 1. 规格与语法
- [x] 在 `database-link` spec delta 中明确 FDW 选择子句的语法与默认值策略
- [x] 补充权限/错误处理场景（FDW 不存在、无权限、不支持 dblink 能力）

## 2. 目录与元数据
- [x] 确认 `pg_dblink` 是否已存储 FDW 名称；若没有，补充存储/依赖/回收规则
- [x] 明确 `ALTER DATABASE LINK` 是否允许变更 FDW 驱动（若允许，说明对既有对象的影响；若不允许，明确错误）

## 3. DDL 实现（后续实现阶段）
- [x] 在 `CREATE DATABASE LINK` 解析与执行中支持在 `USING '<connect_string>'` 中解析 `fdw=<fdwname>`
- [x] 在创建/更新对应 `FOREIGN SERVER` 时绑定所选 FDW
- [x] 添加 FDW 能力探测：仅当 FDW 实现 dblink 所需 hook 时允许用于 `@dblink`

## 4. 测试
- [x] 回归测试：在 `USING` connect string 中显式指定 `fdw=postgres_fdw` 的建链语句
- [x] 错误测试：指定不存在的 FDW/无权限/不支持 dblink hook

## 5. 文档
- [x] 用户文档：说明 `FDW` 子句用途、默认值、以及目前支持的 FDW 列表/限制

## 实现记录（证据）
- 落地提交：ccc04b435e6
- 关键代码：src/backend/commands/foreigncmds.c（解析/剥离 `fdw=` + 权限/能力校验）、src/backend/optimizer/util/plancat.c（dblink 元数据回调注入到 RTE/RelOptInfo）
- 关键 FDW：contrib/postgres_fdw/postgres_fdw.c、contrib/postgres_fdw/deparse.c（@dblink 元数据/解码修复）
- 回归测试：contrib/postgres_fdw/sql/database_link.sql + contrib/postgres_fdw/expected/database_link.out
- 运行方式：make -C contrib/postgres_fdw check REGRESS=database_link
