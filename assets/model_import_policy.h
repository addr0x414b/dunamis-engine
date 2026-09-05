#ifndef MODEL_IMPORT_POLICY_H
#define MODEL_IMPORT_POLICY_H

#include <cstdint>
#include <filesystem>
#include <string>

#include <assimp/postprocess.h>

namespace model_loading {

// This policy is part of the CPU asset identity in source, even though the
// in-process cache does not need a key field: the policy is fixed for the
// lifetime of an engine process. Increment it if import semantics change.
inline constexpr std::uint32_t importedGeometryPolicyVersion = 1;

// Before the meter migration, legacy scene transforms interpreted numeric
// asset coordinates as Dunamis units with 100 units per meter.
inline constexpr float legacyDunamisUnitsPerMeter = 100.0f;
inline constexpr float legacyAssetScaleFactor =
    1.0f / legacyDunamisUnitsPerMeter;

// Assimp's GlobalScale consumes importer-provided source scale metadata before
// PreTransformVertices bakes node transforms into mesh vertices. In the pinned
// Assimp 6.0.5 implementation this is the format-aware FBX conversion:
// UnitScaleFactor * 0.01. The FBX AI_CONFIG_FBX_CONVERT_TO_M setting is not
// used by that implementation, so it is intentionally not enabled here.
constexpr unsigned int meterNormalizedImportFlags() noexcept {
    return aiProcess_Triangulate | aiProcess_FlipUVs |
           aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace |
           aiProcess_GlobalScale | aiProcess_PreTransformVertices |
           aiProcess_FindInvalidData;
}

struct LegacyAssetScaleMigration {
    float scaleFactor = 1.0f;
    bool known = false;
    std::string evidence;
};

// Determine the factor that converts a legacy scene's authored object scale
// to the normalized meter-valued geometry basis. FBX uses its actual Assimp
// UnitScaleFactor metadata. Assimp 6.0.5's other current importers do not set
// an importer file scale, so formats without reliable unit metadata use the
// documented convention that their numeric coordinates are meters (including
// OBJ).
[[nodiscard]] LegacyAssetScaleMigration inspectLegacyAssetScaleMigration(
    const std::filesystem::path& modelPath);

}  // namespace model_loading

#endif
