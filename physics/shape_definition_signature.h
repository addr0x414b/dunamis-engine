#ifndef SHAPE_DEFINITION_SIGNATURE_H
#define SHAPE_DEFINITION_SIGNATURE_H

#include <array>
#include <cstdint>
#include <string>

class Character;
class GameObject;

namespace physics {

struct ShapeDefinitionSignature {
    enum class Type : std::uint8_t {
        Mesh,
        ConvexHull,
        Sphere,
        CharacterCapsule,
    };

    Type type = Type::Mesh;
    std::string modelIdentity;
    std::array<std::uint32_t, 3> scaleBits{};
    std::uint32_t radiusBits = 0;
    std::uint32_t heightBits = 0;

    [[nodiscard]] bool operator==(
        const ShapeDefinitionSignature& other) const noexcept {
        return type == other.type && modelIdentity == other.modelIdentity &&
               scaleBits == other.scaleBits && radiusBits == other.radiusBits &&
               heightBits == other.heightBits;
    }

    [[nodiscard]] bool operator!=(
        const ShapeDefinitionSignature& other) const noexcept {
        return !(*this == other);
    }
};

[[nodiscard]] ShapeDefinitionSignature makeGameObjectShapeDefinitionSignature(
    const GameObject& object);
[[nodiscard]] ShapeDefinitionSignature makeCharacterShapeDefinitionSignature(
    const Character& character);

}  // namespace physics

#endif
