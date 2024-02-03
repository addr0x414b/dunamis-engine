#include "editor.h"

void Editor::processEvent(SDL_Event* event) {
    ImGui_ImplSDL2_ProcessEvent(event);

     if (event->type == SDL_MOUSEMOTION) {
        processMouseMovement(event->motion.xrel, event->motion.yrel);
    }
}

void Editor::processInput() {
    const Uint8* keystate = SDL_GetKeyboardState(NULL);

    const float cameraSpeed = 0.05f;

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
        //processMouseMovement(xPos, yPos);
    } else {
        if (movingMouse) {
            //SDL_WarpMouseInWindow(window, 1000, 300);
        }
        movingMouse = false;
        SDL_SetRelativeMouseMode(SDL_FALSE);
        //SDL_WarpMouseInWindow(window, 960, 540);
    }
}
void Editor::setWindow(SDL_Window* window) { this->window = window; }

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

void Editor::ShowExampleMenuFile() {
    ImGui::MenuItem("(demo menu)", NULL, false, false);
    if (ImGui::MenuItem("New")) {
    }
    if (ImGui::MenuItem("Open", "Ctrl+O")) {
    }
    if (ImGui::BeginMenu("Open Recent")) {
        ImGui::MenuItem("fish_hat.c");
        ImGui::MenuItem("fish_hat.inl");
        ImGui::MenuItem("fish_hat.h");
        if (ImGui::BeginMenu("More..")) {
            ImGui::MenuItem("Hello");
            ImGui::MenuItem("Sailor");
            if (ImGui::BeginMenu("Recurse..")) {
                ShowExampleMenuFile();
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Save", "Ctrl+S")) {
    }
    if (ImGui::MenuItem("Save As..")) {
    }

    ImGui::Separator();
    if (ImGui::BeginMenu("Options")) {
        static bool enabled = true;
        ImGui::MenuItem("Enabled", "", &enabled);
        ImGui::BeginChild("child", ImVec2(0, 60), ImGuiChildFlags_Border);
        for (int i = 0; i < 10; i++) ImGui::Text("Scrolling Text %d", i);
        ImGui::EndChild();
        static float f = 0.5f;
        static int n = 0;
        ImGui::SliderFloat("Value", &f, 0.0f, 1.0f);
        ImGui::InputFloat("Input", &f, 0.1f);
        ImGui::Combo("Combo", &n, "Yes\0No\0Maybe\0\0");
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Colors")) {
        float sz = ImGui::GetTextLineHeight();
        for (int i = 0; i < ImGuiCol_COUNT; i++) {
            const char* name = ImGui::GetStyleColorName((ImGuiCol)i);
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(
                p, ImVec2(p.x + sz, p.y + sz), ImGui::GetColorU32((ImGuiCol)i));
            ImGui::Dummy(ImVec2(sz, sz));
            ImGui::SameLine();
            ImGui::MenuItem(name);
        }
        ImGui::EndMenu();
    }

    // Here we demonstrate appending again to the "Options" menu (which we
    // already created above) Of course in this demo it is a little bit silly
    // that this function calls BeginMenu("Options") twice. In a real code-base
    // using it would make senses to use this feature from very different code
    // locations.
    if (ImGui::BeginMenu("Options"))  // <-- Append!
    {
        static bool b = true;
        ImGui::Checkbox("SomeOption", &b);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Disabled", false))  // Disabled
    {
        IM_ASSERT(0);
    }
    if (ImGui::MenuItem("Checked", NULL, true)) {
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Quit", "Alt+F4")) {
        quit = true;
    }
}