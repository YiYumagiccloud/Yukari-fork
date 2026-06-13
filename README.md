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

## Configuration

```json
{
  "enabled": true,
  "targets": [
    "com.example.app"
  ]
}
```

## Build artifacts

GitHub Actions runs the native release build, packages the Magisk module, verifies that the Zygisk library is present, and uploads:

```text
Yukari-<commit-sha>/Yukari.zip
```

Local build:

```bash
gradle :module:assembleRelease
bash scripts/package.sh
```

The packaging script fails instead of producing an incomplete zip when `arm64-v8a.so` is missing.
