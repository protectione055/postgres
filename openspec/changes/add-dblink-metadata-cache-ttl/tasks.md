# Tasks: Add session TupleDesc cache for @dblink metadata (TTL)

## 1. 规格与语法
- [x] 在 `database-link` spec delta 中新增/修改需求：会话级元数据缓存 + TTL 行为
- [x] 明确 `USING '<connect_string>'` 中 TTL 参数的格式、默认值、边界与错误处理

## 2. 目录与元数据
- [x] 明确 TTL 的存储位置：仅保存在 `pg_dblink`（推荐）还是仅存在内存/执行期
- [x] 明确 `ALTER DATABASE LINK` 是否允许修改 TTL（若允许，描述生效时机；若不允许，明确错误）

## 3. 实现
- [x] 在创建/更新 link 时解析并剥离 TTL 相关保留参数（不可传递给 FDW/server options）
- [x] 增加会话级 TupleDesc 缓存结构：key、value（TupleDesc + signature + expires_at）
- [x] 在调用 `GetDblinkTableMetadata()` 前检查缓存命中与有效期；未命中/过期则刷新
- [x] 明确缓存 key 的维度（至少 serverid + effective userid + remote namespace + remote relname；必要时包含 dblinkname）

## 4. 测试
- [x] 回归测试：同一会话内重复执行 `SELECT`/视图查询，应在 TTL 内复用缓存（建议通过观测 hook 调用次数的测试钩子或统计视图/日志）
- [x] 行为测试：设置短 TTL（例如 1s），远端 DDL 变更后在 TTL 过期后应刷新元数据
- [x] 错误测试：TTL 参数非法值（负数、非数字、过大）应报错

## 5. 文档
- [ ] 用户文档：说明默认 TTL=60s、如何在 `USING` 中配置、以及对“远端 schema drift 可见性延迟”的影响
