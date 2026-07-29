// The GPU backend: tiles in VRAM, dabs and compositing in compute shaders.
// Issues #12 and #13; D-021 for why it is opt-in, D-025 for why SDL_GPU.
//
// The whole file is behind SABLE_HAVE_GPU. Without SDL3 at configure time the
// stub at the bottom is all that is compiled, and `makeGpuBackend` returns
// nullptr — which is also what it returns on a machine with no usable device,
// so there is one "no GPU" path to get right rather than two.
//
// The invariant everything else hangs off: **the host tile is the storage of
// record and the device copy is a cache of it**, except between a dab and the
// next sync point, when the device copy is ahead. Nothing can write host
// pixels without moving `Tile::stamp()` (see canvas.hpp), so a stale device
// copy is always detectable without the writer knowing this file exists. That
// is what makes undo, layer operations and file loading need no hook here.
#include "sbl/gpu.hpp"

#ifdef SABLE_HAVE_GPU

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>

#include "sbl/io.hpp"
#include "sbl/paint.hpp"

namespace sbl {
namespace {

/// The GPU arena, its shaders and every transfer below are 8-bit RGBA
/// throughout, so this backend's byte count stays a constant even though a
/// tile's is not any more (D-023). A 16-bit document is DECLINED rather than
/// converted — see `GpuBackend::applyDab` and `GpuBackend::compositeRect` —
/// because painting it at half its depth without saying so is the one outcome
/// worse than not using the GPU at all.
constexpr std::size_t kTileBytes8 = tileBytes(ColourDepth::Bits8);


// glslc -mfmt=c output; regenerate with engine/src/shaders/build.sh.
const std::uint32_t kCompositeSpv[] =
#include "shaders/composite.spv.inc"
    ;
const std::uint32_t kDabSpv[] =
#include "shaders/dab.spv.inc"
    ;

// Must match the shaders.
constexpr std::size_t kMaxOps   = 192;
constexpr int         kMaxDepth = 4;
constexpr std::size_t kMaxDabs  = 64;
/// One brush mask, one byte a texel. Two of them live in `masks_`, the stamp
/// first and the grain after it (D-032).
constexpr std::size_t kMaskBytes = static_cast<std::size_t>(MASK_SIZE) * MASK_SIZE;

/// Tiles the arena holds before it starts evicting. 128 MiB of VRAM, which
/// covers what a 4000 x 4000 document with a handful of layers actually
/// touches, and leaves room on the integrated parts D-019 aims at. The device
/// copy is a cache, so overflowing this costs an upload, never a pixel.
constexpr std::uint32_t kArenaSlots = 512;

/// Tiles per submission. Bounds the transfer buffers at 2 MiB each and keeps
/// one fence wait covering a useful amount of work.
constexpr std::uint32_t kChunkSlots = 8;

constexpr std::uint32_t kNoSlot = 0xFFFFFFFFu;

/// A slot index as the op list carries it: sixteen bits each for the layer's
/// tile and its mask tile, so a masked layer still costs one uvec4 per op and
/// the uniform block keeps its 192-op ceiling (#48).
constexpr std::uint32_t kNoSlot16 = 0xFFFFu;
static_assert(kArenaSlots < kNoSlot16, "an arena slot must fit in sixteen bits");

struct CacheKey {
    LayerId layer;
    TileKey key;
    /// The layer's mask tile at this key, which is a different 256 KiB from its
    /// pixel tile at the same key and must not share a slot with it (#48).
    bool    mask = false;
    friend bool operator==(const CacheKey&, const CacheKey&) = default;
};

struct CacheKeyHash {
    std::size_t operator()(const CacheKey& k) const noexcept {
        return TileKeyHash{}(k.key) * 31u + std::hash<LayerId>{}(k.layer) +
               (k.mask ? 0x9E3779B9u : 0u);
    }
};

/// One tile's device copy.
///
/// `tile` is compared, never dereferenced: undo can destroy a Tile without
/// this file hearing about it, so every use of an entry starts by asking the
/// document for the tile at that key and checking it is still the same one.
struct Resident {
    std::uint32_t slot      = kNoSlot;
    const Tile*   tile      = nullptr;
    std::uint64_t stamp     = 0;      ///< the host stamp the copy was made from
    bool          hostStale = false;  ///< the GPU has painted since
    std::uint64_t used      = 0;      ///< eviction order
};

// std140, mirroring the Params blocks in the two .comp files.
struct CompositeParams {
    std::uint32_t head[4];             // opCount, destSlot, background, -
    std::int32_t  rect[4];             // originX, originY, canvasW, canvasH
    std::uint32_t ops[kMaxOps][4];
};
struct DabParams {
    std::uint32_t head[4];             // dabCount, slot, preserveOpacity, maskFlags
    std::int32_t  origin[4];
    std::int32_t  clipBox[4];
    float         geom[kMaxDabs][4];
    float         turn[kMaxDabs][4];   // cos, sin, grain strength, -
    std::uint32_t paint[kMaxDabs][4];
};

[[nodiscard]] std::uint32_t packRgba(PremulRgba8 c) noexcept {
    return static_cast<std::uint32_t>(c.r) | (static_cast<std::uint32_t>(c.g) << 8) |
           (static_cast<std::uint32_t>(c.b) << 16) | (static_cast<std::uint32_t>(c.a) << 24);
}

[[nodiscard]] std::uint32_t asBits(float f) noexcept {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

/// The op list `composite.comp` replays. The layer tree is walked once per
/// composite and only the arena slots are patched per tile.
struct Op {
    std::uint32_t kind        = 0;         // 0 draw, 1 open folder, 2 close, 3 drop clip
    /// Draw, and close-a-folder: a folder's mask applies to what its children
    /// composited to, so the closing op needs to know whose mask it is.
    LayerId       layer       = NO_LAYER;
    /// mode | clipToBelow << 8 | hasMask << 9 | mask default colour << 16.
    /// The default is what the shader reads where the mask has no tile, which
    /// is `LayerMask::outside` and usually most of the canvas.
    std::uint32_t modeAndClip = 0;
    std::uint32_t opacityBits = 0;
    bool          masked      = false;     // host-side: patch a mask slot in
};

/// False when the document does not fit the shader's limits, which is the
/// signal to composite it on the CPU instead. The nesting ceiling is low on
/// purpose: the shader's group stack is dynamically indexed, so every level
/// costs private memory in all 256 threads of a workgroup, used or not.
bool buildProgram(const Document& doc, std::optional<LayerId> parent, int depth,
                  std::vector<Op>& out) {
    if (depth > kMaxDepth) return false;
    for (const Layer& layer : doc.layers) {
        if (layer.parent != parent) continue;
        if (out.size() >= kMaxOps) return false;

        if (!layer.visible || layer.opacity <= 0.0f) {
            // io.cpp drops the clip mask for an invisible layer that is not
            // itself clipped, and leaves it alone for one that is.
            if (!layer.clipToBelow) out.push_back(Op{3, NO_LAYER, 0, 0});
            continue;
        }

        const bool masked = layer.mask.has_value() && layer.mask->enabled;
        const std::uint32_t modeAndClip =
            static_cast<std::uint32_t>(layer.blend) | (layer.clipToBelow ? 0x100u : 0u) |
            (masked ? 0x200u : 0u) |
            (masked ? static_cast<std::uint32_t>(layer.mask->outside) << 16u : 0u);
        const std::uint32_t opacity = asBits(std::clamp(layer.opacity, 0.0f, 1.0f));

        if (layer.kind == LayerKind::Folder) {
            out.push_back(Op{1, NO_LAYER, 0, 0, false});
            if (!buildProgram(doc, layer.id, depth + 1, out)) return false;
            if (out.size() >= kMaxOps) return false;
            out.push_back(Op{2, layer.id, modeAndClip, opacity, masked});
        } else {
            out.push_back(Op{0, layer.id, modeAndClip, opacity, masked});
        }
    }
    return true;
}

// ---------------------------------------------------------------- the backend

class GpuBackend final : public PaintBackend {
public:
    explicit GpuBackend(SDL_GPUDevice* device) : dev_(device) {}
    ~GpuBackend() override;

    [[nodiscard]] bool start(std::string* why);

    [[nodiscard]] std::string_view name() const noexcept override { return "gpu"; }

    void applyDab(PaintTarget& t, const Dab& dab) override;

    [[nodiscard]] UndoRecord bucketFill(Document& doc, LayerId target, std::int32_t x,
                                        std::int32_t y, StraightRgba8 colour,
                                        int tolerance, bool toMask) override;
    [[nodiscard]] UndoRecord fillSelection(Document& doc, LayerId target,
                                           StraightRgba8 colour, bool toMask) override;
    [[nodiscard]] UndoRecord transformRegion(Document& doc, LayerId target,
                                             const Selection& source,
                                             const Transform& transform) override;
    [[nodiscard]] UndoRecord clearLayer(Layer& layer, bool toMask) override;
    [[nodiscard]] UndoRecord mergeLayerDown(Document& doc, LayerId id) override;

    [[nodiscard]] std::vector<PremulRgba8> compositeRect(const Document& doc,
                                                         std::int32_t x, std::int32_t y,
                                                         std::int32_t w,
                                                         std::int32_t h) override;
    [[nodiscard]] StraightRgba8 pickColour(const Document& doc, std::int32_t x,
                                           std::int32_t y) override;
    [[nodiscard]] std::expected<void, Error> readback(const Document& doc) override;

    [[nodiscard]] std::size_t deviceBytes() const noexcept {
        return static_cast<std::size_t>(arenaSlots_ + 3u * kChunkSlots) * kTileBytes8;
    }

private:
    /// A tile whose device copy is ahead of the host, with the writable host
    /// side to put it back into.
    struct Behind {
        Tile*     tile = nullptr;
        Resident* entry = nullptr;
    };

    [[nodiscard]] Resident* residentFor(LayerId id, TileKey key, const Tile* tile,
                                        bool mask = false);
    [[nodiscard]] std::uint32_t claimSlot();
    void releaseSlot(Resident& entry);
    void dropLayer(LayerId id);
    bool syncTiles(const std::vector<Behind>& work);
    bool syncLayer(Layer& layer);
    void reconcile(const Document& doc);
    [[nodiscard]] bool uploadTiles();
    [[nodiscard]] bool uploadMasks();
    [[nodiscard]] bool downloadSlots(const std::vector<std::uint32_t>& slots,
                                     std::vector<PremulRgba8>& out);
    void submitPending();
    [[nodiscard]] std::vector<PremulRgba8> cpuFallback(const Document& doc,
                                                       std::int32_t x, std::int32_t y,
                                                       std::int32_t w, std::int32_t h);

    SDL_GPUDevice*          dev_        = nullptr;
    SDL_GPUComputePipeline* dabPipe_    = nullptr;
    SDL_GPUComputePipeline* compPipe_   = nullptr;
    SDL_GPUBuffer*          arena_      = nullptr;
    SDL_GPUBuffer*          dest_       = nullptr;
    SDL_GPUBuffer*          masks_      = nullptr;
    /// What `masks_` currently holds, compared by pointer only — registry
    /// entries never move, so identity is enough and no mask is ever uploaded
    /// twice in a stroke.
    const BrushMask*        deviceStamp_ = nullptr;
    const BrushMask*        deviceGrain_ = nullptr;
    SDL_GPUTransferBuffer*  up_         = nullptr;
    SDL_GPUTransferBuffer*  down_       = nullptr;
    std::uint32_t           arenaSlots_ = 0;

    std::unordered_map<CacheKey, Resident, CacheKeyHash> resident_;
    std::vector<std::uint32_t> freeSlots_;
    std::uint64_t clock_ = 0;

    /// Dabs waiting for a dispatch, and the tiles they will land on. A batch
    /// belongs to one target; `applyDab` flushes when the target moves.
    struct Batch {
        LayerId      layer = NO_LAYER;
        bool         preserveOpacity = false;
        std::int32_t clip[4]{};
        /// Borrowed from the registry, and the same for every dab in a batch:
        /// one pair of masks is on the device at a time, so a dab naming a
        /// different pair starts a new batch.
        const BrushMask* stamp = nullptr;
        const BrushMask* grain = nullptr;
        std::vector<Dab> dabs;
        std::vector<std::pair<TileKey, std::uint32_t>> tiles;
    } batch_;

    // Reserved once: nothing on the dab path may allocate (US-02.9).
    std::vector<Op> program_;
    std::vector<std::pair<std::uint32_t, const Tile*>> uploads_;
    std::vector<std::uint32_t> slotScratch_;
    std::vector<PremulRgba8> pixelScratch_;
    std::vector<Behind> behindScratch_;
    std::vector<CompositeParams> params_;
    std::vector<TileKey> keyScratch_;
};

GpuBackend::~GpuBackend() {
    if (dev_ == nullptr) return;
    if (dabPipe_ != nullptr)  SDL_ReleaseGPUComputePipeline(dev_, dabPipe_);
    if (compPipe_ != nullptr) SDL_ReleaseGPUComputePipeline(dev_, compPipe_);
    if (arena_ != nullptr)    SDL_ReleaseGPUBuffer(dev_, arena_);
    if (dest_ != nullptr)     SDL_ReleaseGPUBuffer(dev_, dest_);
    if (masks_ != nullptr)    SDL_ReleaseGPUBuffer(dev_, masks_);
    if (up_ != nullptr)       SDL_ReleaseGPUTransferBuffer(dev_, up_);
    if (down_ != nullptr)     SDL_ReleaseGPUTransferBuffer(dev_, down_);
    SDL_DestroyGPUDevice(dev_);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

bool GpuBackend::start(std::string* why) {
    const auto fail = [why](const char* what) {
        if (why != nullptr) *why = std::string(what) + ": " + SDL_GetError();
        return false;
    };

    const auto pipeline = [&](const std::uint32_t* code, std::size_t bytes,
                              Uint32 readOnly, Uint32 readWrite) {
        SDL_GPUComputePipelineCreateInfo info{};
        info.code_size   = bytes;
        info.code        = reinterpret_cast<const Uint8*>(code);
        info.entrypoint  = "main";
        info.format      = SDL_GPU_SHADERFORMAT_SPIRV;
        info.num_readonly_storage_buffers  = readOnly;
        info.num_readwrite_storage_buffers = readWrite;
        info.num_uniform_buffers = 1;
        info.threadcount_x = 16;
        info.threadcount_y = 16;
        info.threadcount_z = 1;
        return SDL_CreateGPUComputePipeline(dev_, &info);
    };

    compPipe_ = pipeline(kCompositeSpv, sizeof(kCompositeSpv), 1, 1);
    if (compPipe_ == nullptr) return fail("compositing shader");
    // One read-only storage buffer now: the brush masks (D-032).
    dabPipe_ = pipeline(kDabSpv, sizeof(kDabSpv), 1, 1);
    if (dabPipe_ == nullptr) return fail("dab shader");

    SDL_GPUBufferCreateInfo buf{};
    buf.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ |
                SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
    buf.size  = kArenaSlots * static_cast<Uint32>(kTileBytes8);
    arena_ = SDL_CreateGPUBuffer(dev_, &buf);
    if (arena_ == nullptr) return fail("tile arena");
    arenaSlots_ = kArenaSlots;

    buf.size = kChunkSlots * static_cast<Uint32>(kTileBytes8);
    dest_ = SDL_CreateGPUBuffer(dev_, &buf);
    if (dest_ == nullptr) return fail("composite target");

    buf.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
    buf.size  = 2 * static_cast<Uint32>(kMaskBytes);
    masks_ = SDL_CreateGPUBuffer(dev_, &buf);
    if (masks_ == nullptr) return fail("brush masks");

    SDL_GPUTransferBufferCreateInfo tb{};
    tb.size  = kChunkSlots * static_cast<Uint32>(kTileBytes8);
    tb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    up_ = SDL_CreateGPUTransferBuffer(dev_, &tb);
    tb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    down_ = SDL_CreateGPUTransferBuffer(dev_, &tb);
    if (up_ == nullptr || down_ == nullptr) return fail("transfer buffers");

    freeSlots_.reserve(arenaSlots_);
    for (std::uint32_t i = arenaSlots_; i > 0; --i) freeSlots_.push_back(i - 1);

    batch_.dabs.reserve(kMaxDabs);
    batch_.tiles.reserve(64);
    program_.reserve(kMaxOps);
    uploads_.reserve(64);
    slotScratch_.reserve(64);
    behindScratch_.reserve(64);
    params_.resize(kChunkSlots);
    return true;
}

// ------------------------------------------------------------------ residency

std::uint32_t GpuBackend::claimSlot() {
    if (!freeSlots_.empty()) {
        const std::uint32_t slot = freeSlots_.back();
        freeSlots_.pop_back();
        return slot;
    }
    // Least recently used, and never one the GPU has painted into: evicting a
    // clean entry costs an upload, evicting a dirty one would cost the artist
    // a brush stroke. Entries with no slot are the one being filled in.
    auto victim = resident_.end();
    for (auto it = resident_.begin(); it != resident_.end(); ++it) {
        if (it->second.hostStale || it->second.slot == kNoSlot) continue;
        if (victim == resident_.end() || it->second.used < victim->second.used) victim = it;
    }
    if (victim == resident_.end()) return kNoSlot;
    const std::uint32_t slot = victim->second.slot;
    resident_.erase(victim);
    return slot;
}

void GpuBackend::releaseSlot(Resident& entry) {
    if (entry.slot != kNoSlot) freeSlots_.push_back(entry.slot);
    entry.slot = kNoSlot;
}

Resident* GpuBackend::residentFor(LayerId id, TileKey key, const Tile* tile,
                                  bool mask) {
    if (tile == nullptr) return nullptr;
    // Belt and braces behind applyDab's depth check. A 16-bit tile is 512 KiB
    // and the arena slot is 256, so uploading one would not merely be wrong, it
    // would read past the end of the tile; "no slot" is the existing, tested
    // way of saying "let the CPU do this one".
    if (tile->depth() != ColourDepth::Bits8) return nullptr;
    const CacheKey ck{id, key, mask};
    Resident& entry = resident_[ck];
    entry.used = ++clock_;

    // A different tile at the same key, or the same tile written to since the
    // upload, and the host copy wins. That is the whole invalidation rule.
    if (entry.slot != kNoSlot && entry.tile == tile && entry.stamp == tile->stamp())
        return &entry;

    if (entry.slot == kNoSlot) {
        entry.slot = claimSlot();
        if (entry.slot == kNoSlot) {
            resident_.erase(ck);
            return nullptr;
        }
    }
    entry.tile      = tile;
    entry.stamp     = tile->stamp();
    entry.hostStale = false;
    uploads_.emplace_back(entry.slot, tile);
    return &entry;
}

void GpuBackend::dropLayer(LayerId id) {
    for (auto it = resident_.begin(); it != resident_.end();) {
        if (it->first.layer != id) { ++it; continue; }
        releaseSlot(it->second);
        it = resident_.erase(it);
    }
}

bool GpuBackend::syncTiles(const std::vector<Behind>& work) {
    if (work.empty()) return true;
    // A download only sees work the device has actually been given. Without
    // this, the first touch of a tile part-way through a stroke reads the slot
    // back before the stroke's own upload and dabs have been submitted, and
    // what comes back is whatever the slot held last — which, once the arena
    // has served an earlier document, is that document's pixels.
    submitPending();
    slotScratch_.clear();
    for (const Behind& b : work) slotScratch_.push_back(b.entry->slot);
    if (!downloadSlots(slotScratch_, pixelScratch_)) return false;

    for (std::size_t i = 0; i < work.size(); ++i) {
        // pixels8() moves the stamp — that is its job — so record where to.
        // Never null: only 8-bit tiles ever became resident (`residentFor`).
        std::memcpy(work[i].tile->pixels8(), pixelScratch_.data() + i * TILE_PIXELS,
                    kTileBytes8);
        work[i].entry->stamp     = work[i].tile->stamp();
        work[i].entry->hostStale = false;
    }
    return true;
}

bool GpuBackend::syncLayer(Layer& layer) {
    behindScratch_.clear();
    // Pixel tiles only. Nothing on the device ever writes a mask tile — mask
    // dabs are handed to the CPU below — so a mask copy in the arena is never
    // ahead of the host and has nothing to bring home.
    for (auto& [key, tile] : layer.tiles) {
        const auto it = resident_.find(CacheKey{layer.id, key, false});
        if (it == resident_.end() || !it->second.hostStale) continue;
        if (it->second.tile != &tile) {
            it->second.hostStale = false;    // a different tile lives here now
            continue;
        }
        behindScratch_.push_back(Behind{&tile, &it->second});
    }
    return syncTiles(behindScratch_);
}

void GpuBackend::reconcile(const Document& doc) {
    // Reclaims slots for tiles the document no longer has. Undo and the layer
    // operations destroy tiles without the backend hearing about it, so this
    // is the only place a slot comes back — which is why it runs when the
    // arena has filled up rather than on every call.
    for (auto& entry : resident_) entry.second.tile = nullptr;
    const auto keep = [this](LayerId id, const TileMap& tiles, bool mask) {
        for (const auto& [key, tile] : tiles)
            if (const auto it = resident_.find(CacheKey{id, key, mask});
                it != resident_.end() && it->second.stamp == tile.stamp())
                it->second.tile = &tile;
    };
    for (const Layer& layer : doc.layers) {
        keep(layer.id, layer.tiles, false);
        // Without this a mask tile's slot is reclaimed on every reconcile and
        // re-uploaded on the next composite — correct, and pointlessly slow.
        if (layer.mask.has_value()) keep(layer.id, layer.mask->tiles, true);
    }

    for (auto it = resident_.begin(); it != resident_.end();) {
        if (it->second.tile != nullptr) { ++it; continue; }
        releaseSlot(it->second);
        it = resident_.erase(it);
    }
}

bool GpuBackend::uploadTiles() {
    const bool ok = [&] {
        for (std::size_t base = 0; base < uploads_.size(); base += kChunkSlots) {
            const std::size_t n = std::min<std::size_t>(kChunkSlots, uploads_.size() - base);

            auto* staging =
                static_cast<PremulRgba8*>(SDL_MapGPUTransferBuffer(dev_, up_, true));
            if (staging == nullptr) return false;
            for (std::size_t i = 0; i < n; ++i)
                std::memcpy(staging + i * TILE_PIXELS,
                            uploads_[base + i].second->pixels8(), kTileBytes8);
            SDL_UnmapGPUTransferBuffer(dev_, up_);

            SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev_);
            if (cmd == nullptr) return false;
            SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(cmd);
            for (std::size_t i = 0; i < n; ++i) {
                const SDL_GPUTransferBufferLocation src{up_,
                                                        static_cast<Uint32>(i * kTileBytes8)};
                const SDL_GPUBufferRegion dst{
                    arena_, uploads_[base + i].first * static_cast<Uint32>(kTileBytes8),
                    static_cast<Uint32>(kTileBytes8)};
                SDL_UploadToGPUBuffer(pass, &src, &dst, false);
            }
            SDL_EndGPUCopyPass(pass);
            if (!SDL_SubmitGPUCommandBuffer(cmd)) return false;
        }
        return true;
    }();
    uploads_.clear();
    return ok;
}

bool GpuBackend::uploadMasks() {
    // Only when they change. A preset's masks are fixed for the length of a
    // stroke, so this is one small upload per stroke and nothing per dispatch
    // — which is the whole reason the masks are not simply pushed with the
    // rest of the parameters.
    if (batch_.stamp == deviceStamp_ && batch_.grain == deviceGrain_) return true;

    auto* staging = static_cast<std::uint8_t*>(SDL_MapGPUTransferBuffer(dev_, up_, true));
    if (staging == nullptr) return false;
    // Every mask is exactly kMaskBytes long — the registry sizes them on the
    // way in — so nothing here has to check. Absent ones are zeroed rather
    // than left alone: the shader is told not to read one, but a buffer still
    // holding the previous stroke's paper is one flag bug away from using it.
    const auto put = [&](const BrushMask* m, std::size_t at) {
        if (m != nullptr) std::memcpy(staging + at, m->coverage.data(), kMaskBytes);
        else              std::memset(staging + at, 0, kMaskBytes);
    };
    put(batch_.stamp, 0);
    put(batch_.grain, kMaskBytes);
    SDL_UnmapGPUTransferBuffer(dev_, up_);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev_);
    if (cmd == nullptr) return false;
    SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(cmd);
    const SDL_GPUTransferBufferLocation src{up_, 0};
    const SDL_GPUBufferRegion dst{masks_, 0, 2 * static_cast<Uint32>(kMaskBytes)};
    // Cycled: every byte is replaced, and a dispatch from the batch before
    // this one may still be reading the old contents.
    SDL_UploadToGPUBuffer(pass, &src, &dst, true);
    SDL_EndGPUCopyPass(pass);
    if (!SDL_SubmitGPUCommandBuffer(cmd)) return false;

    deviceStamp_ = batch_.stamp;
    deviceGrain_ = batch_.grain;
    return true;
}

bool GpuBackend::downloadSlots(const std::vector<std::uint32_t>& slots,
                               std::vector<PremulRgba8>& out) {
    out.resize(slots.size() * TILE_PIXELS);
    for (std::size_t base = 0; base < slots.size(); base += kChunkSlots) {
        const std::size_t n = std::min<std::size_t>(kChunkSlots, slots.size() - base);

        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev_);
        if (cmd == nullptr) return false;
        SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(cmd);
        for (std::size_t i = 0; i < n; ++i) {
            const SDL_GPUBufferRegion src{
                arena_, slots[base + i] * static_cast<Uint32>(kTileBytes8),
                static_cast<Uint32>(kTileBytes8)};
            const SDL_GPUTransferBufferLocation dst{down_,
                                                    static_cast<Uint32>(i * kTileBytes8)};
            SDL_DownloadFromGPUBuffer(pass, &src, &dst);
        }
        SDL_EndGPUCopyPass(pass);

        SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        if (fence == nullptr) return false;
        const bool ok = SDL_WaitForGPUFences(dev_, true, &fence, 1);
        SDL_ReleaseGPUFence(dev_, fence);
        if (!ok) return false;

        const auto* mapped =
            static_cast<const PremulRgba8*>(SDL_MapGPUTransferBuffer(dev_, down_, false));
        if (mapped == nullptr) return false;
        std::memcpy(out.data() + base * TILE_PIXELS, mapped, n * kTileBytes8);
        SDL_UnmapGPUTransferBuffer(dev_, down_);
    }
    return true;
}

// ----------------------------------------------------------------------- dabs

void GpuBackend::submitPending() {
    // The uploads go first and unconditionally, even when there are no dabs to
    // dispatch. `uploads_` and `batch_` are both queues that exist only on the
    // host until this runs, and the device cannot see either of them — so
    // ANYTHING that reads the arena or dispatches into it comes through here
    // first. Leaving the upload queued and reading the slot anyway is how a
    // tile came back holding the previous document's pixels.
    if (!uploads_.empty() && !uploadTiles())
        recordFailure(Error{ErrorKind::Io, "could not upload tiles to the GPU: " +
                                               std::string(SDL_GetError())});
    if (batch_.dabs.empty() || batch_.tiles.empty()) {
        batch_.dabs.clear();
        batch_.tiles.clear();
        return;
    }

    DabParams p{};
    p.head[0] = static_cast<std::uint32_t>(batch_.dabs.size());
    p.head[2] = batch_.preserveOpacity ? 1u : 0u;
    p.head[3] = (batch_.stamp != nullptr ? 1u : 0u) |
                (batch_.grain != nullptr ? 2u : 0u);
    std::copy(batch_.clip, batch_.clip + 4, p.clipBox);
    if (!uploadMasks()) {
        recordFailure(Error{ErrorKind::Io, "could not upload the brush masks: " +
                                               std::string(SDL_GetError())});
        batch_.dabs.clear();
        batch_.tiles.clear();
        return;
    }
    for (std::size_t i = 0; i < batch_.dabs.size(); ++i) {
        const Dab& d  = batch_.dabs[i];
        p.geom[i][0]  = static_cast<float>(d.x);
        p.geom[i][1]  = static_cast<float>(d.y);
        p.geom[i][2]  = d.radius;
        p.geom[i][3]  = d.hardness;
        // Turned on the host, so the shader runs no trigonometry of its own —
        // one fewer function whose last bit has to match the CPU's.
        p.turn[i][0]  = std::cos(d.angle);
        p.turn[i][1]  = std::sin(d.angle);
        p.turn[i][2]  = d.grainStrength;
        // The shader paints 8-bit tiles, and this narrows to exactly what
        // `CpuBackend::applyDab` narrows the same dab to — which is what keeps
        // tests/differential.cpp inside its ±1 tolerance without touching it.
        p.paint[i][0] = packRgba(narrow(d.colour));
        p.paint[i][1] = d.erase ? 1u : 0u;
    }

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev_);
    if (cmd != nullptr) {
        const SDL_GPUStorageBufferReadWriteBinding rw{arena_, false, 0, 0, 0};
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, nullptr, 0, &rw, 1);
        SDL_BindGPUComputePipeline(pass, dabPipe_);
        // Bound even when neither mask is in use: the pipeline declares the
        // binding, and an unbound storage buffer is a validation error rather
        // than an unread one.
        SDL_BindGPUComputeStorageBuffers(pass, 0, &masks_, 1);
        for (const auto& [key, slot] : batch_.tiles) {
            p.head[1]   = slot;
            p.origin[0] = key.first  * TILE_SIZE;
            p.origin[1] = key.second * TILE_SIZE;
            SDL_PushGPUComputeUniformData(cmd, 0, &p, sizeof(p));
            SDL_DispatchGPUCompute(pass, TILE_SIZE / 16, TILE_SIZE / 16, 1);
        }
        SDL_EndGPUComputePass(pass);
        SDL_SubmitGPUCommandBuffer(cmd);
    } else {
        recordFailure(Error{ErrorKind::Io, "no GPU command buffer for the stroke: " +
                                               std::string(SDL_GetError())});
    }
    batch_.dabs.clear();
    batch_.tiles.clear();
}

void GpuBackend::applyDab(PaintTarget& t, const Dab& dab) {
    if (t.layer.locked) return;

    // #48, and the same answer D-023 gave a 16-bit document: the reference
    // implementation takes what this backend does not do. A mask dab is one
    // `dab.comp` could paint — a mask tile is an ordinary 8-bit tile — but the
    // batch is keyed on a layer id alone, so a mask stroke and a pixel stroke
    // on the same layer would land in one dispatch on the wrong slot. Routing
    // it to the CPU is four lines; a second key on the batch is a change to
    // every dab path for a stroke that costs a few milliseconds either way.
    // `submitPending` first, because the CPU is about to write host tiles this
    // backend may still owe pixels to.
    if (t.toMask) {
        submitPending();
        syncLayer(t.layer);
        cpuBackend().applyDab(t, dab);
        return;
    }
    if (t.layer.kind != LayerKind::Raster) return;
    if (dab.colour.a == 0 || dab.radius <= 0.0f) return;

    // D-023: the arena and both shaders are 8-bit RGBA. Hand a 16-bit layer
    // straight to the reference implementation rather than half of it to the
    // device — this is the same fallback the arena-full case takes below, and
    // the artist's picture is right either way. `submitPending` first, because
    // the CPU is about to write host tiles this backend may still owe pixels to.
    if (t.layer.depth != ColourDepth::Bits8) {
        submitPending();
        syncLayer(t.layer);
        cpuBackend().applyDab(t, dab);
        return;
    }

    const double r = dab.radius;
    const auto lo = [](double v) { return static_cast<std::int32_t>(std::floor(v)); };
    const auto hi = [](double v) { return static_cast<std::int32_t>(std::ceil(v)); };
    const std::int32_t minX = std::max<std::int32_t>(0, lo(dab.x - r));
    const std::int32_t maxX = std::min<std::int32_t>(t.width  - 1, hi(dab.x + r));
    const std::int32_t minY = std::max<std::int32_t>(0, lo(dab.y - r));
    const std::int32_t maxY = std::min<std::int32_t>(t.height - 1, hi(dab.y + r));
    if (minX > maxX || minY > maxY) return;

    // The selection folds into the clip box once per batch rather than being
    // tested per pixel: it cannot change inside a stroke.
    std::int32_t clip[4]{0, 0, t.width, t.height};
    if (t.selection != nullptr) {
        clip[0] = std::max(clip[0], t.selection->x);
        clip[1] = std::max(clip[1], t.selection->y);
        clip[2] = std::min(clip[2], t.selection->x + t.selection->w);
        clip[3] = std::min(clip[3], t.selection->y + t.selection->h);
    }

    if (batch_.layer != t.layer.id ||
        batch_.preserveOpacity != t.layer.preserveOpacity ||
        batch_.stamp != dab.stamp || batch_.grain != dab.grain ||
        !std::equal(clip, clip + 4, batch_.clip) ||
        batch_.dabs.size() >= kMaxDabs) {
        submitPending();
    }
    batch_.layer           = t.layer.id;
    batch_.preserveOpacity = t.layer.preserveOpacity;
    batch_.stamp           = dab.stamp;
    batch_.grain           = dab.grain;
    std::copy(clip, clip + 4, batch_.clip);

    for (std::int32_t ty = tileIndex(minY); ty <= tileIndex(maxY); ++ty) {
        for (std::int32_t tx = tileIndex(minX); tx <= tileIndex(maxX); ++tx) {
            const TileKey key{tx, ty};
            Tile* tile = t.layer.find(key);

            // D-006, unchanged: copy on first touch, on the host. #12 asks for
            // this decision explicitly, and host is the answer — the 256 MB
            // budget the status bar shows keeps meaning what it says, undo
            // does not compete with art for VRAM, and `memoryBytes()` needed
            // no change at all. The cost is one download per tile per stroke,
            // never per dab.
            if (t.touched.insert(key).second) {
                TileSnapshot snap;
                snap.layer = t.layer.id;
                snap.key   = key;
                if (tile != nullptr) {
                    const auto it = resident_.find(CacheKey{t.layer.id, key, false});
                    if (it != resident_.end() && it->second.hostStale &&
                        it->second.tile == tile) {
                        behindScratch_.assign(1, Behind{tile, &it->second});
                        if (!syncTiles(behindScratch_))
                            recordFailure(Error{ErrorKind::Io,
                                                "could not snapshot a tile for undo: " +
                                                    std::string(SDL_GetError())});
                    }
                    snap.before.emplace(tile->clone());
                }
                t.undo.tiles.push_back(std::move(snap));
            }
            if (tile == nullptr) {
                if (dab.erase) continue;    // nothing to erase from an empty tile
                tile = &t.layer.tileFor(key);
            }

            Resident* entry = residentFor(t.layer.id, key, tile);
            if (entry == nullptr) {
                // The arena is full of tiles this very stroke has painted, so
                // there is nothing safe to evict. The CPU is the reference
                // implementation and can finish the stroke; put the device's
                // work back on the host first so the two do not fight.
                submitPending();
                syncLayer(t.layer);
                cpuBackend().applyDab(t, dab);
                return;
            }
            entry->hostStale = true;
            if (std::ranges::find(batch_.tiles, key,
                                  &std::pair<TileKey, std::uint32_t>::first) ==
                batch_.tiles.end())
                batch_.tiles.emplace_back(key, entry->slot);
        }
    }
    batch_.dabs.push_back(dab);
}

// ---------------------------------------------------------------- compositing

std::vector<PremulRgba8> GpuBackend::cpuFallback(const Document& doc, std::int32_t x,
                                                 std::int32_t y, std::int32_t w,
                                                 std::int32_t h) {
    uploads_.clear();
    return cpuBackend().compositeRect(doc, x, y, w, h);
}

std::vector<PremulRgba8> GpuBackend::compositeRect(const Document& doc, std::int32_t x,
                                                   std::int32_t y, std::int32_t w,
                                                   std::int32_t h) {
    if (w <= 0 || h <= 0) return {};
    submitPending();

    // D-023, and the same answer as `applyDab`: composite.comp reads 8-bit
    // tiles out of the arena, so a 16-bit document is composited by the
    // reference implementation instead. Deliberately BEFORE buildProgram —
    // there is no version of this the shader could be asked to attempt.
    if (doc.depth != ColourDepth::Bits8) return cpuFallback(doc, x, y, w, h);

    program_.clear();
    if (!buildProgram(doc, std::nullopt, 0, program_))
        return cpuFallback(doc, x, y, w, h);   // deeper or wider than the shader

    std::vector<PremulRgba8> out(static_cast<std::size_t>(w) * h, PremulRgba8{});
    const std::int32_t x0 = std::max(x, 0), x1 = std::min(x + w, doc.width);
    const std::int32_t y0 = std::max(y, 0), y1 = std::min(y + h, doc.height);
    if (x0 >= x1 || y0 >= y1) return out;      // wholly off-canvas: transparent

    keyScratch_.clear();
    for (std::int32_t ty = tileIndex(y0); ty <= tileIndex(y1 - 1); ++ty)
        for (std::int32_t tx = tileIndex(x0); tx <= tileIndex(x1 - 1); ++tx)
            keyScratch_.emplace_back(tx, ty);

    // The only place that sees the whole document, so the only place a slot
    // whose tile has been thrown away comes back. Not worth an O(tiles) walk
    // on every call, so it waits until the arena has actually filled.
    if (freeSlots_.empty()) reconcile(doc);

    // Every tile a chunk draws has to stay resident until the chunk has been
    // dispatched, so a chunk may never need more slots than the arena has —
    // otherwise it would evict its own inputs and composite the wrong pixels.
    // A masked layer needs a second slot for its mask tile, and it has to stay
    // resident just as long — a chunk that evicted its own mask would composite
    // the layer unmasked (#48).
    std::uint32_t draws = 0;
    for (const Op& op : program_)
        draws += (op.kind == 0 ? 1u : 0u) + (op.masked ? 1u : 0u);
    if (draws > arenaSlots_) return cpuFallback(doc, x, y, w, h);
    const std::size_t chunk =
        std::max<std::size_t>(1, std::min<std::uint32_t>(
                                     kChunkSlots, draws == 0 ? kChunkSlots
                                                             : arenaSlots_ / draws));

    const std::uint32_t bg = packRgba(doc.background.premultiply());

    for (std::size_t base = 0; base < keyScratch_.size(); base += chunk) {
        const std::size_t n = std::min<std::size_t>(chunk, keyScratch_.size() - base);

        for (std::size_t i = 0; i < n; ++i) {
            CompositeParams& p = params_[i];
            p.head[0] = static_cast<std::uint32_t>(program_.size());
            p.head[1] = static_cast<std::uint32_t>(i);
            p.head[2] = bg;
            p.rect[0] = keyScratch_[base + i].first  * TILE_SIZE;
            p.rect[1] = keyScratch_[base + i].second * TILE_SIZE;
            p.rect[2] = doc.width;
            p.rect[3] = doc.height;

            for (std::size_t j = 0; j < program_.size(); ++j) {
                const Op& op = program_[j];
                p.ops[j][0] = op.kind;
                // Both halves absent: no tile here, and no mask tile here.
                p.ops[j][1] = kNoSlot;
                p.ops[j][2] = op.modeAndClip;
                p.ops[j][3] = op.opacityBits;
                // Kind 2 closes a folder, which has no tile of its own but may
                // well have a mask over what its children composited to.
                if (op.kind != 0 && op.kind != 2) continue;

                const Layer* layer = doc.layerById(op.layer);
                if (layer == nullptr) continue;
                const TileKey key = keyScratch_[base + i];

                if (op.kind == 0) {
                    const Tile* tile = layer->find(key);
                    if (tile != nullptr) {
                        const Resident* entry = residentFor(op.layer, key, tile);
                        if (entry == nullptr) return cpuFallback(doc, x, y, w, h);
                        p.ops[j][1] = (p.ops[j][1] & ~kNoSlot16) | entry->slot;
                    }
                }
                // No mask tile at this key is not a failure: the shader falls
                // back to the mask's default colour, which is what most of a
                // sparse mask is made of.
                if (!op.masked) continue;
                const Tile* maskTile = layer->mask->find(key);
                if (maskTile == nullptr) continue;
                const Resident* entry = residentFor(op.layer, key, maskTile, true);
                if (entry == nullptr) return cpuFallback(doc, x, y, w, h);
                p.ops[j][1] = (p.ops[j][1] & kNoSlot16) | (entry->slot << 16u);
            }
        }

        if (!uploadTiles()) {
            recordFailure(Error{ErrorKind::Io, "could not upload tiles to the GPU: " +
                                                   std::string(SDL_GetError())});
            return cpuFallback(doc, x, y, w, h);
        }

        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev_);
        if (cmd == nullptr) return cpuFallback(doc, x, y, w, h);
        const SDL_GPUStorageBufferReadWriteBinding rw{dest_, true, 0, 0, 0};
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(cmd, nullptr, 0, &rw, 1);
        SDL_BindGPUComputePipeline(pass, compPipe_);
        SDL_BindGPUComputeStorageBuffers(pass, 0, &arena_, 1);
        for (std::size_t i = 0; i < n; ++i) {
            SDL_PushGPUComputeUniformData(cmd, 0, &params_[i], sizeof(params_[i]));
            SDL_DispatchGPUCompute(pass, TILE_SIZE / 16, TILE_SIZE / 16, 1);
        }
        SDL_EndGPUComputePass(pass);

        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        for (std::size_t i = 0; i < n; ++i) {
            const SDL_GPUBufferRegion src{dest_, static_cast<Uint32>(i * kTileBytes8),
                                          static_cast<Uint32>(kTileBytes8)};
            const SDL_GPUTransferBufferLocation dst{down_,
                                                    static_cast<Uint32>(i * kTileBytes8)};
            SDL_DownloadFromGPUBuffer(copy, &src, &dst);
        }
        SDL_EndGPUCopyPass(copy);

        SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        if (fence == nullptr) return cpuFallback(doc, x, y, w, h);
        const bool waited = SDL_WaitForGPUFences(dev_, true, &fence, 1);
        SDL_ReleaseGPUFence(dev_, fence);
        if (!waited) return cpuFallback(doc, x, y, w, h);

        const auto* mapped =
            static_cast<const PremulRgba8*>(SDL_MapGPUTransferBuffer(dev_, down_, false));
        if (mapped == nullptr) return cpuFallback(doc, x, y, w, h);

        for (std::size_t i = 0; i < n; ++i) {
            const std::int32_t ox = keyScratch_[base + i].first  * TILE_SIZE;
            const std::int32_t oy = keyScratch_[base + i].second * TILE_SIZE;
            const std::int32_t cx0 = std::max(x0, ox), cx1 = std::min(x1, ox + TILE_SIZE);
            const std::int32_t cy0 = std::max(y0, oy), cy1 = std::min(y1, oy + TILE_SIZE);
            for (std::int32_t py = cy0; py < cy1; ++py)
                std::memcpy(out.data() + static_cast<std::size_t>(py - y) * w + (cx0 - x),
                            mapped + i * TILE_PIXELS +
                                static_cast<std::size_t>(py - oy) * TILE_SIZE + (cx0 - ox),
                            static_cast<std::size_t>(cx1 - cx0) * 4);
        }
        SDL_UnmapGPUTransferBuffer(dev_, down_);
    }
    return out;
}

StraightRgba8 GpuBackend::pickColour(const Document& doc, std::int32_t x,
                                     std::int32_t y) {
    if (x < 0 || y < 0 || x >= doc.width || y >= doc.height) return StraightRgba8{};
    // Through the compositor rather than beside it: #13's strongest argument
    // is that one compositor serves the screen, the export and the eyedropper,
    // so Alt+click cannot report a colour that is not on the canvas. It costs
    // one tile's round trip, which is a click and not a frame.
    const std::vector<PremulRgba8> px = compositeRect(doc, x, y, 1, 1);
    return px.empty() ? StraightRgba8{} : px[0].unpremultiply();
}

std::expected<void, Error> GpuBackend::readback(const Document& doc) {
    submitPending();
    // const_cast: `PaintBackend::readback` takes a const Document precisely
    // because a backend writes tiles back through pointers of its own. The
    // tiles are not const, only this view of them is.
    bool ok = true;
    for (const Layer& layer : doc.layers) ok = syncLayer(const_cast<Layer&>(layer)) && ok;
    reconcile(doc);
    if (ok) return {};

    // Reported down both channels on purpose. The callers inside the engine
    // that cannot continue without their pixels take the `std::expected`; the
    // ones that only pass through — undo, a layer operation, a save — discard
    // it, and `takeError` is what carries the news to the app for them.
    Error failure{ErrorKind::Io, "could not read painted tiles back from the GPU: " +
                                     std::string(SDL_GetError())};
    recordFailure(failure);
    return std::unexpected(std::move(failure));
}

// --------------------------------------------------- everything else, on host
//
// Fills, transforms, merges and clears are click-frequency and land on host
// tiles the reference implementation already handles correctly. Each reads the
// pixels back first and then lets the CPU backend run; the host writes move
// every touched tile's stamp, which is what drops the device copies. #12 calls
// these out as acceptable stalls — the measurements are in the PR.

UndoRecord GpuBackend::bucketFill(Document& doc, LayerId target, std::int32_t x,
                                  std::int32_t y, StraightRgba8 colour, int tolerance,
                                  bool toMask) {
    if (const auto ready = readback(doc); !ready.has_value()) {
        recordFailure(ready.error());
        return {};
    }
    // floodFill, not cpuBackend().bucketFill: the region has to be found on
    // the image THIS backend composited, or the fill stops at a boundary the
    // artist cannot see.
    return floodFill(doc, target, x, y, colour, tolerance, toMask);
}

UndoRecord GpuBackend::fillSelection(Document& doc, LayerId target, StraightRgba8 c,
                                     bool toMask) {
    if (const auto ready = readback(doc); !ready.has_value()) {
        recordFailure(ready.error());
        return {};
    }
    return cpuBackend().fillSelection(doc, target, c, toMask);
}

UndoRecord GpuBackend::transformRegion(Document& doc, LayerId target,
                                       const Selection& source,
                                       const Transform& transform) {
    if (const auto ready = readback(doc); !ready.has_value()) {
        recordFailure(ready.error());
        return {};
    }
    return cpuBackend().transformRegion(doc, target, source, transform);
}

UndoRecord GpuBackend::clearLayer(Layer& layer, bool toMask) {
    submitPending();
    // The undo record has to hold what was painted, not what the host last saw.
    if (!syncLayer(layer))
        recordFailure(Error{ErrorKind::Io,
                            "could not read painted tiles back from the GPU: " +
                                std::string(SDL_GetError())});
    UndoRecord rec = cpuBackend().clearLayer(layer, toMask);
    dropLayer(layer.id);
    return rec;
}

UndoRecord GpuBackend::mergeLayerDown(Document& doc, LayerId id) {
    if (const auto ready = readback(doc); !ready.has_value()) {
        recordFailure(ready.error());
        return {};
    }
    UndoRecord rec = cpuBackend().mergeLayerDown(doc, id);
    dropLayer(id);
    return rec;
}

}  // namespace

bool gpuBackendCompiledIn() noexcept { return true; }

std::unique_ptr<PaintBackend> makeGpuBackend(std::string* why) {
    // Refcounted, and an application has almost always done it already. No
    // window and no event queue: D-022 draws the engine's line there, and
    // SDL_GPU needs neither.
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        // A machine with no display server can still have a device. The
        // headless engine tests reach the GPU this way, which is the only
        // reason they can check it against the CPU at all.
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            if (why != nullptr)
                *why = std::string("no video subsystem: ") + SDL_GetError();
            return nullptr;
        }
    }

    SDL_GPUDevice* device =
        SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, nullptr);
    if (device == nullptr) {
        // D-025: SPIR-V only, so this is also the "Direct3D or Metal machine"
        // path. The CPU backend is already the default; nothing to do.
        if (why != nullptr) *why = std::string("no SPIR-V GPU device: ") + SDL_GetError();
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return nullptr;
    }

    auto backend = std::make_unique<GpuBackend>(device);
    std::string reason;
    if (!backend->start(&reason)) {
        if (why != nullptr) *why = reason;
        return nullptr;
    }
    if (why != nullptr) *why = std::string("GPU: ") + SDL_GetGPUDeviceDriver(device);
    return backend;
}

std::size_t gpuDeviceBytes(const PaintBackend& backend) noexcept {
    const auto* gpu = dynamic_cast<const GpuBackend*>(&backend);
    return gpu != nullptr ? gpu->deviceBytes() : 0;
}

}  // namespace sbl

#else   // SABLE_HAVE_GPU

namespace sbl {

bool gpuBackendCompiledIn() noexcept { return false; }

std::unique_ptr<PaintBackend> makeGpuBackend(std::string* why) {
    if (why != nullptr) *why = "built without SDL3";
    return nullptr;
}

std::size_t gpuDeviceBytes(const PaintBackend&) noexcept { return 0; }

}  // namespace sbl

#endif  // SABLE_HAVE_GPU
