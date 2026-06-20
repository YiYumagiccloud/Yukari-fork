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
- Optional `enhancedMode` can filter read-only Binder replies by swapping the reply buffer pointer to a modified userspace copy. 
  > **⚠️ Experimental Warning:** This feature is highly experimental. Enabling it may cause severe app crashes (SIGSEGV) due to Binder buffer ownership conflicts. Keep it `false` for stable daily use.
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

Keep `enhancedMode` disabled for the normal stable path. Set it to `true` only for testing if you fully understand the Binder lifecycle risks.

### Build

```bash
gradle :module:assembleRelease
bash scripts/package.sh
```
