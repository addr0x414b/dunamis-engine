#include "model_import_policy.h"

#include <assimp/Importer.hpp>
#include <assimp/metadata.h>
#include <assimp/scene.h>

#include <cmath>
#include <exception>
#include <iomanip>
#include <sstream>

namespace model_loading {
namespace {

bool isFbxPath(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    for (char& character : extension) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return extension == ".fbx";
}

bool readUnitScaleFactor(const aiScene& scene, float& output) {
    if (scene.mMetaData == nullptr) return false;

    float floatValue = 0.0f;
    if (scene.mMetaData->Get("UnitScaleFactor", floatValue)) {
        output = floatValue;
        return true;
    }

    double doubleValue = 0.0;
    if (scene.mMetaData->Get("UnitScaleFactor", doubleValue)) {
        output = static_cast<float>(doubleValue);
        return true;
    }
    return false;
}

LegacyAssetScaleMigration unknownFbxScale(
    const std::filesystem::path& path, const std::string& reason) {
    LegacyAssetScaleMigration result;
    result.evidence = "Preserved legacy scale for FBX '" + path.string() +
                      "': " + reason;
    return result;
}

}  // namespace

LegacyAssetScaleMigration inspectLegacyAssetScaleMigration(
    const std::filesystem::path& modelPath) {
    // The extension only avoids a second full Assimp read for formats whose
    // pinned importers are known not to publish source-unit metadata. It does
    // not provide the FBX conversion factor; that value below comes from the
    // imported UnitScaleFactor metadata itself.
    if (!isFbxPath(modelPath)) {
        return {legacyAssetScaleFactor, true,
                "Unitless/non-FBX numeric coordinates were preserved; "
                "legacy 100-DU-per-meter scene scale becomes 0.01"};
    }

    Assimp::Importer importer;
    const aiScene* scene = nullptr;
    try {
        scene = importer.ReadFile(modelPath.generic_string(), 0);
    } catch (const std::exception& exception) {
        return unknownFbxScale(modelPath, exception.what());
    } catch (...) {
        return unknownFbxScale(modelPath, "Assimp threw an unknown exception");
    }
    if (scene == nullptr) {
        return unknownFbxScale(
            modelPath, importer.GetErrorString() &&
                              importer.GetErrorString()[0] != '\0'
                          ? importer.GetErrorString()
                          : "Assimp could not read the asset");
    }

    float unitScaleFactor = 0.0f;
    if (!readUnitScaleFactor(*scene, unitScaleFactor) ||
        !std::isfinite(unitScaleFactor) || unitScaleFactor <= 0.0f) {
        return unknownFbxScale(
            modelPath, "Assimp did not expose a finite positive UnitScaleFactor");
    }

    const float scaleFactor = 1.0f / unitScaleFactor;
    if (!std::isfinite(scaleFactor) || scaleFactor <= 0.0f) {
        return unknownFbxScale(
            modelPath, "UnitScaleFactor produced an invalid compatibility factor");
    }

    std::ostringstream evidence;
    evidence << "FBX UnitScaleFactor=" << std::setprecision(9)
             << unitScaleFactor << "; legacy authored scale factor="
             << scaleFactor;
    return {scaleFactor, true, evidence.str()};
}

}  // namespace model_loading
