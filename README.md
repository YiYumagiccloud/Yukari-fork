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

## Layout

- `module/module.prop` Magisk module metadata
- `module/config.json` default config
- `module/src/main/cpp` native Zygisk source skeleton
- `scripts/package.sh` creates a Magisk module zip after native build outputs are available
