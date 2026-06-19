# Yukari

[English](README.md) | [中文](README.zh-CN.md)

---

Zygisk module that hides selected custom-ROM service signals from configured target applications.

### Fixed service keywords

Matching is ASCII case-insensitive and built into the module:

- `lineage`
- `crdroid`
- `aospa`
- `pixelexperience`
- `omnirom`
- `protonaosp`

Users configure target applications only. The module does not stop or unregister services globally; filtering is scoped to selected application processes.

### Current native behavior

- Zygisk app specialization matches configured target packages.
- Java `ServiceManager.sCache` entries containing fixed ROM keywords are removed.
- Binder request filtering rewrites matching service lookup names before they reach ServiceManager.
- Binder reply filtering is attempted only when the reply buffer is already writable.
- Optional `enhancedMode` can filter read-only Binder replies by swapping the reply buffer pointer to a modified userspace copy. This can hide more `listServices` results, but it is intended for geek/power users because Binder buffer ownership no longer matches the pointer libbinder sees.
- Binder interception uses Zygisk's standard PLT hook API instead of patching libc inline.

Request/reply filtering uses same-length placeholders instead of changing Parcel size.

### Configuration

```json
{
  "enabled": true,
  "enhancedMode": false,
  "targets": [
    "com.example.app"
  ]
}
```

Keep `enhancedMode` disabled for the normal stable path. Set it to `true` only if you understand and accept the Binder buffer ownership risk.

### Build

```bash
gradle :module:assembleRelease :module:assembleDebug
bash scripts/package.sh release
bash scripts/package.sh debug
```
