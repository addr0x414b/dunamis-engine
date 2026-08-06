#include "scene/loading_cache_key.h"

#include <iostream>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main() {
    const std::string first = model_loading::normalizedExternalSourceIdentity(
        "models/../textures/albedo.png");
    const std::string equivalent =
        model_loading::normalizedExternalSourceIdentity("textures/albedo.png");
    const model_loading::TextureCacheKey baseKey{
        first, model_loading::TextureUsage::BaseColor};
    const model_loading::TextureCacheKey equivalentKey{
        equivalent, model_loading::TextureUsage::BaseColor};
    const model_loading::TextureCacheKey normalKey{
        equivalent, model_loading::TextureUsage::Normal};
    const model_loading::TextureCacheKey embeddedZero{
        model_loading::embeddedSourceIdentity("model.glb", 0),
        model_loading::TextureUsage::BaseColor};
    const model_loading::TextureCacheKey embeddedOne{
        model_loading::embeddedSourceIdentity("model.glb", 1),
        model_loading::TextureUsage::BaseColor};

    bool passed = true;
    passed &= expect(baseKey == equivalentKey,
                     "Equivalent external paths produced different keys");
    passed &= expect(
        model_loading::TextureCacheKeyHash{}(baseKey) ==
            model_loading::TextureCacheKeyHash{}(equivalentKey),
        "Equivalent external paths produced different hashes");
    passed &= expect(!(equivalentKey == normalKey),
                     "Texture usage was omitted from the cache key");
    passed &= expect(!(embeddedZero == embeddedOne),
                     "Embedded texture indices were omitted from the key");
    passed &= expect(model_loading::fallbackSourceIdentity() ==
                         model_loading::fallbackSourceIdentity(),
                     "Fallback source identity was not stable");
    return passed ? 0 : 1;
}
