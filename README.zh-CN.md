# Yukari

[English](README.md) | [中文](README.zh-CN.md)

---

Zygisk 模块，用于隐藏指定目标应用的自定义 ROM 服务信号。

### 固定服务关键词

匹配方式为 ASCII 大小写不敏感，并内置在模块中：

- `lineage`
- `crdroid`
- `aospa`
- `pixelexperience`
- `omnirom`
- `protonaosp`

用户只需配置目标应用。模块不会全局停止或注销服务；过滤仅作用于选定的应用进程。

### 当前 native 行为

- Zygisk 应用特化时匹配配置的目标包名。
- 移除 Java `ServiceManager.sCache` 中包含固定 ROM 关键词的条目。
- Binder 请求过滤：在请求到达 ServiceManager 之前，重写匹配的服务查询名。
- Binder 回复过滤：仅在回复缓冲区可写时尝试过滤。
- 可选 `enhancedMode`：通过将回复缓冲区指针替换为修改后的用户态副本，可过滤只读 Binder 回复。
  > **⚠️ 实验性警告：** 此功能为高危实验性功能。开启后可能导致应用严重崩溃 (SIGSEGV)，因为它破坏了 Binder 缓冲区的生命周期。日常使用请务必保持关闭。
- Binder 拦截使用 Zygisk 的标准 PLT hook API，而非内联修补 libc。

请求/回复过滤使用等长占位符替换，不改变 Parcel 大小。

### 配置

```json
{
  "enabled": true,
  "enhancedMode": false,
  "targets": [
    "com.example.app"
  ]
}
```

正常稳定使用请保持 `enhancedMode` 关闭。仅在你完全理解 Binder 生命周期风险时才设为 `true` 用于测试。

### 构建

```bash
gradle :module:assembleRelease
bash scripts/package.sh
```
