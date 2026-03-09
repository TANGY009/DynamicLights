#include "mod/DynamicLight.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/mod/RegisterHelper.h"

#include "mc/client/player/LocalPlayer.h"
#include "mc/common/Brightness.h"
#include "mc/deps/core/string/HashedString.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/ChunkBlockPos.h"
#include "mc/world/level/ILevel.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Tick.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/registry/BlockTypeRegistry.h"
#include "mc/world/level/chunk/LevelChunk.h"
#include "mc/world/level/dimension/Dimension.h"

#include <optional>
#include <cmath>
#include <string>
#include <string_view>

namespace mod {

namespace {

using SharedTypes::Legacy::ArmorSlot;

inline constexpr std::string_view kShieldItemName       = "minecraft:shield";
inline constexpr std::string_view kMiningHelmetItemName = "minecraft:golden_helmet";
inline constexpr std::string_view kPumpkinBlockName     = "minecraft:lit_pumpkin";

struct LightCandidate {
    uchar       brightness = 0;
    std::string fakeBlockName;
    int         priority = 0;

    [[nodiscard]] bool isActive() const { return brightness > 0 && !fakeBlockName.empty(); }
};

struct AppliedLightState {
    Dimension*  dimension = nullptr;
    ILevel*     level     = nullptr;
    BlockPos    position{};
    std::string fakeBlockName;
    uchar       brightness = 0;

    [[nodiscard]] bool isActive() const {
        return dimension != nullptr && level != nullptr && !fakeBlockName.empty() && brightness > 0;
    }
};

std::optional<AppliedLightState> gAppliedLight;
std::optional<AppliedLightState> gTrailingLight;
std::optional<uchar>             gPumpkinBrightness;

inline constexpr float kTrailingLightKeepDistance = 1.05F;

[[nodiscard]] uchar clampBrightness(uchar value) { return value > 15 ? 15 : value; }

[[nodiscard]] uchar getBlockBrightness(Block const& block) {
    return clampBrightness(block.mDirectData->mLightEmission.get().mValue);
}

[[nodiscard]] uchar getPumpkinBrightness(BlockTypeRegistry const& blockTypeRegistry) {
    if (!gPumpkinBrightness.has_value()) {
        auto const& pumpkinBlock = blockTypeRegistry.getDefaultBlockState(HashedString{kPumpkinBlockName}, false);
        gPumpkinBrightness       = getBlockBrightness(pumpkinBlock);
    }
    return *gPumpkinBrightness;
}

[[nodiscard]] std::optional<LightCandidate> getBlockItemCandidate(ItemStack const& itemStack, int priority) {
    if (itemStack.isNull() || !itemStack.isBlock()) {
        return std::nullopt;
    }

    auto const* block = itemStack.getBlockForRendering();
    if (block == nullptr) {
        return std::nullopt;
    }

    auto const brightness = getBlockBrightness(*block);
    if (brightness == 0) {
        return std::nullopt;
    }

    return LightCandidate{brightness, block->getTypeName(), priority};
}

[[nodiscard]] std::optional<LightCandidate> getFallbackCandidate(
    ItemStack const&         itemStack,
    BlockTypeRegistry const& blockTypeRegistry,
    int                      priority,
    bool                     helmetMode
) {
    if (itemStack.isNull()) {
        return std::nullopt;
    }

    auto const* item = itemStack.getItem();
    if (item == nullptr) {
        return std::nullopt;
    }

    auto const& fullName = item->mFullName.get();
    if ((!helmetMode && fullName == kShieldItemName) || (helmetMode && fullName == kMiningHelmetItemName)) {
        return LightCandidate{getPumpkinBrightness(blockTypeRegistry), std::string{kPumpkinBlockName}, priority};
    }

    return std::nullopt;
}

[[nodiscard]] LightCandidate chooseLightCandidate(LocalPlayer& player) {
    auto const& blockTypeRegistry = *player.getLevel().getBlockTypeRegistry();
    auto const& inventory         = player.getInventory();

    auto const& mainHandItem = inventory.getItem(player.getSelectedItemSlot());
    auto const& offhandItem  = player.getOffhandSlot();
    auto const& headItem     = player.getArmor(ArmorSlot::Head);

    LightCandidate best{};
    auto           takeCandidate = [&best](std::optional<LightCandidate> candidate) {
        if (!candidate.has_value()) {
            return;
        }
        if (!best.isActive() || candidate->brightness > best.brightness
            || (candidate->brightness == best.brightness && candidate->priority < best.priority)) {
            best = std::move(*candidate);
        }
    };

    takeCandidate(getBlockItemCandidate(mainHandItem, 0));
    takeCandidate(getBlockItemCandidate(offhandItem, 1));
    takeCandidate(getFallbackCandidate(offhandItem, blockTypeRegistry, 1, false));
    takeCandidate(getFallbackCandidate(headItem, blockTypeRegistry, 2, true));

    return best;
}

[[nodiscard]] std::optional<Block const*> resolveFakeBlock(ILevel& level, std::string_view blockName) {
    if (blockName.empty()) {
        return std::nullopt;
    }

    auto const& block = level.getBlockTypeRegistry()->getDefaultBlockState(HashedString{blockName}, false);
    return std::addressof(block);
}

[[nodiscard]] bool isSameLight(
    AppliedLightState const& applied,
    Dimension const&         dimension,
    BlockPos const&          position,
    std::string_view         fakeBlockName
) {
    return applied.isActive() && applied.dimension == std::addressof(dimension) && applied.position == position
        && applied.fakeBlockName == fakeBlockName;
}

[[nodiscard]] bool isAdjacentBlock(BlockPos const& lhs, BlockPos const& rhs) {
    auto const dx = std::abs(lhs.x - rhs.x);
    auto const dy = std::abs(lhs.y - rhs.y);
    auto const dz = std::abs(lhs.z - rhs.z);
    return dx + dy + dz == 1;
}

[[nodiscard]] float distanceToBlockCenterXZ(Vec3 const& playerPos, BlockPos const& blockPos) {
    auto const centerX = static_cast<float>(blockPos.x) + 0.5F;
    auto const centerZ = static_cast<float>(blockPos.z) + 0.5F;
    auto const dx      = playerPos.x - centerX;
    auto const dz      = playerPos.z - centerZ;
    return std::sqrt(dx * dx + dz * dz);
}

[[nodiscard]] bool invokeLightingCallbacks(
    ILevel&            level,
    BlockSource const& blockSource,
    BlockPos const&    position,
    std::string_view   fakeBlockName,
    bool               applyFakeLight
) {
    auto* chunk = blockSource.getChunkAt(position);
    if (chunk == nullptr || chunk->mReadOnly) {
        return false;
    }

    auto const fakeBlock = resolveFakeBlock(level, fakeBlockName);
    if (!fakeBlock.has_value()) {
        return false;
    }

    auto const& actualBlock = blockSource.getBlock(position);
    auto const  localPos    = ChunkBlockPos{position, blockSource.getMinHeight()};

    if (applyFakeLight) {
        chunk->_lightingCallbacks(localPos, actualBlock, **fakeBlock, const_cast<BlockSource*>(&blockSource));
    } else {
        chunk->_lightingCallbacks(localPos, **fakeBlock, actualBlock, const_cast<BlockSource*>(&blockSource));
    }

    return true;
}

void clearOneLight(std::optional<AppliedLightState>& lightState) {
    if (!lightState.has_value() || !lightState->isActive()) {
        lightState.reset();
        return;
    }

    auto& applied     = *lightState;
    auto& blockSource = applied.dimension->getBlockSourceFromMainChunkSource();
    static_cast<void>(
        invokeLightingCallbacks(*applied.level, blockSource, applied.position, applied.fakeBlockName, false)
    );
    lightState.reset();
}

void clearAppliedLight() {
    clearOneLight(gAppliedLight);
    clearOneLight(gTrailingLight);
}

void updateTrailingLight(LocalPlayer& player, LightCandidate const& candidate) {
    if (!gTrailingLight.has_value()) {
        return;
    }

    if (!gAppliedLight.has_value() || !candidate.isActive()) {
        clearOneLight(gTrailingLight);
        return;
    }

    auto const& trailing = *gTrailingLight;
    if (!trailing.isActive() || trailing.fakeBlockName != candidate.fakeBlockName) {
        clearOneLight(gTrailingLight);
        return;
    }

    if (!isAdjacentBlock(trailing.position, gAppliedLight->position)) {
        clearOneLight(gTrailingLight);
        return;
    }

    if (distanceToBlockCenterXZ(player.getPosition(), trailing.position) > kTrailingLightKeepDistance) {
        clearOneLight(gTrailingLight);
    }
}

void updateAppliedLight(LocalPlayer& player) {
    auto const  candidate   = chooseLightCandidate(player);
    auto&       dimension   = player.getDimension();
    auto&       level       = player.getLevel();
    auto const  position    = BlockPos{player.getPosition()};
    auto const& blockSource = player.getDimensionBlockSourceConst();

    auto* chunk = blockSource.getChunkAt(position);
    if (chunk == nullptr || chunk->mReadOnly) {
        clearAppliedLight();
        return;
    }

    auto const& actualBlock      = blockSource.getBlock(position);
    auto const  actualBrightness = getBlockBrightness(actualBlock);

    if (gAppliedLight.has_value()) {
        auto const& applied = *gAppliedLight;
        auto const sameMain = candidate.isActive() && isSameLight(applied, dimension, position, candidate.fakeBlockName);
        if (!sameMain || candidate.brightness <= actualBrightness) {
            if (candidate.isActive() && candidate.brightness > actualBrightness && applied.isActive()
                && applied.dimension == std::addressof(dimension) && applied.level == std::addressof(level)
                && applied.fakeBlockName == candidate.fakeBlockName
                && isAdjacentBlock(applied.position, position)) {
                clearOneLight(gTrailingLight);
                gTrailingLight = applied;
                gAppliedLight.reset();
            } else {
                clearOneLight(gAppliedLight);
                clearOneLight(gTrailingLight);
            }
        }
    }

    if (!candidate.isActive() || candidate.brightness <= actualBrightness || gAppliedLight.has_value()) {
        updateTrailingLight(player, candidate);
        return;
    }

    if (!invokeLightingCallbacks(level, blockSource, position, candidate.fakeBlockName, true)) {
        return;
    }

    gAppliedLight = AppliedLightState{
        .dimension     = std::addressof(dimension),
        .level         = std::addressof(level),
        .position      = position,
        .fakeBlockName = candidate.fakeBlockName,
        .brightness    = candidate.brightness,
    };

    updateTrailingLight(player, candidate);
}

LL_TYPE_INSTANCE_HOOK(
    LocalPlayerTickWorldHook,
    HookPriority::Normal,
    LocalPlayer,
    &LocalPlayer::$tickWorld,
    void,
    Tick const& currentTick
) {
    origin(currentTick);
    updateAppliedLight(*thisFor<LocalPlayer>());
}

} // namespace

DynamicLight& DynamicLight::getInstance() {
    static DynamicLight instance;
    return instance;
}

bool DynamicLight::load() {
    getSelf().getLogger().debug("Loading...");
    return true;
}

bool DynamicLight::enable() {
    getSelf().getLogger().debug("Enabling...");

    if (auto const result = LocalPlayerTickWorldHook::hook(); result != 0) {
        getSelf().getLogger().error("Failed to hook LocalPlayer::tickWorld, error code: {}", result);
        return false;
    }

    return true;
}

bool DynamicLight::disable() {
    getSelf().getLogger().debug("Disabling...");
    clearAppliedLight();

    if (!LocalPlayerTickWorldHook::unhook()) {
        getSelf().getLogger().error("Failed to unhook LocalPlayer::tickWorld");
        return false;
    }

    return true;
}

} // namespace mod

LL_REGISTER_MOD(mod::DynamicLight, mod::DynamicLight::getInstance());
