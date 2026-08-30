// mei_audio.h — tiny OpenSL ES SFX player for the Pavlov-style menu UI sounds.
// Loads the game's own UI .wav assets (pushed to files/) and plays them on menu interactions.
#pragma once

enum MeiSnd {
    MSND_SELECT = 0,   // button click / confirm      (PVR_SFX_UI_MainMenu_Select)
    MSND_HOVER,        // hover / tab change           (Submenu_Select)
    MSND_DESELECT,     // back / toggle off            (Submenu_Deselect)
    MSND_OPEN,         // menu opens                   (Window_SlideOpen)
    MSND_CLOSE,        // menu closes                  (Window_SlideClose)
    MSND_GRAB,         // slider/scroll grab           (ScrollBar_Grab)
    MSND_UNGRAB,       // slider/scroll release        (ScrollBar_Ungrab)
    MSND_COUNT
};

void mei_audio_init();          // idempotent; safe if OpenSL or files are missing (silently no-ops)
void mei_audio_play(int snd);   // fire-and-forget; callable from the render thread
void mei_audio_shutdown();
