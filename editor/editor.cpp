#include "editor.h"

void Editor::processEvent(SDL_Event* event) {
    ImGui_ImplSDL2_ProcessEvent(event);

    if (event->type == SDL_MOUSEMOTION) {
        processMouseMovement(event->motion.xrel, event->motion.yrel);
    }
}

void Editor::processInput() {
    if (scene->simulating) {
        return;
    }

    const Uint8* keystate = SDL_GetKeyboardState(NULL);

    int xPos;
    int yPos;

    if (SDL_GetMouseState(&xPos, &yPos) & SDL_BUTTON(3)) {
        SDL_ShowCursor(SDL_DISABLE);
        SDL_SetRelativeMouseMode(SDL_TRUE);
        movingMouse = true;

        if (keystate[SDL_SCANCODE_W]) {
            editorCamera.position += cameraSpeed * editorCamera.front;
        }

        if (keystate[SDL_SCANCODE_S]) {
            editorCamera.position -= cameraSpeed * editorCamera.front;
        }

        if (keystate[SDL_SCANCODE_A]) {
            editorCamera.position -= glm::normalize(glm::cross(
                                         editorCamera.front, editorCamera.up)) *
                                     cameraSpeed;
        }

        if (keystate[SDL_SCANCODE_D]) {
            editorCamera.position += glm::normalize(glm::cross(
                                         editorCamera.front, editorCamera.up)) *
                                     cameraSpeed;
        }

        if (keystate[SDL_SCANCODE_SPACE]) {
            editorCamera.position += cameraSpeed * editorCamera.up;
        }

        if (keystate[SDL_SCANCODE_LCTRL]) {
            editorCamera.position -= cameraSpeed * editorCamera.up;
        }

        if (keystate[SDL_SCANCODE_LSHIFT]) {
            cameraSpeed = 0.2f;
        } else {
            cameraSpeed = 0.05f;
        }
    } else {
        movingMouse = false;
        SDL_SetRelativeMouseMode(SDL_FALSE);
    }
}
void Editor::setWindow(SDL_Window* window) { this->window = window; }
void Editor::setScene(Scene* scene) { this->scene = scene; }

void Editor::processMouseMovement(double xPos, double yPos) {
    if (movingMouse) {
        if (firstMouse) {
            lastX = xPos;
            lastY = yPos;
            firstMouse = false;
        }

        float xOffset = xPos;
        float yOffset = -yPos;
        lastX = xPos;
        lastY = yPos;

        float sensitivity = 0.1f;
        xOffset *= sensitivity;
        yOffset *= sensitivity;

        yaw += xOffset;
        pitch += yOffset;

        if (pitch > 89.0f) {
            pitch = 89.0f;
        }
        if (pitch < -89.0f) {
            pitch = -89.0f;
        }

        glm::vec3 direction;
        direction.x = cos(glm::radians(yaw)) * cos(glm::radians(pitch));
        direction.y = sin(glm::radians(pitch));
        direction.z = sin(glm::radians(yaw)) * cos(glm::radians(pitch));
        editorCamera.front = glm::normalize(direction);
        SDL_WarpMouseInWindow(window, 960, 540);
    }
}

void Editor::showMenuBar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            // ShowExampleMenuFile();
            if (ImGui::MenuItem("New Project")) {
            }

            if (ImGui::MenuItem("New Scene")) {
            }

            if (ImGui::MenuItem("Load Project")) {
            }

            if (ImGui::BeginMenu("Load Scene")) {
                if (ImGui::MenuItem("Main Menu")) {
                    Debugger::print("Loading Main Menu Scene");
                }
                ImGui::MenuItem("Test Scene");
                ImGui::MenuItem("Level 01");
                ImGui::MenuItem("Level 02");
                ImGui::EndMenu();
            }

            if (ImGui::MenuItem("Quit")) {
                quit = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "CTRL+Z")) {
            }
            if (ImGui::MenuItem("Redo", "CTRL+Y", false, false)) {
            }  // Disabled item
            ImGui::Separator();
            if (ImGui::MenuItem("Cut", "CTRL+X")) {
            }
            if (ImGui::MenuItem("Copy", "CTRL+C")) {
            }
            if (ImGui::MenuItem("Paste", "CTRL+V")) {
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

void Editor::showSideBar() {
    int w;
    int h;

    SDL_GetWindowSize(window, &w, &h);
    int offset = 19;

    ImGui::SetNextWindowSize(ImVec2(250, h - offset));
    ImGui::SetNextWindowPos(ImVec2(0, offset));

    ImGui::Begin("My First Tool", NULL,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove);

    if (ImGui::Button("Play")) {
        scene->simulating = true;
    }
    ImGui::SameLine();

    if (ImGui::Button("Stop")) {
        scene->simulating = false;
    }

    // Display contents in a scrolling region
    ImGui::SeparatorText("Scene Objects");
    ImGui::BeginChild("Scrolling");
    // for (int n = 0; n < 5000; n++) ImGui::Text("%04d: Some text", n);

    /*for (const auto& camera : scene->cameras) {
        ImGui::Text(camera->name);
    }
    for (const auto& gameObject : scene->gameObjects) {
        ImGui::Text(gameObject->name);
    }*/

    static int selected = -1;

    for (int i = 0; i < scene->gameObjects.size(); i++) {
        char buf[32];
        sprintf(buf, scene->gameObjects[i]->name);
        if (ImGui::Selectable(buf, selected == i)) {
            selected = i;
            selectedObject = scene->gameObjects[i];
        }
    }

    ImGui::EndChild();
    ImGui::End();
}

void Editor::showEditorBar() {
    int w;
    int h;

    SDL_GetWindowSize(window, &w, &h);
    int offset = 19;

    ImGui::SetNextWindowSize(ImVec2(250, h - offset));
    ImGui::SetNextWindowPos(ImVec2(w - 250, offset));

    ImGui::Begin("Tool Bro", NULL,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove);

    ImGui::SeparatorText("Inspector");
    
    if (selectedObject != nullptr) {
        ImGui::Text(selectedObject->name);
        ImGui::DragFloat3("Position", &selectedObject->position[0], 0.1f);
        ImGui::DragFloat3("Scale", &selectedObject->scale[0], 0.1f);
        ImGui::DragFloat3("Rotation", &selectedObject->rotation[0], 0.1f);
        if (ImGui::Button("Play")) {
            scene->simulating = true;
        }
        ImGui::SameLine();

        if (ImGui::Button("Stop")) {
            scene->simulating = false;
        }

        // ImGui::DragFloat3("Pos 1", &editorCamera.position[0], 0.1f);
        // ImGui::DragFloat3("Pos 2", &scene->sceneCamera.position[0], 0.1f);
        ImGui::DragFloat3("Pos 1", &scene->gameObjects[0]->position[0], 0.1f);
        ImGui::DragFloat3("Pos 2", &scene->gameObjects[1]->position[0], 0.1f);
        ImGui::DragFloat3("Pos 3", &scene->gameObjects[2]->position[0], 0.1f);

        // Display contents in a scrolling region
        ImGui::SeparatorText("Scene Objects");
        ImGui::BeginChild("Scrolling");
        // for (int n = 0; n < 5000; n++) ImGui::Text("%04d: Some text", n);

        for (const auto& camera : scene->cameras) {
            ImGui::Text(camera->name);
        }
        for (const auto& gameObject : scene->gameObjects) {
            ImGui::Text(gameObject->name);
        }

        ImGui::EndChild();
    }
    ImGui::End();
}