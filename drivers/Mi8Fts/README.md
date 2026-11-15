# Dipper ST FTS touch

`Mi8Fts.c` is adapted from the device-validated `bsd/dev/mi8_fts.c` in
`reference/dipper-xnu` (source SHA-256 in its manifest:
`e0592a354c5c0113a0edb71f0215663a0584a98d643451c10d8607a9ab2f1875`).

The hardware protocol, SE14 I2C recovery, PM8998 L28 provisioning and FTM5
event decoding are retained. The original `/dev/redwood-touch` publication is
replaced with the `native_touch_*` seqlock consumed by NativeHidInput.

The reference project records physical touch down/up events on this device
(`docs/device-validation-20261030.json`).
