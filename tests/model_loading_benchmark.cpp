#include "scene/game_object.h"

#include <chrono>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Expected a model path\n";
        return 2;
    }

    GameObject object;
    object.modelPath = argv[1];
    const auto start = std::chrono::steady_clock::now();
    const Result result = object.loadModel();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    if (!result) {
        std::cerr << result.error() << '\n';
        return 1;
    }
    std::cout << "benchmark loadModel total="
              << std::chrono::duration<double, std::milli>(elapsed).count()
              << "ms\n";
    return 0;
}
