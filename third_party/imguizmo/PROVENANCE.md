# ImGuizmo provenance

- Upstream project: ImGuizmo
- Upstream repository: https://github.com/CedricGuillemet/ImGuizmo
- Vendored commit: `5ab7676402ace03cdf930b2d972f59c7d03c6fa8` (upstream v1.92.5 WIP)
- License: MIT; see `LICENSE`.

Only `ImGuizmo.h`, `ImGuizmo.cpp`, and the upstream license are vendored.

`ImGuizmo.cpp` has two compatibility corrections:

1. Its `Manipulate` draw-list clip push intersects an existing clip rect. This
   preserves ImGuizmo's normal full-viewport clip while allowing Dunamis to
   enforce the central dock-area clip required by its full-swapchain scene
   renderer.
2. Its hover helper safely handles Dear ImGui foreground draw lists, which are
   viewport-owned rather than window-owned. The fallback accepts only an
   unobstructed mouse position within ImGuizmo's configured rectangle.
