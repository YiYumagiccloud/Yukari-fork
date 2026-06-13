# Yukari

Zygisk module that hides selected custom-ROM service signals from configured target applications.

## Module identity

- Name: `Yukari`
- Module ID: `Yukari`
- Configuration: `/data/adb/modules/Yukari/config.json`

## Fixed service keywords

Matching is ASCII case-insensitive and built into the module:

- `lineage`
- `crdroid`
- `aospa`
- `pixelexperience`
- `omnirom`
- `protonaosp`

Users configure target applications only. The module does not stop or unregister services globally; filtering is scoped to selected application processes.

## Current native behavior

- Zygisk app specialization matches configured target packages.
- Java `ServiceManager.sCache` entries containing fixed ROM keywords are removed.
- An arm64 inline `ioctl` hook scrubs matching UTF-16 service names from context-manager Binder transactions before they reach ServiceManager.

The reply-side filtering path for `listServices`/debug info is planned next; the current implementation focuses on service lookup requests and cache cleanup.

## Configuration

```json
{
  "enabled": true,
  "targets": [
    "com.example.app"
  ]
}
```

## Layout

- `module/module.prop` Magisk module metadata
- `module/config.json` default config
- `module/src/main/cpp` native Zygisk source
- `scripts/package.sh` creates a Magisk module zip after native build outputs are available

## Build

```bash
gradle :module:assembleRelease
bash scripts/package.sh
```

A Gradle wrapper can be generated locally with `gradle wrapper --gradle-version 8.14.2` if needed.
