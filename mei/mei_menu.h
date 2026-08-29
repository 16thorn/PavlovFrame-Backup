// mei_menu.h — the Dear ImGui UI for "mei mei [private]".
#pragma once

// Apply the mei theme (colors/rounding/font scale). Call once after ImGui::CreateContext.
void mei_style();

// Build one frame of the menu window. Call between ImGui::NewFrame() and ImGui::Render().
// Reads/writes g_mei; on any change, flags a debounced save. `panel_w/panel_h` are the
// backbuffer pixel size so the window can fill the panel.
void mei_menu_frame(int panel_w, int panel_h);

// Debounced persistence tick — call once per frame (writes mei.cfg ~1s after the last edit).
void mei_menu_flush();
