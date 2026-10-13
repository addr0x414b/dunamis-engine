# dunamis-engine
Dunamis game engine

## Importing hierarchical models from C++

During authored scene construction, use `model_import::importHierarchy` to
create ordinary selectable GameObjects from the source node hierarchy:

```cpp
#include "../scene/model_importer.h"  // from game/level_1.cpp

#include <stdexcept>

void Level1::buildDefaults() {
    model_import::Options options;
    options.rootName = "Environment";
    options.generateStaticColliders = true;

    model_import::ImportResult imported;
    const Result result = model_import::importHierarchy(
        *this, "game/assets/models/environment.glb", options, imported);
    if (!result) {
        throw std::runtime_error("Model import failed: " + result.error());
    }

    GameObject* importedRoot = imported.root;  // Owned by this Scene.
    (void)importedRoot;
}
```

Importing is supported while the target Scene is inactive, including from
`buildDefaults()`. Static collision creates one mesh collider for each
mesh-bearing source node and no collider for empty organizational nodes. The
import fails with a diagnostic if a mesh-bearing source node also has
mesh-bearing descendants, because Jolt does not permit physics bodies beneath
other physics bodies. Skeletal animation and morph targets are not supported by
the static model importer.

At startup, an existing saved scene file is loaded before `buildDefaults()` is
called. After changing C++ defaults, remove or load a different authored scene
file if you need to see the updated defaults; imported authored hierarchies
themselves are saved as individual scene objects and are not regenerated from
the source asset on reload.

The importer uses the scene-graph structure exposed by the configured Assimp
format importer. Formats that contain named groups or nodes (including FBX,
glTF/GLB, and OBJ object groups) produce separate authored objects; a source
file containing only one undivided mesh remains one renderable object under
the import root.
