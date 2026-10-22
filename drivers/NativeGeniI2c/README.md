# NativeGeniI2c

GENI serial-engine I2C master for Qualcomm QUP v3, FIFO (PIO) mode, polled. Pure C: the caller supplies the register accessors
and owns clocks and pins. Sequences follow the Linux `i2c-qcom-geni.c` FIFO path; see the comments in `GeniI2c.c`.

Current state: built into the fajita kernel by `scripts/build-fajita-touch.py` (libFajitaTouch.a); not yet run on hardware.

Host test: `tests/geni-i2c-test.cpp` against the register model `tests/FakeGeniI2c.*` (run by `scripts/check-fajita-touch-host.py`).
