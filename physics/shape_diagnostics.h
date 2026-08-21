#ifndef SHAPE_DIAGNOSTICS_H
#define SHAPE_DIAGNOSTICS_H

#include <cstddef>

namespace physics {

struct ShapeDiagnostics {
    enum class Representation {
        TriangleMesh,
        ConvexHull,
        AnalyticSphere,
        AnalyticCapsule,
    };

    Representation representation = Representation::TriangleMesh;
    std::size_t inputVertices = 0;
    std::size_t inputTriangles = 0;
    std::size_t inputPoints = 0;
    std::size_t cookedHullVertices = 0;
    std::size_t joltTriangles = 0;
    std::size_t joltBytes = 0;
    float radius = 0.0f;
    float height = 0.0f;
};

}  // namespace physics

#endif
