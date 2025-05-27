# RTX Texture Streaming Change Log

## 0.7.0 BETA

- Added support for "Texture Sets". Textures belonging to the same material using the same UV parameterization can share a single Sampler Feedback surface. In this implementation, which use just one possible approach, the diffuse texture is used to collect sampler feedback when the material meets certain conditions. These conditions are that none of the "follower" textures in a texture may have a width or height larger than that of the "primary" texture. The advantages of using texture sets are increased performance and having to maintain fewer sampler feedback resources.
- GPU performance counters specifically for the G-Buffer pass and the Sampler Feedback resolve pass
- New heap/tile allocation strategy. Instead of dynamically allocating/releasing heaps as the working set changes, now the Tiled Texture Manager returns the desired number of heaps based on the number of actively requested tiles plus a configurable number of "standby" tiles. The Feedbackmanager responds to this by possibly allocating more heaps but it will in steady state not release heaps. This brings the physical memory allocation up to a "high-water mark" where in steady state no further kernel memory allocations take place. If the active working set is less than the space avilable in allocated heaps, this space is used for standby tiles. If the application desires to compact memory, for example in pause or loading screens, excess standby tiles can be released, tile allocations can be defragmented (combined in heaps) and empty heaps can be released to reclaim physical memory.
- Code cleanup.

## 0.6.0 BETA

- Added support for a configurable number of "standby" tiles. When tiles are fully mapped but no longer being actively requested with sampler feedback they are not immediately deallocated but instead placed in a standby queue. Tiles in standby can be removed from this queue when they are being requested again without requiring them to be mapped and uploaded. This trades a limited amount of additional memory usage for reduced streaming pressure.
- Internal code changes, bug fixes, and performance improvements.

## 0.5.0 BETA

- Initial release.
