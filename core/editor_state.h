#ifndef EDITOR_STATE_H
#define EDITOR_STATE_H

enum class SceneRunState {
    Editing,
    Playing,
};

enum class EditorCommand {
    None,
    Play,
    Stop,
};

#endif
