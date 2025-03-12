# RTXTS SDK Integration Guide

The RTXTS Tiled Texture Manager library helps manage Direct3D 12 Reserved Resource (tiled) textures driven by Sampler Feedback. The library can be configured to follow this logic.

A typical texture streaming pipeline may operate as follows per frame:

* Read resolved data, identify new tiles, and allocate memory
* Update tile mappings
* Fill new tiles with texture data
* Render the scene and update the `Sampler Feedback` textures
* Resolve `Sampler Feedback` textures for the current frame

Reading  back resolved sampler feedback data on the CPU should be executed asynchronously to prevent explicit synchronization between the CPU and GPU.

The RTXTS-TTM library handles the initial two passes, other passes should be implemented using the application's render abstraction.

## API

### Resource Management

For each tiled texture, we generate an integer identifier using `AddTiledTexture()` that will be associated with the resource. The library doesn't directly access D3D resources. Instead, it prepares all the necessary data to allocate tiles. Additionally, it offers helper function to obtain texture descriptor parameters for Sampler Feedback resource creation and a `MinMip` texture. The latter can be used to guarantee accurate data sampling where only resident tiles are touched.

```cpp
void rtxts::TiledTextureManager::AddTiledTexture(const TiledTextureDesc& tiledTextureDesc, uint32_t& textureId);
```

### Render Flow

The application is responsible for modifying the shaders that access texture data. For tiled textures, the HLSL `Sample()` function which includes an extra `status` out parameter is used. This parameter indicates whether every texture fetch from a `Sample()` operation has hit mapped tiles in a tiled resource. The `status` flag is verified using the `CheckAccessFullyMapped()` function. If this function returns false, we must determine the optimal texture level to sample from that contains all the necessary tiles mapped. For that we can utilize a dedicated `MinMip` texture. Alternatively, the shader can use a short loop to traverse down the mip pyramid to locate the most detailed mip level with resident tiles. It is crucial to ensure that at least the final mip level is mapped and contains the necessary data to prevent visual artifacts.

After acquiring the data, we must also record sampler feedback information for the original sample. This step informs the tile management code to stream in tiles to match the requested texture data on screen. We accomplish this using the HLSL `WriteSamplerFeedback()` function.

In each frame, we must read back the sampler feedback data through a two-step process. The first step, on the GPU, resolves the data from its internal opaque format and writes it to a buffer in a standard layout. The second step, on the CPU, asynchronously reads from that buffer to identify which tiles were accessed. Once the data is available on the CPU, we call `UpdateWithSamplerFeedback()` for each texture being processed. It’s important to note that resolving sampler feedback data and performing readbacks incurs a small overhead, so limiting the maximum amount of operations per frame may be beneficial.

After the sampler feedback data is copied back, we can clear sampler feedback textures content. For optimal performance, both the copying and clearing operations should be executed in batches.

```cpp
void rtxts::TiledTextureManager::UpdateWithSamplerFeedback(uint32_t textureId, SamplerFeedbackDesc& samplerFeedbackDesc, uint32_t timeStamp, uint32_t timeout);
```

After updating the internal state with `UpdateWithSamplerFeedback()` call, we retrieve a list of unnecessary tiles by calling `GetTilesToUnmap()`.

```cpp
void rtxts::TiledTextureManager::GetTilesToUnmap(uint32_t textureId, std::vector<TileType>& tileIndices);
```

Tiles that no longer need to be mapped should be updated using `ID3D12CommandQueue::UpdateTileMappings()`, as their underlying memory is already marked as available for reuse by other tiles.

After that, we transition to the newly identified tiles, which require both memory mapping and texture data updates. To do this, we start with `GetTilesToMap()` to retrieve a list of new tiles to process for each texture. For each texture processed in the current frame, we record the data and send a request to an external system to prepare the texture data for the new tiles. Only once this data becomes available we call `UpdateTilesMapping()` to update the internal state, which is then reflected in the MinMip data.

```cpp
void rtxts::TiledTextureManager::GetTilesToMap(uint32_t textureId, std::vector<TileType>& tileIndices);
void rtxts::TiledTextureManager::UpdateTilesMapping(uint32_t textureId, std::vector<TileType>& tileIndices);
```

### Memory Management

To create a `Tiled Texture Manager`, you need to fill an underlying descriptor that controls the allocation granularity for physical heaps and implements the `HeapAllocator()` interface class.

```cpp
struct TiledTextureManagerDesc
{
    bool alwaysMapPackedTiles = true;
    HeapAllocator* pHeapAllocator = nullptr;
    uint32_t heapTilesCapacity = 256; // number of 64KB tiles per heap, controls allocation granularity
};
```

```cpp
class HeapAllocator
{
public:
    virtual ~HeapAllocator() {};

    virtual void AllocateHeap(uint64_t heapSizeInBytes, uint32_t& heapId) = 0;
    virtual void ReleaseHeap(uint32_t heapId) = 0;
};
```

The TiledTextureManager controls heap memory allocations by invoking `AllocateHeap()` and `ReleaseHeap()` via `rtxts::HeapAllocator`. Application code overrides these functions and implements the creation and releasing of `D3D12Heap` objects. In `ReleaseHeap()`, the application also may choose to not (immediately) release the underlying heap and directly reuse it the next time `AllocateHeap()` is called. This wastes some memory but could avoid frequent heap object creation and destruction.

## Performance

* Batch resolve and clear calls for feedback textures
* Limit the maximum number of feedback textures to resolve, and tiles to map, per frame. For example a round-robin strategy could be used for textures, and a queue for tiles to map.
* For passes with ray tracing texture LODs ideally should be aligned with rasterization. Naive texture mip selection defaulting to level 0 can result in inefficient data streaming.
* The amount of `WriteSamplerFeedback` invocations per frame could be reduced by using, for example, stochastic writes. Or by tracking which subset of Sampler Feedback textures will be read back for the current frame, and only calling `WriteSamplerFeedback` on this subset.
* For multiple textures which shares UVs, such as in the same material, a single sampler feedback resource might be used to represent the requested state for the entire set. However care must be taken when individual tiled textures use differently sized (in texel width/height) tiles.
