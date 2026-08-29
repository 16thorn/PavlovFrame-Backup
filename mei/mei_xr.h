// mei_xr.h — OpenXR/Vulkan injection entry points, called from pavchams.cpp.
#pragma once

// Install the xrGetInstanceProcAddr hook. Safe to call once, early (from the mod's boot thread).
// Returns true if the OpenXR loader was found and the hook armed.
bool mei_xr_install();

// True once the Vulkan/OpenXR backend has fully initialised (session + swapchain + ImGui live).
bool mei_xr_ready();

// True once our injected controller actions are reading real state (trigger/stick). While false,
// pavchams keeps the up-gesture fallback for opening the menu.
bool mei_xr_actions_live();
