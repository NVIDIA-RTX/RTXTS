# RTXTS Change Log

## 0.6.0 BETA

- Added support for a configurable number of "standby" tiles. When tiles are fully mapped but no longer being actively requested with sampler feedback they are not immediately deallocated but instead placed in a standby queue. Tiles in standby can be removed from this queue when they are being requested again without requiring them to be mapped and uploaded. This trades a limited amount of additional memory usage for reduced streaming pressure.
- Internal code changes, bug fixes, and performance improvements.

## 0.5.0 BETA

- Initial release.
