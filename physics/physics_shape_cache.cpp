#include "physics_shape_cache.h"

#include <Jolt/Core/StreamWrapper.h>
#include <Jolt/Core/StreamUtils.h>
#include <Jolt/Physics/Collision/PhysicsMaterial.h>

#include <array>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <utility>

#include "physics_units.h"

namespace physics {
namespace {

constexpr std::array<char, 8> cacheMagic = {'D', 'U', 'N', 'P', 'H', 'Y',
                                             'S', '1'};
constexpr std::uint64_t fnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t fnvPrime = 1099511628211ULL;
constexpr std::uint32_t maximumHeaderStringBytes = 16U * 1024U;

class Fnv1a64 {
public:
    void addBytes(const void* data, std::size_t size) noexcept {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        for (std::size_t index = 0; index < size; ++index) {
            value_ ^= bytes[index];
            value_ *= fnvPrime;
        }
    }

    template <typename T>
    void add(const T& value) noexcept {
        addBytes(&value, sizeof(value));
    }

    void addString(const std::string& value) noexcept {
        const std::uint64_t size = value.size();
        add(size);
        addBytes(value.data(), value.size());
    }

    [[nodiscard]] std::uint64_t value() const noexcept { return value_; }

private:
    std::uint64_t value_ = fnvOffsetBasis;
};

std::uint32_t normalizedFloatBits(float value) noexcept {
    const float normalized = value == 0.0f ? 0.0f : value;
    std::uint32_t bits = 0;
    std::memcpy(&bits, &normalized, sizeof(bits));
    return bits;
}

void addMeshGeometry(Fnv1a64& hash,
                     const std::vector<MeshInstance>& meshInstances) noexcept {
    const std::uint64_t meshCount = meshInstances.size();
    hash.add(meshCount);
    for (const MeshInstance& instance : meshInstances) {
        const Mesh& mesh = instance.mesh;
        const std::uint64_t vertexCount = mesh.vertices.size();
        const std::uint64_t indexCount = mesh.indices.size();
        hash.add(vertexCount);
        hash.add(indexCount);
        for (const Vertex& vertex : mesh.vertices) {
            hash.add(normalizedFloatBits(vertex.pos.x));
            hash.add(normalizedFloatBits(vertex.pos.y));
            hash.add(normalizedFloatBits(vertex.pos.z));
        }
        for (const std::uint32_t index : mesh.indices) hash.add(index);
    }
}

void addIdentity(Fnv1a64& hash, const StaticMeshShapeCacheKey& key) noexcept {
    hash.addString(key.modelIdentity);
    hash.add(key.geometryFingerprint);
    for (const std::uint32_t bit : key.scaleBits) hash.add(bit);
    hash.add(key.colliderType);
    hash.add(physicsShapeCacheVersion);
    hash.add(physicsShapeCookingVersion);
    hash.add(physicsUnitConventionVersion);
    hash.add(normalizedFloatBits(dunamisUnitsPerMeter));
    hash.addString(joltShapeCacheCompatibilityTag);
}

template <typename T>
bool writeValue(std::ostream& stream, const T& value) {
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
    return static_cast<bool>(stream);
}

template <typename T>
bool readValue(std::istream& stream, T& value) {
    stream.read(reinterpret_cast<char*>(&value), sizeof(value));
    return static_cast<bool>(stream);
}

bool writeString(std::ostream& stream, const std::string& value) {
    if (value.size() > maximumHeaderStringBytes) return false;
    const std::uint32_t size = static_cast<std::uint32_t>(value.size());
    if (!writeValue(stream, size)) return false;
    stream.write(value.data(), static_cast<std::streamsize>(value.size()));
    return static_cast<bool>(stream);
}

bool readString(std::istream& stream, std::string& value) {
    std::uint32_t size = 0;
    if (!readValue(stream, size) || size > maximumHeaderStringBytes) return false;
    value.resize(size);
    stream.read(value.data(), static_cast<std::streamsize>(size));
    return static_cast<bool>(stream);
}

bool writeHeader(std::ostream& stream, const StaticMeshShapeCacheKey& key) {
    stream.write(cacheMagic.data(), static_cast<std::streamsize>(cacheMagic.size()));
    if (!stream || !writeValue(stream, physicsShapeCacheVersion) ||
        !writeValue(stream, physicsShapeCookingVersion) ||
        !writeValue(stream, physicsUnitConventionVersion) ||
        !writeValue(stream, normalizedFloatBits(dunamisUnitsPerMeter)) ||
        !writeString(stream, joltShapeCacheCompatibilityTag) ||
        !writeString(stream, key.modelIdentity) ||
        !writeValue(stream, key.geometryFingerprint)) {
        return false;
    }
    for (const std::uint32_t bit : key.scaleBits) {
        if (!writeValue(stream, bit)) return false;
    }
    return writeValue(stream, key.colliderType) && writeValue(stream, key.stableHash);
}

bool readAndValidateHeader(std::istream& stream,
                           const StaticMeshShapeCacheKey& expected,
                           std::string& error) {
    std::array<char, cacheMagic.size()> magic{};
    std::uint32_t formatVersion = 0;
    std::uint32_t cookingVersion = 0;
    std::uint32_t unitConventionVersion = 0;
    std::uint32_t unitsPerMeterBits = 0;
    std::string joltTag;
    StaticMeshShapeCacheKey actual;
    if (!stream.read(magic.data(), static_cast<std::streamsize>(magic.size())) ||
        magic != cacheMagic || !readValue(stream, formatVersion) ||
        !readValue(stream, cookingVersion) ||
        !readValue(stream, unitConventionVersion) ||
        !readValue(stream, unitsPerMeterBits) || !readString(stream, joltTag) ||
        !readString(stream, actual.modelIdentity) ||
        !readValue(stream, actual.geometryFingerprint)) {
        error = "truncated or malformed cache header";
        return false;
    }
    for (std::uint32_t& bit : actual.scaleBits) {
        if (!readValue(stream, bit)) {
            error = "truncated cache scale identity";
            return false;
        }
    }
    if (!readValue(stream, actual.colliderType) ||
        !readValue(stream, actual.stableHash)) {
        error = "truncated cache identity";
        return false;
    }
    if (formatVersion != physicsShapeCacheVersion ||
        cookingVersion != physicsShapeCookingVersion ||
        unitConventionVersion != physicsUnitConventionVersion ||
        unitsPerMeterBits != normalizedFloatBits(dunamisUnitsPerMeter) ||
        joltTag != joltShapeCacheCompatibilityTag) {
        error = "incompatible cache version";
        return false;
    }
    if (!(actual == expected)) {
        error = "cache header identity does not match request";
        return false;
    }
    return true;
}

std::string pathString(const std::filesystem::path& path) {
    return path.generic_string();
}

}  // namespace

bool StaticMeshShapeCacheKey::operator==(
    const StaticMeshShapeCacheKey& other) const noexcept {
    return modelIdentity == other.modelIdentity &&
           geometryFingerprint == other.geometryFingerprint &&
           scaleBits == other.scaleBits && colliderType == other.colliderType &&
           stableHash == other.stableHash;
}

StaticMeshShapeCacheKey makeStaticMeshShapeCacheKey(
    const std::string& modelIdentity,
    const std::vector<MeshInstance>& meshInstances, const glm::vec3& scale,
    std::uint8_t colliderType) {
    StaticMeshShapeCacheKey key;
    key.modelIdentity = modelIdentity;
    key.scaleBits = {normalizedFloatBits(scale.x), normalizedFloatBits(scale.y),
                     normalizedFloatBits(scale.z)};
    key.colliderType = colliderType;
    Fnv1a64 geometryHash;
    addMeshGeometry(geometryHash, meshInstances);
    key.geometryFingerprint = geometryHash.value();
    Fnv1a64 identityHash;
    addIdentity(identityHash, key);
    key.stableHash = identityHash.value();
    return key;
}

PhysicsShapeCache::PhysicsShapeCache(std::filesystem::path directory)
    : directory_(std::move(directory)) {}

std::filesystem::path PhysicsShapeCache::defaultDirectory() {
    return std::filesystem::path(DUNAMIS_SOURCE_DIR) / "game/.cache/physics";
}

std::filesystem::path PhysicsShapeCache::pathFor(
    const StaticMeshShapeCacheKey& key) const {
    std::ostringstream name;
    name << std::hex << std::setfill('0') << std::setw(16) << key.stableHash
         << ".joltshape";
    return directory_ / name.str();
}

ShapeCacheLoadResult PhysicsShapeCache::load(
    const StaticMeshShapeCacheKey& key) const {
    using Clock = std::chrono::steady_clock;
    const std::filesystem::path path = pathFor(key);
    const Clock::time_point readStart = Clock::now();
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        std::error_code error;
        if (!std::filesystem::exists(path, error) || error) {
            ShapeCacheLoadResult result;
            result.readDuration = Clock::now() - readStart;
            return result;
        }
        ShapeCacheLoadResult result;
        result.status = ShapeCacheLoadStatus::Invalid;
        result.message = "unable to open cache file";
        result.readDuration = Clock::now() - readStart;
        return result;
    }
    std::string error;
    if (!readAndValidateHeader(stream, key, error)) {
        ShapeCacheLoadResult result;
        result.status = ShapeCacheLoadStatus::Invalid;
        result.message = std::move(error);
        result.readDuration = Clock::now() - readStart;
        return result;
    }
    const Clock::time_point restoreStart = Clock::now();
    JPH::StreamInWrapper streamIn(stream);
    JPH::Shape::IDToShapeMap idToShape;
    JPH::Shape::IDToMaterialMap idToMaterial;
    JPH::Shape::ShapeResult restored = JPH::Shape::sRestoreWithChildren(
        streamIn, idToShape, idToMaterial);
    if (streamIn.IsFailed() || restored.HasError() || !restored.IsValid()) {
        ShapeCacheLoadResult result;
        result.status = ShapeCacheLoadStatus::Invalid;
        result.message = restored.HasError() ? restored.GetError().c_str()
                                             : "failed to restore Jolt shape";
        result.readDuration = restoreStart - readStart;
        result.restoreDuration = Clock::now() - restoreStart;
        return result;
    }
    JPH::ShapeRefC shape = restored.Get();
    if (shape->GetType() != JPH::EShapeType::Mesh) {
        ShapeCacheLoadResult result;
        result.status = ShapeCacheLoadStatus::Invalid;
        result.message = "restored shape is not a mesh";
        result.readDuration = restoreStart - readStart;
        result.restoreDuration = Clock::now() - restoreStart;
        return result;
    }
    ShapeCacheLoadResult result;
    result.status = ShapeCacheLoadStatus::Hit;
    result.shape = shape;
    result.readDuration = restoreStart - readStart;
    result.restoreDuration = Clock::now() - restoreStart;
    return result;
}

void PhysicsShapeCache::discard(const StaticMeshShapeCacheKey& key) const noexcept {
    std::error_code error;
    std::filesystem::remove(pathFor(key), error);
}

bool PhysicsShapeCache::save(const StaticMeshShapeCacheKey& key,
                             const JPH::Shape& shape,
                             std::string& error) const noexcept {
    const std::filesystem::path path = pathFor(key);
    const std::filesystem::path temporary = path.string() + ".tmp";
    std::error_code filesystemError;
    std::filesystem::create_directories(directory_, filesystemError);
    if (filesystemError) {
        error = "could not create cache directory " + pathString(directory_) +
                ": " + filesystemError.message();
        return false;
    }
    try {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream.is_open() || !writeHeader(stream, key)) {
            error = "could not write cache header to " + pathString(temporary);
        } else {
            JPH::StreamOutWrapper streamOut(stream);
            JPH::Shape::ShapeToIDMap shapeToId;
            JPH::Shape::MaterialToIDMap materialToId;
            shape.SaveWithChildren(streamOut, shapeToId, materialToId);
            stream.flush();
            if (!streamOut.IsFailed() && stream) {
                stream.close();
                if (stream) {
                    std::filesystem::rename(temporary, path, filesystemError);
                    if (!filesystemError) return true;
                    error = "could not promote cache file " + pathString(temporary) +
                            ": " + filesystemError.message();
                } else {
                    error = "could not close cache file " + pathString(temporary);
                }
            } else {
                error = "could not serialize cooked shape to " +
                        pathString(temporary);
            }
        }
    } catch (const std::exception& exception) {
        error = exception.what();
    } catch (...) {
        error = "unknown cache write failure";
    }
    std::filesystem::remove(temporary, filesystemError);
    return false;
}

}  // namespace physics
