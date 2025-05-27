# RTXTS SDK Integration Guide

The RTXTS Tiled Texture Manager library helps manage Direct3D 12 Reserved Resource (tiled) textures driven by Sampler Feedback. The library can be configured to follow this logic.

A typical texture streaming pipeline may operate as follows per frame:

* Read resolved data for a previous frame, identify new tiles, and allocate memory
* Update tile mappings
* Fill new tiles with texture data
* Render the scene and update the Sampler Feedback textures
* Resolve Sampler Feedback textures for the current frame

Reading  back resolved sampler feedback data on the CPU should be executed asynchronously to prevent explicit synchronization between the CPU and GPU.

The RTXTS-TTM library handles the initial two passes, other passes should be implemented using the application's render abstraction.

## Resource Management

For each tiled texture, we generate an integer identifier using `AddTiledTexture()` that will be associated with the resource. The library doesn't directly access D3D resources. Instead, it prepares all the necessary data to allocate tiles. Additionally, it offers helper function to obtain texture descriptor parameters for Sampler Feedback resource creation and a "MinMip" texture. The latter can be used to guarantee accurate data sampling where only resident tiles are touched.

```cpp
void rtxts::TiledTextureManager::AddTiledTexture(const TiledTextureDesc& tiledTextureDesc, uint32_t& textureId);
```

## Render Flow

### Shader Sampler Feedback

The application is responsible for modifying the shaders that access texture data. For tiled textures, the HLSL `Sample()` function which includes an extra `status` out parameter is used. This parameter indicates whether every texture fetch from a `Sample()` operation has hit mapped tiles in a tiled resource. The `status` flag is verified using the `CheckAccessFullyMapped()` function. If this function returns false, we must determine the optimal texture level to sample from that contains all the necessary tiles mapped. For that we can utilize a dedicated MinMip texture. Alternatively, the shader can use a short loop to traverse down the mip pyramid to locate the most detailed mip level with resident tiles. It is crucial to ensure that at least the final mip level is mapped and contains the necessary data to prevent visual artifacts.

After acquiring the data, we must also record sampler feedback information for the original sample. This step informs the tile management code to stream in tiles to match the requested texture data on screen. We accomplish this using the HLSL `WriteSamplerFeedback()` function.

### SetConfig

Before updating any tiled textures, at the beginning of the frame, the number of desired standby tiles to keep allocated in addition to the number of actively requested can be changed using `TiledTextureManagerConfig::numExtraStandbyTiles` and `SetConfig(const TiledTextureManagerConfig& config)`.

### Updating tiled textures

In each frame, we must read back the sampler feedback data through a two-step process. The first step, on the GPU, resolves the data from its internal opaque format and writes it to a buffer in a standard layout. The second step, on the CPU, asynchronously reads from that buffer to identify which tiles were accessed. As  resolving, copying and processing sampler feedback data takes GPU and CPU resources limiting the maximum amount of operations per frame may be beneficial. Batching the resolve operations is also recommended.

After reading back the sampler feedback resources, their resolved data can be passed to the TiledTextureManager for the corresponding textureId:

```cpp
void rtxts::TiledTextureManager::UpdateWithSamplerFeedback(uint32_t textureId, SamplerFeedbackDesc& samplerFeedbackDesc, uint32_t timeStamp, uint32_t timeout);
```

For a set of tiled textures which share the same texture parameterization, such as the case where multiple textures of the same material are sampled using the same UV coordinates in the shader, the requested allocation state for a texture can be made to match that of another "primary" texture. Note, however, that this will only cause tiles in the "follower" texture to be requested if tiles in the "primary" texture overlap with it in texel coordinates. If the follower texture has a higher resolution mip level, for example, these tiles will not be allocated using this method.

```cpp
void TiledTextureManagerImpl::MatchPrimaryTexture(uint32_t primaryTextureId, uint32_t followerTextureId, float timeStamp, float timeout)
```

### Heap management

After updating all managed textures for this frame, by calling UpdateWithSamplerFeedback and MatchPrimaryTexture, the number of heaps which are desired to be allocated can be queried with `uint32_t GetNumDesiredHeaps()`.

The application can allocate and remove heaps using `AddHeap(uint32_t heapId)` and `RemoveHeap(uint32_t heapId)`. The identifier which is provided by the application is mainly used so the application during the tile mapping phase can correlate a tile which is tracked internally to a heap object which is allocated externally, and this heap object can be passed to `UpdateTileMappings`.

### Tile unmapping/mapping

After updating the internal state with `UpdateWithSamplerFeedback()`, we retrieve a list of unnecessary tiles by calling `GetTilesToUnmap()`.

```cpp
void rtxts::TiledTextureManager::GetTilesToUnmap(uint32_t textureId, std::vector<uint32_t>& tileIndices);
```

Tiles that no longer need to be mapped should be updated using `ID3D12CommandQueue::UpdateTileMappings()`, as their underlying memory is already marked as available for reuse by other tiles.

After that, we transition to the newly identified tiles, which require both memory mapping and texture data updates. To do this, we start with `GetTilesToMap()` to retrieve a list of new tiles to process for each texture. For each texture processed in the current frame, we record the data and send a request to an external system to prepare the texture data for the new tiles. Only once this data becomes available we call `UpdateTilesMapping()` to update the internal state, which is then reflected in the MinMip data.

```cpp
void rtxts::TiledTextureManager::GetTilesToMap(uint32_t textureId, std::vector<uint32_t>& tileIndices);
void rtxts::TiledTextureManager::UpdateTilesMapping(uint32_t textureId, std::vector<uint32_t>& tileIndices);
```

## Performance

* Batch resolve and clear calls for feedback textures
* Limit the maximum number of feedback textures to resolve, and tiles to map, per frame. For example a round-robin strategy could be used for textures, and a queue for tiles to map.
* For passes with ray tracing texture LODs ideally should be aligned with rasterization. Naive texture mip selection defaulting to level 0 can result in inefficient data streaming.
* The amount of `WriteSamplerFeedback` invocations per frame could be reduced by using, for example, stochastic writes. Or by tracking which subset of Sampler Feedback textures will be read back for the current frame, and only calling `WriteSamplerFeedback` on this subset.
