#ifndef EDITOR_H
#define EDITOR_H

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_vulkan.h>
#include <SDL2/SDL.h>

#include "../core/debugger/debugger.h"
#include "../scene/scene.h"
#include "../scene/3d/camera.h"

class Editor {
   public:
    void processEvent(SDL_Event* event);
    void processInput();
    void showMenuBar();
    void showSideBar();
    void showEditorBar();
    void setWindow(SDL_Window* window);
    void setScene(Scene* scene);
    bool quit = false;
    Camera editorCamera;

   private:
    void ShowExampleMenuFile();
    Scene* scene = nullptr;
    void processMouseMovement(double xPos, double yPos);
    double lastX = 960.0f;
    double lastY = 540.0f;
    double yaw = -90.0f;
    double pitch = 0.0f;
    bool firstMouse = true;
    bool movingMouse = false;
    SDL_Window *window;
    float cameraSpeed = 0.05f;
    GameObject* selectedObject = nullptr;
};

#endif  // EDITOR_H