#include <array>
#include <limits>
#include <map>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <vector>
#include <string>
#include <optional>

#include "modloader/shared/modloader.hpp"

#include "beatsaber-hook/shared/utils/typedefs.h"
#include "beatsaber-hook/shared/utils/il2cpp-utils.hpp"
#include "beatsaber-hook/shared/utils/logging.hpp"
#include "beatsaber-hook/shared/utils/utils.h"
#include "beatsaber-hook/shared/utils/hooking.hpp"

#include "GlobalNamespace/BeatmapCharacteristicSO.hpp"
#include "GlobalNamespace/BeatmapData.hpp"
#include "GlobalNamespace/BeatmapDataLoader.hpp"
#include "GlobalNamespace/BeatmapEventData.hpp"
#include "GlobalNamespace/BeatmapLineData.hpp"
#include "GlobalNamespace/BeatmapObjectData.hpp"
#include "GlobalNamespace/BeatmapObjectSpawnMovementData.hpp"
#include "GlobalNamespace/BeatmapObjectType.hpp"
#include "GlobalNamespace/BeatmapSaveData_EventData.hpp"
#include "GlobalNamespace/BeatmapSaveData_NoteData.hpp"
#include "GlobalNamespace/BeatmapSaveData_ObstacleData.hpp"
#include "GlobalNamespace/IDifficultyBeatmap.hpp"
#include "GlobalNamespace/IDifficultyBeatmapSet.hpp"
#include "GlobalNamespace/NoteCutDirectionExtensions.hpp"
#include "GlobalNamespace/NoteData.hpp"
#include "GlobalNamespace/NoteLineLayer.hpp"
#include "GlobalNamespace/ObstacleController.hpp"
#include "GlobalNamespace/ObstacleData.hpp"
#include "GlobalNamespace/SimpleColorSO.hpp"
#include "GlobalNamespace/SpawnRotationProcessor.hpp"
#include "GlobalNamespace/StandardLevelDetailView.hpp"
#include "GlobalNamespace/StretchableObstacle.hpp"
#include "GlobalNamespace/FlyingScoreSpawner.hpp"
#include "GlobalNamespace/MainMenuViewController.hpp"
#include "GlobalNamespace/BeatmapObjectSpawnController.hpp"
#include "GlobalNamespace/BeatmapObjectSpawnController_InitData.hpp"
#include "GlobalNamespace/BeatmapObjectExecutionRatingsRecorder.hpp"
#include "GlobalNamespace/BeatmapDataObstaclesMergingTransform.hpp"
#include "System/Collections/Generic/List_1.hpp"
#include "System/Decimal.hpp"
#include "System/Single.hpp"
#include "UnityEngine/Camera.hpp"
#include "UnityEngine/Color.hpp"
#include "UnityEngine/Graphics.hpp"
#include "UnityEngine/Resources.hpp"
#include "UnityEngine/Quaternion.hpp"
#include "UnityEngine/Vector2.hpp"
#include "UnityEngine/Vector3.hpp"
#include "pinkcore/shared/RequirementAPI.hpp"


using namespace GlobalNamespace;
using namespace System::Collections;


static ModInfo modInfo;

Logger& logger()
{
    static auto logger = new Logger(modInfo, LoggerOptions(false, true));
    return *logger;
}

extern "C" void setup(ModInfo& info)
{
    info.id = "MappingExtensions";
    info.version = "0.20.4";
    modInfo = info;
    logger().info("Leaving setup!");
}

[[maybe_unused]] static void dump_real(int before, int after, void* ptr)
{
    logger().info("Dumping Immediate Pointer: %p: %lx", ptr, *reinterpret_cast<long*>(ptr));
    auto begin = static_cast<long*>(ptr) - before;
    auto end   = static_cast<long*>(ptr) + after;
    for (auto cur = begin; cur != end; ++cur) {
        logger().info("0x%lx: %lx", (long)cur - (long)ptr, *cur);
    }
}

// Normalized indices are faster to compute & reverse, and more accurate than, effective indices (see below).
// A "normalized" precision index is an effective index * 1000. So unlike normal precision indices, only 0 is 0.
int ToNormalizedPrecisionIndex(int index)
{
    if (index <= -1000) {
        return index + 1000;
    } else if (index >= 1000) {
        return index - 1000;
    } else {
        return index * 1000; // wasn't precision yet
    }
}
int FromNormalizedPrecisionIndex(int index)
{
    if (index % 1000 == 0) {
        return index / 1000;
    } else if (index > 0) {
        return index + 1000;
    } else {
        return index - 1000;
    }
}

// An effective index is a normal/extended index, but with decimal places that do what you'd expect.
float ToEffectiveIndex(int index)
{
    float effectiveIndex = index;
    if (effectiveIndex <= -1000) {
        effectiveIndex = effectiveIndex / 1000.0f + 1.0f;
    } else if (effectiveIndex >= 1000) {
        effectiveIndex = effectiveIndex / 1000.0f - 1.0f;
    }
    return effectiveIndex;
}

static IDifficultyBeatmap* storedDiffBeatmap = nullptr;
static BeatmapCharacteristicSO* storedBeatmapCharacteristicSO = nullptr;
MAKE_HOOK_MATCH(StandardLevelDetailView_RefreshContent, &StandardLevelDetailView::RefreshContent, void, StandardLevelDetailView* self)
{
    StandardLevelDetailView_RefreshContent(self);
    storedBeatmapCharacteristicSO = self->get_selectedDifficultyBeatmap()->get_parentDifficultyBeatmapSet()->get_beatmapCharacteristic();
}
MAKE_HOOK_MATCH(MainMenuViewController_DidActivate, &MainMenuViewController::DidActivate, void, MainMenuViewController* self,
                     bool firstActivation, bool addedToHierarchy, bool screenSystemEnabling) {
    storedBeatmapCharacteristicSO = nullptr;
    return MainMenuViewController_DidActivate(self, firstActivation, addedToHierarchy, screenSystemEnabling);
}

static bool skipWallRatings = false;
MAKE_HOOK_MATCH(BeatmapObjectSpawnController_Start, &BeatmapObjectSpawnController::Start, void, BeatmapObjectSpawnController* self)
{
    if (storedDiffBeatmap) {
        float njs = storedDiffBeatmap->get_noteJumpMovementSpeed();
        if (njs < 0)
            self->dyn__initData()->dyn_noteJumpMovementSpeed() = njs;
    }
    skipWallRatings = false;

    return BeatmapObjectSpawnController_Start(self);
}

MAKE_HOOK_MATCH(BeatmapObjectExecutionRatingsRecorder_HandleObstacleDidPassAvoidedMark, &BeatmapObjectExecutionRatingsRecorder::HandleObstacleDidPassAvoidedMark, void, BeatmapObjectExecutionRatingsRecorder* self,
    ObstacleController* obstacleController)
{
    if (skipWallRatings) {
        return;
    } else {
        return BeatmapObjectExecutionRatingsRecorder_HandleObstacleDidPassAvoidedMark(self, obstacleController);
    }
}

/* PC version hooks */

MAKE_HOOK_MATCH(BeatmapObjectSpawnMovementData_GetNoteOffset, &BeatmapObjectSpawnMovementData::GetNoteOffset, UnityEngine::Vector3, BeatmapObjectSpawnMovementData* self,
    int noteLineIndex, GlobalNamespace::NoteLineLayer noteLineLayer)
{
    if (noteLineIndex == 4839) {
        logger().info("lineIndex %i and lineLayer %i!", noteLineIndex, noteLineLayer.value);
    }
    auto __result = BeatmapObjectSpawnMovementData_GetNoteOffset(self, noteLineIndex, noteLineLayer);

    if (noteLineIndex >= 1000 || noteLineIndex <= -1000) {
        if (noteLineIndex <= -1000)
            noteLineIndex += 2000;
        float num = -(self->get_noteLinesCount() - 1.0f) * 0.5f;
        num += ((float)noteLineIndex * self->get_noteLinesDistance() / 1000.0f);

        float yPos = self->LineYPosForLineLayer(noteLineLayer);
        __result   = UnityEngine::Vector3(num, yPos, 0.0f);
    }
    return __result;
}

MAKE_HOOK_MATCH(BeatmapObjectSpawnMovementData_HighestJumpPosYForLineLayer, &BeatmapObjectSpawnMovementData::HighestJumpPosYForLineLayer, float, BeatmapObjectSpawnMovementData* self,
    NoteLineLayer lineLayer)
{
    float __result = BeatmapObjectSpawnMovementData_HighestJumpPosYForLineLayer(self, lineLayer);
    float delta = (self->dyn__topLinesHighestJumpPosY() - self->dyn__upperLinesHighestJumpPosY());

    if (lineLayer >= 1000 || lineLayer <= -1000) {
        __result = self->dyn__upperLinesHighestJumpPosY() - delta - delta + self->dyn__jumpOffsetY() + (lineLayer * (delta / 1000.0f));
    } else if (lineLayer > 2 || lineLayer < 0) {
        __result = self->dyn__upperLinesHighestJumpPosY() - delta + self->dyn__jumpOffsetY() + (lineLayer * delta);
    }
    if (__result > 2.9f) {
        logger().warning("Extreme note jump! lineLayer %i gave jump %f!", (int)lineLayer, __result);
    }
    return __result;
}

MAKE_HOOK_MATCH(BeatmapObjectSpawnMovementData_LineYPosForLineLayer, &BeatmapObjectSpawnMovementData::LineYPosForLineLayer, float, BeatmapObjectSpawnMovementData* self,
    NoteLineLayer lineLayer)
{
    float __result = BeatmapObjectSpawnMovementData_LineYPosForLineLayer(self, lineLayer);
    // if (!Plugin.active) return __result;
    float delta = (self->dyn__topLinesYPos() - self->dyn__upperLinesYPos());

    if (lineLayer >= 1000 || lineLayer <= -1000) {
        __result = self->dyn__upperLinesYPos() - delta - delta + (lineLayer * delta / 1000.0f);
    } else if (lineLayer > 2 || lineLayer < 0) {
        __result = self->dyn__upperLinesYPos() - delta + (lineLayer * delta);
    }
    if (__result > 1.9f) {
        logger().warning("Extreme note position! lineLayer %i gave YPos %f!", (int)lineLayer, __result);
    }
    return __result;
}

MAKE_HOOK_MATCH(BeatmapObjectSpawnMovementData_Get2DNoteOffset, &BeatmapObjectSpawnMovementData::Get2DNoteOffset, UnityEngine::Vector2, BeatmapObjectSpawnMovementData* self,
    int noteLineIndex, GlobalNamespace::NoteLineLayer noteLineLayer)
{
    if (noteLineIndex == 4839) {
        logger().info("lineIndex %i and lineLayer %i!", noteLineIndex, noteLineLayer.value);
    }
    auto __result = BeatmapObjectSpawnMovementData_Get2DNoteOffset(self, noteLineIndex, noteLineLayer);
    if (noteLineIndex >= 1000 || noteLineIndex <= -1000) {
        if (noteLineIndex <= -1000)
            noteLineIndex += 2000;
        float num = -(self->dyn__noteLinesCount() - 1.0f) * 0.5f;
        float x = num + ((float)noteLineIndex * self->dyn__noteLinesDistance() / 1000.0f);
        float y = self->LineYPosForLineLayer(noteLineLayer);
        __result = UnityEngine::Vector2(x, y);
    }
    return __result;
}

MAKE_HOOK_MATCH(FlyingScoreSpawner_SpawnFlyingScore, &FlyingScoreSpawner::SpawnFlyingScore, void, FlyingScoreSpawner* self, ByRef<GlobalNamespace::NoteCutInfo> noteCutInfo, int noteLineIndex,
    int multiplier, UnityEngine::Vector3 pos, UnityEngine::Quaternion rotation, UnityEngine::Quaternion inverseRotation,
    UnityEngine::Color color)
{
        if (noteLineIndex < 0)
            noteLineIndex = 0;
        if (noteLineIndex > 3)
            noteLineIndex = 3;
    return FlyingScoreSpawner_SpawnFlyingScore(self, noteCutInfo, noteLineIndex, multiplier, pos, rotation, inverseRotation, color);
}

MAKE_HOOK_MATCH(NoteCutDirectionExtensions_RotationAngle, &NoteCutDirectionExtensions::RotationAngle, float, NoteCutDirection cutDirection)
{
    float __result = NoteCutDirectionExtensions_RotationAngle(cutDirection);
    if (cutDirection >= 1000 && cutDirection <= 1360) {
        __result = 1000 - cutDirection;
    } else if (cutDirection >= 2000 && cutDirection <= 2360) {
        __result = 2000 - cutDirection;
    }
    return __result;
}
MAKE_HOOK_MATCH(NoteCutDirectionExtensions_Direction, &NoteCutDirectionExtensions::Direction, UnityEngine::Vector2, NoteCutDirection cutDirection)
{
    UnityEngine::Vector2 __result = NoteCutDirectionExtensions_Direction(cutDirection);
    // if (!Plugin.active) return __result;
    if ((cutDirection >= 1000 && cutDirection <= 1360) ||
        (cutDirection >= 2000 && cutDirection <= 2360))
    {
        // uses RotationAngle hook indirectly
        auto quaternion = NoteCutDirectionExtensions::Rotation(cutDirection, 0.0f);
        static auto forward = UnityEngine::Vector3::get_forward();
        UnityEngine::Vector3 dir = quaternion * forward;
        __result = UnityEngine::Vector2(dir.x, dir.y);
        // logger().debug("NoteCutDirectionExtensions: {%f, %f}", dir.x, dir.y);
    }
    return __result;
}

bool MirrorPrecisionLineIndex(int lineIndex, int lineCount)
{
    if (lineIndex >= 1000 || lineIndex <= -1000) {
        bool notVanillaRange = (lineIndex <= 0 || lineIndex > lineCount * 1000);

        int newIndex = (lineCount + 1) * 1000 - lineIndex;
        if (notVanillaRange)
            newIndex -= 2000; // this fixes the skip between 1000 and -1000 which happens once iff start or end is negative
        lineIndex = newIndex;
        return true;
    }
    return false;
}

MAKE_HOOK_MATCH(NoteCutDirection_Mirror, &NoteData::Mirror, void, NoteData* self, int lineCount)
{
    int lineIndex = self->get_lineIndex();
    int flipLineIndex = self->get_flipLineIndex();
    NoteCutDirection_Mirror(self, lineCount);
    if (MirrorPrecisionLineIndex(lineIndex, lineCount)) {
        self->set_lineIndex(lineIndex);
    }
    if (MirrorPrecisionLineIndex(flipLineIndex, lineCount)) {
        self->set_flipLineIndex(flipLineIndex);
    }
}

MAKE_HOOK_MATCH(NoteCutDirection_MirrorTransformCutDirection, &NoteData::Mirror, void, NoteData* self, int lineCount)
{
    int state = self->get_cutDirection().value;
    NoteCutDirection_MirrorTransformCutDirection(self, lineCount);
    if (state >= 1000) {
        int newdir         = 2360 - state;
        self->set_cutDirection(newdir);
    }
}
MAKE_HOOK_MATCH(ObstacleController_Init, &ObstacleController::Init, void, ObstacleController* self, ObstacleData* obstacleData, float worldRotation,
    UnityEngine::Vector3 startPos, UnityEngine::Vector3 midPos, UnityEngine::Vector3 endPos, float move1Duration,
    float move2Duration, float singleLineWidth, float height)
{
    ObstacleController_Init(
        self, obstacleData, worldRotation, startPos, midPos, endPos, move1Duration, move2Duration, singleLineWidth, height);
    if ((obstacleData->get_obstacleType().value < 1000) && !(obstacleData->get_width() >= 1000))
        return;
    // Either wall height or wall width are precision

    skipWallRatings = true;
    int mode        = (obstacleData->get_obstacleType().value >= 4001 && obstacleData->get_obstacleType().value <= 4100000) ? 1 : 0;
    int obsHeight;
    int startHeight = 0;
    if (mode == 1) {
        int value = obstacleData->get_obstacleType().value;
        value -= 4001;
        obsHeight = value / 1000;
        startHeight = value % 1000;
    } else {
        int value = obstacleData->get_obstacleType().value;
        obsHeight = value - 1000; // won't be used unless height is precision
    }

    float num = (float)obstacleData->get_width() * singleLineWidth;
    if ((obstacleData->get_width() >= 1000) || (mode == 1)) {
        if (obstacleData->get_width() >= 1000) {
            float width              = (float)obstacleData->get_width() - 1000.0f;
            float precisionLineWidth = singleLineWidth / 1000.0f;
            num                      = width * precisionLineWidth;
        }
        // Change y of b for start height
        UnityEngine::Vector3 b { b.x = (num - singleLineWidth) * 0.5f, b.y = 4 * ((float)startHeight / 1000), b.z = 0 };

        self->dyn__startPos() = startPos + b;
        self->dyn__midPos()   = midPos + b;
        self->dyn__endPos()   = endPos + b;
    }

    float num2       = UnityEngine::Vector3::Distance(self->dyn__endPos(), self->dyn__midPos()) / move2Duration;
    float length     = num2 * obstacleData->get_duration();
    float multiplier = 1;
    if (obstacleData->get_obstacleType().value >= 1000) {
        multiplier = (float)obsHeight / 1000;
    }

    self->dyn__stretchableObstacle()->SetSizeAndColor((num * 0.98f), (height * multiplier), length, self->get_color());
    self->dyn__bounds() = self->dyn__stretchableObstacle()->dyn__bounds();
}

MAKE_HOOK_MATCH(ObstacleData_Mirror, &ObstacleData::Mirror, void, ObstacleData* self, int lineCount)
{
    int __state         = self->get_lineIndex();
    bool precisionWidth = (self->get_width() >= 1000);
    ObstacleData_Mirror(self, lineCount);

    logger().debug("lineCount: %i", lineCount);
    if (__state >= 1000 || __state <= -1000 || precisionWidth) {
        int normIndex = ToNormalizedPrecisionIndex(__state);
        int normWidth = ToNormalizedPrecisionIndex(self->get_width());

        // The vanilla formula * 1000
        int normNewIndex = lineCount * 1000 - normWidth - normIndex;

        self->set_lineIndex(FromNormalizedPrecisionIndex(normNewIndex));
        logger().debug("wall (of type %i) with lineIndex %i (norm %i) and width %i (norm %i) mirrored to %i (norm %i)",(int)(self->get_obstacleType()), __state, 
        normIndex, self->get_width(), normWidth, self->get_lineIndex(), normNewIndex);
    }
}

MAKE_HOOK_MATCH(SpawnRotationProcessor_RotationForEventValue, &SpawnRotationProcessor::RotationForEventValue, float, SpawnRotationProcessor* self, int index)
{
    float __result = SpawnRotationProcessor_RotationForEventValue(self, index);
    if (!storedBeatmapCharacteristicSO->dyn__requires360Movement()) return __result;
    if (index >= 1000 && index <= 1720)
        __result = index - 1360;
    return __result;
}

/* End of PC version hooks */

MAKE_HOOK_MATCH(BeatmapDataObstaclesMergingTransform_CreateTransformedData, &BeatmapDataObstaclesMergingTransform::CreateTransformedData, IReadonlyBeatmapData *, IReadonlyBeatmapData *beatmapData) {
    return beatmapData;
}

extern "C" void load() {
PinkCore::RequirementAPI::RegisterInstalled("Mapping Extensions");    


    logger().info("Installing ME Hooks, please wait");
    il2cpp_functions::Init();

    Logger& hookLogger = logger();

    INSTALL_HOOK(hookLogger, StandardLevelDetailView_RefreshContent);
    INSTALL_HOOK(hookLogger, MainMenuViewController_DidActivate);
    INSTALL_HOOK(hookLogger, BeatmapObjectSpawnMovementData_GetNoteOffset);
    INSTALL_HOOK(hookLogger, BeatmapObjectSpawnMovementData_HighestJumpPosYForLineLayer);
    INSTALL_HOOK(hookLogger, BeatmapObjectSpawnMovementData_LineYPosForLineLayer);
    INSTALL_HOOK(hookLogger, BeatmapObjectSpawnMovementData_Get2DNoteOffset);
    INSTALL_HOOK(hookLogger, FlyingScoreSpawner_SpawnFlyingScore);
    INSTALL_HOOK(hookLogger, NoteCutDirectionExtensions_RotationAngle);
    INSTALL_HOOK(hookLogger, NoteCutDirectionExtensions_Direction);
    INSTALL_HOOK(hookLogger, NoteCutDirection_Mirror);
    INSTALL_HOOK(hookLogger, ObstacleController_Init);
    INSTALL_HOOK(hookLogger, ObstacleData_Mirror);
    INSTALL_HOOK(hookLogger, SpawnRotationProcessor_RotationForEventValue);
    INSTALL_HOOK(hookLogger, NoteCutDirection_MirrorTransformCutDirection);
    INSTALL_HOOK(hookLogger, BeatmapObjectSpawnController_Start);
    INSTALL_HOOK(hookLogger, BeatmapObjectExecutionRatingsRecorder_HandleObstacleDidPassAvoidedMark);
    INSTALL_HOOK(hookLogger, BeatmapDataObstaclesMergingTransform_CreateTransformedData);

    logger().info("Installed ME Hooks successfully!");
}