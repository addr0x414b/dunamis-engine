#include "../rendering/physics_debug_renderer.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

[[noreturn]] void fail(const char* message) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}

void expect(bool condition, const char* message) {
    if (!condition) fail(message);
}

void testTriangleEdgeExpansion() {
    std::vector<std::uint32_t> output;
    expect(PhysicsDebugRenderer::makeTriangleEdgeIndices(
               {0, 1, 2}, 3, output),
           "single triangle edge conversion failed");
    expect(output == std::vector<std::uint32_t>{0, 1, 1, 2, 2, 0},
           "single triangle did not expand into its three directed edges");

    expect(PhysicsDebugRenderer::makeTriangleEdgeIndices(
               {0, 1, 2, 2, 1, 3}, 4, output),
           "two triangle edge conversion failed");
    expect(output == std::vector<std::uint32_t>{0, 1, 1, 2, 2, 0,
                                                 2, 1, 1, 3, 3, 2},
           "shared triangle edges were unexpectedly deduplicated or reordered");
}

void testTriangleEdgeValidation() {
    std::vector<std::uint32_t> output = {99};
    expect(!PhysicsDebugRenderer::makeTriangleEdgeIndices(
               {0, 1}, 3, output) && output.empty(),
           "incomplete triangle input was not rejected safely");
    expect(!PhysicsDebugRenderer::makeTriangleEdgeIndices(
               {0, 1, 4}, 4, output) && output.empty(),
           "out-of-range triangle index was not rejected safely");
    expect(PhysicsDebugRenderer::makeTriangleEdgeIndices({}, 0, output) &&
               output.empty(),
           "empty triangle input was not handled safely");

    std::vector<std::uint32_t> largeTriangles;
    constexpr std::uint32_t triangleCount = 20000;
    largeTriangles.reserve(static_cast<std::size_t>(triangleCount) * 3);
    for (std::uint32_t triangle = 0; triangle < triangleCount; ++triangle) {
        const std::uint32_t base = triangle * 3;
        largeTriangles.push_back(base);
        largeTriangles.push_back(base + 1);
        largeTriangles.push_back(base + 2);
    }
    expect(PhysicsDebugRenderer::makeTriangleEdgeIndices(
               largeTriangles, static_cast<std::size_t>(triangleCount) * 3,
               output),
           "large triangle batch conversion failed");
    expect(output.size() == static_cast<std::size_t>(triangleCount) * 6,
           "large triangle batch conversion truncated its dynamic output");
}

}  // namespace

int main() {
    testTriangleEdgeExpansion();
    testTriangleEdgeValidation();
    return EXIT_SUCCESS;
}
