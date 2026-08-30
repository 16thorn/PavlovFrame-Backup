// mei_xr.cpp — inject the "mei mei [private]" ImGui panel into Pavlov's VR frame as an OpenXR
// quad composition layer, rendered with a self-owned Vulkan ImGui backend.
//
// STRATEGY
//   1. Inline-hook xrGetInstanceProcAddr (the single dispatch point every OpenXR call flows through).
//   2. Wrap xrCreateSession   -> capture the app's VkInstance/PhysicalDevice/Device/Queue from the
//                                XrGraphicsBindingVulkan(2)KHR in the create-info chain, and create
//                                our own VIEW reference space.
//   3. Wrap xrEndFrame        -> render ImGui into our own XrSwapchain (Vulkan), then append a
//                                XrCompositionLayerQuad (in VIEW space, in front of the face) to the
//                                app's layer list before forwarding to the real xrEndFrame.
//
//   The panel is head-locked in VIEW space, so it needs no world/tracking math to render. Input
//   (mei_input.cpp) is fed the head + controller poses by pavchams and expresses the ray in the
//   SAME head-local frame, so cursor and quad stay consistent with zero calibration.
//
//   We use ONLY the app's existing Vulkan device/queue — we allocate nothing on the GPU beyond our
//   swapchain, one render pass, per-image framebuffers, a descriptor pool and a command buffer.
//
// NOTE (on-device): everything here compiles against the standard Khronos OpenXR + NDK Vulkan
// headers. It cannot be exercised off-headset; the ON-DEVICE.md checklist tracks first-pixel bring-up.

#define XR_USE_GRAPHICS_API_VULKAN 1
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "mei_menu.h"
#include "mei_input.h"
#include "mei_settings.h"
#include "mei_xr.h"
#include "mei_esp.h"

#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <time.h>
#include <math.h>
#include <android/log.h>

#define XLOG(...) __android_log_print(ANDROID_LOG_INFO, "MEI-XR", __VA_ARGS__)

// ============================================================================
//  captured handles / state
// ============================================================================
static PFN_xrGetInstanceProcAddr real_xrGIPA = nullptr;

static XrInstance g_inst    = XR_NULL_HANDLE;
static XrSession  g_session = XR_NULL_HANDLE;
static XrSpace    g_viewspace = XR_NULL_HANDLE;

static VkInstance       g_vkInst = VK_NULL_HANDLE;
static VkPhysicalDevice g_vkPhys = VK_NULL_HANDLE;
static VkDevice         g_vkDev  = VK_NULL_HANDLE;
static uint32_t         g_vkQF   = 0;
static VkQueue          g_vkQueue = VK_NULL_HANDLE;

static XrSwapchain      g_swap = XR_NULL_HANDLE;
static uint32_t         g_swapW = 1280, g_swapH = 820;
static int64_t          g_swapFmt = 0;
static std::vector<VkImage>       g_images;
static std::vector<VkImageView>   g_views;
static std::vector<VkFramebuffer> g_fbs;
static VkRenderPass     g_rp   = VK_NULL_HANDLE;
static VkDescriptorPool g_dpool= VK_NULL_HANDLE;
static VkCommandPool    g_cpool= VK_NULL_HANDLE;
// per-swapchain-image rings (frames-in-flight): index by the XR-acquired image idx so cmd buffer,
// fence, and framebuffer stay aligned. This is what lets us submit WITHOUT a per-frame CPU stall.
static std::vector<VkCommandBuffer> g_cmds;
static std::vector<VkFence>         g_fences;

static bool g_gfx_captured = false;   // vk handles + session valid
static bool g_inited       = false;   // full backend up
static bool g_failed       = false;   // gave up (log once)
static double g_last_time  = 0.0;
static ImGuiContext* g_menuCtx = nullptr;   // the menu's ImGui context

// ---- ESP overlay: a SECOND view-locked quad with its own swapchain + ImGui context ----
static const uint32_t ESP_W = 1280, ESP_H = 800;   // aspect must match MEI_ESP_ASPECT (1.6)
static XrSwapchain    g_espSwap = XR_NULL_HANDLE;
static std::vector<VkImage>       g_espImages;
static std::vector<VkImageView>   g_espViews;
static std::vector<VkFramebuffer> g_espFbs;
static VkDescriptorPool g_espDpool = VK_NULL_HANDLE;
static std::vector<VkCommandBuffer> g_espCmds;
static std::vector<VkFence>         g_espFences;
static ImGuiContext*  g_espCtx = nullptr;
static bool           g_espInited = false, g_espFailed = false;
static uint32_t       g_espSlot = 0;

// cached per-frame OpenXR PFNs (resolved once; no loader string-walk on the compositor thread)
static PFN_xrAcquireSwapchainImage  pfn_xrAcquire = nullptr;
static PFN_xrWaitSwapchainImage     pfn_xrWait    = nullptr;
static PFN_xrReleaseSwapchainImage  pfn_xrRelease = nullptr;
static PFN_xrGetActionStateBoolean  pfn_xrGetBool = nullptr;
static PFN_xrLocateSpace            pfn_xrLocate  = nullptr;

// ---- our injected input action set (real controller buttons) ----
static XrActionSet g_actionSet = XR_NULL_HANDLE;
static XrAction    g_actClick  = XR_NULL_HANDLE;   // right trigger   -> click
static XrAction    g_actToggle = XR_NULL_HANDLE;   // left stick press-> open/close
static XrAction    g_actAim    = XR_NULL_HANDLE;   // right aim pose  -> cursor ray
static XrSpace     g_aimSpace  = XR_NULL_HANDLE;   // action space for the aim pose
static XrSpace     g_localspace = XR_NULL_HANDLE;  // world-anchored (LOCAL) reference space
static XrPosef     g_panelPose  = { {0,0,0,1}, {0,0,0} };  // panel pose in LOCAL space (world-locked)
static bool        g_needPlace  = false;           // (re)place the panel in front of the head on next frame

// ---- tiny quaternion/vector math for the world-anchored panel ----
struct V3f { float x, y, z; };
static V3f qrot(const XrQuaternionf& q, V3f v) {   // rotate v by quaternion q
    float tx = 2.f*(q.y*v.z - q.z*v.y), ty = 2.f*(q.z*v.x - q.x*v.z), tz = 2.f*(q.x*v.y - q.y*v.x);
    return { v.x + q.w*tx + (q.y*tz - q.z*ty),
             v.y + q.w*ty + (q.z*tx - q.x*tz),
             v.z + q.w*tz + (q.x*ty - q.y*tx) };
}
static XrQuaternionf qconj(const XrQuaternionf& q) { return { -q.x, -q.y, -q.z, q.w }; }

// ---- vector helpers + look-at (for grab-to-move panel) ----
static V3f   xv(const XrVector3f& v) { return { v.x, v.y, v.z }; }
static V3f   v3sub(V3f a, V3f b)   { return { a.x-b.x, a.y-b.y, a.z-b.z }; }
static float v3len(V3f a)          { return sqrtf(a.x*a.x + a.y*a.y + a.z*a.z); }
static V3f   v3norm(V3f a)         { float l=v3len(a); return l<1e-6f ? V3f{0,0,-1} : V3f{a.x/l,a.y/l,a.z/l}; }
static V3f   v3cross(V3f a, V3f b) { return { a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x }; }
// quaternion whose local +Z (panel normal, faces the viewer) points from `from` toward `to`, +Y ~ world up
static XrQuaternionf quat_face(V3f from, V3f to) {
    V3f z = v3norm(v3sub(to, from));                 // panel +Z toward the head
    V3f up = { 0, 1, 0 };
    V3f x = v3cross(up, z); float xl = v3len(x);
    if (xl < 1e-4f) { up = { 0, 0, 1 }; x = v3cross(up, z); xl = v3len(x); }
    x = { x.x/xl, x.y/xl, x.z/xl };
    V3f y = v3cross(z, x);
    float m00=x.x,m01=y.x,m02=z.x, m10=x.y,m11=y.y,m12=z.y, m20=x.z,m21=y.z,m22=z.z;
    float tr = m00+m11+m22; XrQuaternionf q;
    if (tr > 0.f)                { float s=sqrtf(tr+1.f)*2.f; q.w=0.25f*s; q.x=(m21-m12)/s; q.y=(m02-m20)/s; q.z=(m10-m01)/s; }
    else if (m00>m11 && m00>m22) { float s=sqrtf(1.f+m00-m11-m22)*2.f; q.w=(m21-m12)/s; q.x=0.25f*s; q.y=(m01+m10)/s; q.z=(m02+m20)/s; }
    else if (m11>m22)            { float s=sqrtf(1.f+m11-m00-m22)*2.f; q.w=(m02-m20)/s; q.x=(m01+m10)/s; q.y=0.25f*s; q.z=(m12+m21)/s; }
    else                         { float s=sqrtf(1.f+m22-m00-m11)*2.f; q.w=(m10-m01)/s; q.x=(m02+m20)/s; q.y=(m12+m21)/s; q.z=0.25f*s; }
    return q;
}

// grab-to-move state (shared: poll_actions gates clicks while grabbing; update_aim_ray feeds the ray)
static bool  g_trigger_raw = false;          // latest hw trigger, ungated
static V3f   g_ctrlPosL    = {0,0,0};        // right controller aim origin in LOCAL space
static V3f   g_ctrlFwdL    = {0,0,-1};       // right controller aim forward in LOCAL space
static bool  g_ctrlValidL  = false;
static bool  g_grabbing    = false;          // panel currently pinned to the controller ray
static float g_grab_dist   = 0.6f;           // controller->panel distance captured at grab start

static bool        g_actions_built = false;        // set created + bindings suggested
static bool        g_actions_live  = false;        // getState returned active at least once
bool mei_xr_actions_live() { return g_actions_live; }

// resolve a real OpenXR function by name through the captured instance
template <class T> static bool xr_get(const char* name, T* out) {
    if (!real_xrGIPA || g_inst == XR_NULL_HANDLE) return false;
    PFN_xrVoidFunction f = nullptr;
    if (XR_FAILED(real_xrGIPA(g_inst, name, &f)) || !f) return false;
    *out = reinterpret_cast<T>(f);
    return true;
}

// ============================================================================
//  generic arm64 inline hook (LDR x17,#8 ; BR x17 ; .quad target) — same shape as pavchams
// ============================================================================
static bool inline_hook_abs(void* target, void* replacement, void** out_orig) {
    // build a trampoline that runs the 4 stolen instrs then jumps to target+16.
    uint32_t* src = (uint32_t*)target;
    // refuse to relocate PC-relative first 4 instrs (keep it conservative)
    for (int i = 0; i < 4; i++) {
        uint32_t in = src[i];
        if ((in & 0x9f000000u) == 0x90000000u ||  // ADRP
            (in & 0x9f000000u) == 0x10000000u ||  // ADR
            (in & 0x7c000000u) == 0x14000000u ||  // B/BL
            (in & 0xff000010u) == 0x54000000u ||  // B.cond
            (in & 0x7e000000u) == 0x34000000u ||  // CBZ/CBNZ
            (in & 0x7e000000u) == 0x36000000u ||  // TBZ/TBNZ
            (in & 0x3b000000u) == 0x18000000u) {  // LDR literal
            XLOG("hook: prologue +%d non-relocatable (0x%08x)", i*4, in);
            return false;
        }
    }
    uint8_t* t = (uint8_t*)mmap(nullptr, 64, PROT_READ|PROT_WRITE|PROT_EXEC,
                               MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (t == MAP_FAILED) return false;
    memcpy(t, src, 16);
    uint32_t* w = (uint32_t*)(t + 16);
    w[0] = 0x58000051; w[1] = 0xd61f0220;           // LDR x17,#8 ; BR x17
    *(uint64_t*)(t + 24) = (uint64_t)target + 16;
    __builtin___clear_cache((char*)t, (char*)t + 64);
    *out_orig = t;

    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t page = (uintptr_t)target & ~(uintptr_t)(ps - 1);
    if (mprotect((void*)page, ps * 2, PROT_READ|PROT_WRITE|PROT_EXEC) != 0) return false;
    uint32_t* d = (uint32_t*)target;
    d[0] = 0x58000051; d[1] = 0xd61f0220;
    *(uint64_t*)((uint8_t*)target + 8) = (uint64_t)replacement;
    __builtin___clear_cache((char*)target, (char*)target + 16);
    return true;
}

// ============================================================================
//  Vulkan helpers
// ============================================================================
static void check(VkResult r, const char* what) { if (r != VK_SUCCESS) XLOG("VK %s = %d", what, r); }

static int64_t pick_color_format() {
    // enumerate what the runtime supports; prefer a plain 8-bit RGBA/BGRA
    PFN_xrEnumerateSwapchainFormats fn;
    if (!xr_get("xrEnumerateSwapchainFormats", &fn)) return VK_FORMAT_R8G8B8A8_UNORM;
    uint32_t n = 0; fn(g_session, 0, &n, nullptr);
    std::vector<int64_t> fmts(n); fn(g_session, n, &n, fmts.data());
    // Prefer UNORM: the runtime then does no linear<->sRGB conversion, so ImGui's already-sRGB
    // vertex colors reach the compositor unmodified (no double-gamma / washed-out edges).
    const int64_t pref[] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
                             VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB };
    for (int64_t p : pref) for (int64_t f : fmts) if (f == p) return p;
    return n ? fmts[0] : VK_FORMAT_R8G8B8A8_UNORM;
}

static bool make_swapchain() {
    PFN_xrCreateSwapchain xrCreateSwapchain;
    PFN_xrEnumerateSwapchainImages xrEnumSwapImg;
    if (!xr_get("xrCreateSwapchain", &xrCreateSwapchain) ||
        !xr_get("xrEnumerateSwapchainImages", &xrEnumSwapImg)) return false;

    g_swapFmt = pick_color_format();
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags   = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format       = g_swapFmt;
    ci.sampleCount  = 1;
    ci.width        = g_swapW;  ci.height = g_swapH;
    ci.faceCount    = 1;  ci.arraySize = 1;  ci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(g_session, &ci, &g_swap))) { XLOG("xrCreateSwapchain failed"); return false; }

    uint32_t n = 0; xrEnumSwapImg(g_swap, 0, &n, nullptr);
    std::vector<XrSwapchainImageVulkanKHR> imgs(n, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR });
    xrEnumSwapImg(g_swap, n, &n, (XrSwapchainImageBaseHeader*)imgs.data());
    g_images.clear();
    for (auto& i : imgs) g_images.push_back(i.image);
    // cache the hot per-frame swapchain PFNs once (no loader string-walk on the render thread)
    xr_get("xrAcquireSwapchainImage", &pfn_xrAcquire);
    xr_get("xrWaitSwapchainImage",    &pfn_xrWait);
    xr_get("xrReleaseSwapchainImage", &pfn_xrRelease);
    XLOG("swapchain %ux%u fmt=%lld images=%u", g_swapW, g_swapH, (long long)g_swapFmt, n);
    return n > 0 && pfn_xrAcquire && pfn_xrWait && pfn_xrRelease;
}

static bool make_renderpass() {
    VkAttachmentDescription a{};
    a.format = (VkFormat)g_swapFmt; a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    a.finalLayout   = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;   // OpenXR composites from this
    VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub{}; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0; dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1; rp.pAttachments = &a;
    rp.subpassCount = 1; rp.pSubpasses = &sub;
    rp.dependencyCount = 1; rp.pDependencies = &dep;
    check(vkCreateRenderPass(g_vkDev, &rp, nullptr, &g_rp), "CreateRenderPass");
    return g_rp != VK_NULL_HANDLE;
}

static bool make_framebuffers() {
    for (VkImage img : g_images) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = (VkFormat)g_swapFmt;
        vi.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                          VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkImageView view; check(vkCreateImageView(g_vkDev, &vi, nullptr, &view), "CreateImageView");
        g_views.push_back(view);
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = g_rp; fi.attachmentCount = 1; fi.pAttachments = &view;
        fi.width = g_swapW; fi.height = g_swapH; fi.layers = 1;
        VkFramebuffer fb; check(vkCreateFramebuffer(g_vkDev, &fi, nullptr, &fb), "CreateFramebuffer");
        g_fbs.push_back(fb);
    }
    return !g_fbs.empty();
}

static bool make_vk_objects() {
    VkDescriptorPoolSize sz{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8 };
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dp.maxSets = 8; dp.poolSizeCount = 1; dp.pPoolSizes = &sz;
    check(vkCreateDescriptorPool(g_vkDev, &dp, nullptr, &g_dpool), "DescriptorPool");

    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; cp.queueFamilyIndex = g_vkQF;
    check(vkCreateCommandPool(g_vkDev, &cp, nullptr, &g_cpool), "CommandPool");

    // one command buffer + one fence PER swapchain image -> true frames-in-flight. Submitting into
    // g_cmds[idx]/g_fences[idx] (idx = the XR-acquired image) means our GPU work overlaps UE's frame
    // and we never CPU-block on fresh work — the reuse wait on an already-signaled fence is ~free.
    uint32_t N = (uint32_t)g_images.size();
    if (N == 0) return false;
    g_cmds.resize(N); g_fences.resize(N);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = g_cpool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = N;
    check(vkAllocateCommandBuffers(g_vkDev, &ai, g_cmds.data()), "AllocCmds");
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;   // start signaled so the first wait-on-reuse returns immediately
    for (uint32_t i = 0; i < N; i++) check(vkCreateFence(g_vkDev, &fi, nullptr, &g_fences[i]), "Fence");
    if (N < 2) XLOG("WARNING: swapchain has %u image(s) — no frame overlap (pipelining lost)", N);
    return g_dpool && g_cpool && g_cmds[0] && g_fences[0];
}

static bool ensure_init() {
    if (g_inited) return true;
    if (g_failed || !g_gfx_captured) return false;

    // recompute panel metrics from settings + backbuffer aspect
    g_panel_w = (int)g_swapW; g_panel_h = (int)g_swapH;
    g_panel_wm = 0.5f * g_mei.panel_scale;
    g_panel_hm = g_panel_wm * (float)g_swapH / (float)g_swapW;

    if (!make_swapchain()) { g_failed = true; return false; }
    if (!make_renderpass()){ g_failed = true; return false; }
    if (!make_framebuffers()){ g_failed = true; return false; }
    if (!make_vk_objects()){ g_failed = true; return false; }

    IMGUI_CHECKVERSION();
    g_menuCtx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;                 // no disk ini
    io.DisplaySize = ImVec2((float)g_swapW, (float)g_swapH);
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;   // stick can also drive
    io.MouseDrawCursor = true;                // draw a SOFTWARE cursor (no OS cursor exists in VR)
    io.ConfigInputTrickleEventQueue = false;  // apply same-frame cursor move + click together (no split)
    io.Fonts->AddFontDefault();
    mei_style();

    // We link libvulkan directly, so imgui_impl_vulkan uses its own prototypes (no loader shim).
    ImGui_ImplVulkan_InitInfo ii{};
    ii.Instance = g_vkInst; ii.PhysicalDevice = g_vkPhys; ii.Device = g_vkDev;
    ii.QueueFamily = g_vkQF; ii.Queue = g_vkQueue;
    ii.DescriptorPool = g_dpool; ii.RenderPass = g_rp;
    ii.MinImageCount = (uint32_t)g_images.size(); ii.ImageCount = (uint32_t)g_images.size();
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    if (!ImGui_ImplVulkan_Init(&ii)) { XLOG("ImGui_ImplVulkan_Init failed"); g_failed = true; return false; }
    ImGui_ImplVulkan_CreateFontsTexture();

    g_inited = true;
    XLOG("mei backend up: panel %dx%d (%.2fx%.2f m)", g_panel_w, g_panel_h, g_panel_wm, g_panel_hm);
    return true;
}

static double now_s() { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec + t.tv_nsec/1e9; }

// Locate the head (VIEW space) in world-locked LOCAL space. Returns pos + orientation.
static bool locate_head(XrTime t, V3f* pos, XrQuaternionf* ori) {
    if (g_localspace == XR_NULL_HANDLE || g_viewspace == XR_NULL_HANDLE) return false;
    if (!pfn_xrLocate && !xr_get("xrLocateSpace", &pfn_xrLocate)) return false;
    XrSpaceLocation sl{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(pfn_xrLocate(g_viewspace, g_localspace, t, &sl))) return false;
    if (!(sl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) ||
        !(sl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) return false;
    if (pos) *pos = { sl.pose.position.x, sl.pose.position.y, sl.pose.position.z };
    if (ori) *ori = sl.pose.orientation;
    return true;
}

// (Re)anchor the panel when it (re)opens: at the saved head-relative offset if the user placed it,
// otherwise straight in front of the head. World-locked in LOCAL space from there.
static void place_panel(XrTime t) {
    if (!g_needPlace) return;
    V3f hp; XrQuaternionf hq;
    if (!locate_head(t, &hp, &hq)) return;
    if (g_mei.panel_custom) {                          // restore the user's placement (head-relative offset)
        V3f off = { g_mei.panel_off[0], g_mei.panel_off[1], g_mei.panel_off[2] };
        V3f rel = qrot(hq, off);
        g_panelPose.position = { hp.x + rel.x, hp.y + rel.y, hp.z + rel.z };
        g_panelPose.orientation = quat_face(xv(g_panelPose.position), hp);
    } else {                                           // default: arm's-length dead ahead
        V3f fwd = qrot(hq, { 0, 0, -1 });
        g_panelPose.orientation = hq;
        g_panelPose.position = { hp.x + fwd.x * g_mei.panel_dist,
                                 hp.y + fwd.y * g_mei.panel_dist,
                                 hp.z + fwd.z * g_mei.panel_dist };
    }
    g_needPlace = false;
    XLOG("panel anchored at local (%.2f,%.2f,%.2f) custom=%d",
         g_panelPose.position.x, g_panelPose.position.y, g_panelPose.position.z, g_mei.panel_custom);
}

// Grab-to-move: in reposition mode, point OFF the panel and hold the trigger to pin the panel to the
// controller ray at the grab-moment distance; it follows your hand and faces you. Release to drop
// (position saved as a head-relative offset so it survives reopen). Point AT the panel to click as usual.
static void grab_update(XrTime t) {
    if (!g_mei.reposition) { g_grabbing = false; return; }
    if (g_trigger_raw && g_ctrlValidL) {
        if (!g_grabbing) {
            if (g_cursor_valid) return;                // pointing at a widget -> let it click, don't grab
            V3f d = v3sub(xv(g_panelPose.position), g_ctrlPosL);
            g_grab_dist = v3len(d);
            if (g_grab_dist < 0.20f || g_grab_dist > 3.0f) g_grab_dist = 0.6f;
            g_grabbing = true;
        }
        g_panelPose.position = { g_ctrlPosL.x + g_ctrlFwdL.x * g_grab_dist,
                                 g_ctrlPosL.y + g_ctrlFwdL.y * g_grab_dist,
                                 g_ctrlPosL.z + g_ctrlFwdL.z * g_grab_dist };
        V3f hp;
        if (locate_head(t, &hp, nullptr)) g_panelPose.orientation = quat_face(xv(g_panelPose.position), hp);
    } else if (g_grabbing) {
        g_grabbing = false;
        V3f hp; XrQuaternionf hq;
        if (locate_head(t, &hp, &hq)) {                // store final pos as a head-relative offset + persist
            V3f local = qrot(qconj(hq), v3sub(xv(g_panelPose.position), hp));
            g_mei.panel_off[0]=local.x; g_mei.panel_off[1]=local.y; g_mei.panel_off[2]=local.z;
            g_mei.panel_custom = true;
            mei_save();
        }
        XLOG("panel dropped at (%.2f,%.2f,%.2f)", g_panelPose.position.x, g_panelPose.position.y, g_panelPose.position.z);
    }
}

// Locate the right-controller aim pose in world-locked LOCAL space, intersect it with the anchored
// panel plane, and push the resulting cursor into the input bridge. Hand-driven (no gun needed).
static void update_aim_ray(XrTime t) {
    g_ctrlValidL = false;                                  // invalidated until this frame's locate succeeds
    if (!g_actions_built || g_actAim == XR_NULL_HANDLE || g_localspace == XR_NULL_HANDLE) {
        mei_input_set_cursor(0, 0, false); return; }
    if (g_aimSpace == XR_NULL_HANDLE) {              // lazily create the action space (after attach)
        PFN_xrCreateActionSpace mk;
        if (!xr_get("xrCreateActionSpace", &mk)) { mei_input_set_cursor(0,0,false); return; }
        XrActionSpaceCreateInfo ci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        ci.action = g_actAim; ci.poseInActionSpace.orientation = { 0, 0, 0, 1 };
        if (XR_FAILED(mk(g_session, &ci, &g_aimSpace)) || g_aimSpace == XR_NULL_HANDLE) {
            mei_input_set_cursor(0,0,false); return; }
        XLOG("aim action space created");
    }
    if (!pfn_xrLocate && !xr_get("xrLocateSpace", &pfn_xrLocate)) { mei_input_set_cursor(0,0,false); return; }
    XrSpaceLocation sl{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(pfn_xrLocate(g_aimSpace, g_localspace, t, &sl)) ||
        !(sl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) ||
        !(sl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) { mei_input_set_cursor(0,0,false); return; }

    // controller ray in LOCAL space
    XrVector3f cp = sl.pose.position;
    V3f fwdW = qrot(sl.pose.orientation, { 0, 0, -1 });   // aim points down -Z
    g_ctrlPosL = { cp.x, cp.y, cp.z }; g_ctrlFwdL = fwdW; g_ctrlValidL = true;   // publish for grab-to-move
    // transform ray into the panel's local frame (panel at origin, front = +Z, spans ±wm/2 ±hm/2)
    XrQuaternionf inv = qconj(g_panelPose.orientation);
    V3f rel = { cp.x - g_panelPose.position.x, cp.y - g_panelPose.position.y, cp.z - g_panelPose.position.z };
    V3f o = qrot(inv, rel);
    V3f d = qrot(inv, fwdW);
    if (d.z >= -1e-4f) { mei_input_set_cursor(0,0,false); return; }   // not pointing at the panel front
    float tt = -o.z / d.z;
    if (tt <= 0.f) { mei_input_set_cursor(0,0,false); return; }
    float hx = o.x + tt * d.x, hy = o.y + tt * d.y;
    float u = (hx + g_panel_wm * 0.5f) / g_panel_wm;
    float v = (g_panel_hm * 0.5f - hy) / g_panel_hm;
    bool on = (u > -0.05f && u < 1.05f && v > -0.05f && v < 1.05f);
    mei_input_set_cursor(u, v, on);
}

// Render one ImGui frame into the acquired swapchain image. Runs on the app's render/xrEndFrame
// thread. CRITICAL: we do NOT CPU-block on freshly submitted work — the only wait is on the
// per-image fence for REUSE (already signaled in steady state), so our submit overlaps UE's frame.
static void render_frame(XrTime displayTime) {
    if (!pfn_xrAcquire || !pfn_xrWait || !pfn_xrRelease) return;
    // robustly (re)anchor on any open transition (L3, gesture, or file override)
    static bool s_prev_open = false;
    if (g_mei.menu_open && !s_prev_open) g_needPlace = true;
    s_prev_open = g_mei.menu_open;
    if (g_mei.act_replace) { g_mei.act_replace = false; g_mei.panel_custom = false; g_needPlace = true; }  // Reset position
    place_panel(displayTime);         // world-lock: fix the panel in front of the head on (re)open
    update_aim_ray(displayTime);      // controller ray in LOCAL space -> cursor (before NewFrame)
    grab_update(displayTime);         // reposition mode: drag the panel with the controller ray

    // Acquire/wait/release must stay balanced. xrWaitSwapchainImage can return XR_TIMEOUT_EXPIRED,
    // which is a POSITIVE success code (!= XR_SUCCESS) — an image that timed out is acquired but NOT
    // waited, and releasing a not-waited image is XR_ERROR_CALL_ORDER_INVALID (leaks the image ->
    // after imageCount timeouts the swapchain soft-locks). So: acquire once, and on timeout KEEP the
    // image and retry the wait next frame; only release after a successful wait.
    static bool s_have_acq = false; static uint32_t s_acq_idx = 0;
    uint32_t idx = 0;
    if (!s_have_acq) {
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_FAILED(pfn_xrAcquire(g_swap, &ai, &idx))) return;
        s_acq_idx = idx; s_have_acq = true;
    } else { idx = s_acq_idx; }                       // re-use the image we acquired but couldn't wait on
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wi.timeout = 5000000; // 5 ms
    XrResult wr = pfn_xrWait(g_swap, &wi);
    if (wr == XR_TIMEOUT_EXPIRED) return;             // still acquired; retry the wait next frame
    if (wr != XR_SUCCESS) { s_have_acq = false; return; }   // real failure: drop state, don't release un-waited
    s_have_acq = false;                               // waited OK -> we own it through the release below
    if (idx >= g_fbs.size()) { XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        pfn_xrRelease(g_swap, &ri); return; }

    // Cmd buffer + fence cycle by a MONOTONIC slot (mod N), advancing once per frame in lockstep
    // with ImGui_ImplVulkan's own internal vertex/index buffer counter — so g_fences[slot] protects
    // exactly the imgui buffer that will be reused this slot. (The framebuffer, by contrast, must be
    // the one for the XR-acquired image idx, which the runtime may hand out in any order.)
    static uint32_t g_slot = 0;
    uint32_t N = (uint32_t)g_fences.size();
    uint32_t slot = g_slot % N;

    // wait-on-REUSE: block only if THIS slot's previous submit is still in flight (already signaled
    // in steady state -> ~free; it is NOT a wait on the frame we're about to submit).
    vkWaitForFences(g_vkDev, 1, &g_fences[slot], VK_TRUE, UINT64_MAX);
    vkResetFences(g_vkDev, 1, &g_fences[slot]);

    // ---- build UI ----
    double t = now_s(); ImGuiIO& io = ImGui::GetIO();
    float dt = g_last_time > 0 ? (float)(t - g_last_time) : 1.f/72.f;
    io.DeltaTime = dt > 1e-4f ? dt : 1e-4f;   // clamp: two frames sharing a timestamp must not be 0
    g_last_time = t;
    io.DisplaySize = ImVec2((float)g_swapW, (float)g_swapH);
    // cursor events were already queued by update_aim_ray() above (before NewFrame processes them)
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    mei_menu_frame(g_panel_w, g_panel_h);
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();

    // ---- record + submit (NO host wait after) ----
    VkCommandBuffer cmd = g_cmds[slot];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    VkClearValue clear{}; clear.color = { { 0.f, 0.f, 0.f, 0.f } };   // transparent -> alpha-blend quad
    VkRenderPassBeginInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpi.renderPass = g_rp; rpi.framebuffer = g_fbs[idx];
    rpi.renderArea.extent = { g_swapW, g_swapH };
    rpi.clearValueCount = 1; rpi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rpi, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(dd, cmd);
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    vkQueueSubmit(g_vkQueue, 1, &si, g_fences[slot]);   // fence signals when done; we DON'T wait here
    g_slot++;                                           // advance in lockstep with imgui's buffer counter

    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    pfn_xrRelease(g_swap, &ri);       // OpenXR runtime GPU-synchronizes the handoff; no host wait needed
    mei_menu_flush();
}

// ============================================================================
//  ESP overlay surface (second view-locked quad, own swapchain + ImGui context)
// ============================================================================
static bool esp_ensure_init() {
    if (g_espInited) return true;
    if (g_espFailed || !g_gfx_captured || !g_inited) return false;   // needs the menu backend (g_rp/g_cpool/format)
    PFN_xrCreateSwapchain mkSc; PFN_xrEnumerateSwapchainImages enumSc;
    if (!xr_get("xrCreateSwapchain",&mkSc) || !xr_get("xrEnumerateSwapchainImages",&enumSc)) { g_espFailed=true; return false; }
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format=g_swapFmt; ci.sampleCount=1; ci.width=ESP_W; ci.height=ESP_H; ci.faceCount=1; ci.arraySize=1; ci.mipCount=1;
    if (XR_FAILED(mkSc(g_session,&ci,&g_espSwap))) { g_espFailed=true; XLOG("esp xrCreateSwapchain failed"); return false; }
    uint32_t n=0; enumSc(g_espSwap,0,&n,nullptr);
    std::vector<XrSwapchainImageVulkanKHR> imgs(n,{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    enumSc(g_espSwap,n,&n,(XrSwapchainImageBaseHeader*)imgs.data());
    g_espImages.clear(); for(auto&im:imgs) g_espImages.push_back(im.image);
    for (VkImage img : g_espImages) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image=img; vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=(VkFormat)g_swapFmt;
        vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkImageView v; check(vkCreateImageView(g_vkDev,&vi,nullptr,&v),"esp view"); g_espViews.push_back(v);
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass=g_rp; fi.attachmentCount=1; fi.pAttachments=&v; fi.width=ESP_W; fi.height=ESP_H; fi.layers=1;
        VkFramebuffer fb; check(vkCreateFramebuffer(g_vkDev,&fi,nullptr,&fb),"esp fb"); g_espFbs.push_back(fb);
    }
    VkDescriptorPoolSize sz{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,8};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT; dp.maxSets=8; dp.poolSizeCount=1; dp.pPoolSizes=&sz;
    check(vkCreateDescriptorPool(g_vkDev,&dp,nullptr,&g_espDpool),"esp dpool");
    uint32_t N=(uint32_t)g_espImages.size(); if(N==0){ g_espFailed=true; return false; }
    g_espCmds.resize(N); g_espFences.resize(N);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool=g_cpool; ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount=N;
    check(vkAllocateCommandBuffers(g_vkDev,&ai,g_espCmds.data()),"esp cmds");
    VkFenceCreateInfo ff{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; ff.flags=VK_FENCE_CREATE_SIGNALED_BIT;
    for(uint32_t i=0;i<N;i++) check(vkCreateFence(g_vkDev,&ff,nullptr,&g_espFences[i]),"esp fence");

    g_espCtx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_espCtx);
    ImGuiIO& io=ImGui::GetIO(); io.IniFilename=nullptr; io.DisplaySize=ImVec2((float)ESP_W,(float)ESP_H);
    io.Fonts->AddFontDefault();
    ImGui_ImplVulkan_InitInfo ii{};
    ii.Instance=g_vkInst; ii.PhysicalDevice=g_vkPhys; ii.Device=g_vkDev; ii.QueueFamily=g_vkQF; ii.Queue=g_vkQueue;
    ii.DescriptorPool=g_espDpool; ii.RenderPass=g_rp; ii.MinImageCount=N; ii.ImageCount=N; ii.MSAASamples=VK_SAMPLE_COUNT_1_BIT;
    bool ok = ImGui_ImplVulkan_Init(&ii);
    if(ok) ImGui_ImplVulkan_CreateFontsTexture();
    ImGui::SetCurrentContext(g_menuCtx);
    if(!ok){ g_espFailed=true; XLOG("esp ImGui_ImplVulkan_Init failed"); return false; }
    g_espInited=true; XLOG("esp overlay up: %ux%u images=%u", ESP_W, ESP_H, N);
    return true;
}

// draw the ESP into the CURRENT (esp) context's background draw-list
static void esp_draw() {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float W=(float)ESP_W, H=(float)ESP_H;
    if (g_mei.esp_crosshair) {
        float cx=W*0.5f, cy=H*0.5f; ImU32 c=IM_COL32(255,255,255,230);
        dl->AddLine(ImVec2(cx-11,cy),ImVec2(cx+11,cy),c,1.6f);
        dl->AddLine(ImVec2(cx,cy-11),ImVec2(cx,cy+11),c,1.6f);
    }
    if (g_mei.esp_fov_circle && g_mei.aim_enabled) {
        const double D2R=0.017453292519943295;
        double ta=tan((g_mei.aim_fov*0.5)*D2R), tf=tan((g_esp_fov_used*0.5)*D2R);
        float r=(float)((ta/tf)*(W*0.5)); if(r<4)r=4; if(r>H)r=H;
        dl->AddCircle(ImVec2(W*0.5f,H*0.5f), r, IM_COL32(255,255,255,90), 48, 1.4f);
    }
    int n=g_esp_n; if(n>MEI_ESP_MAX)n=MEI_ESP_MAX;
    for(int i=0;i<n;i++){
        EspEntry&e=g_esp[i]; if(!e.valid) continue;
        float fx=e.u*W, fy=e.v*H, hx=e.uh*W, hy=e.vh*H;
        float boxH=fy-hy; if(boxH<6) boxH=6; float boxW=boxH*0.42f;
        float cx=(fx+hx)*0.5f, left=cx-boxW*0.5f, right=cx+boxW*0.5f, top=hy, bot=fy;
        ImU32 col = e.team==0 ? IM_COL32(80,170,255,255) : IM_COL32(255,90,90,255);
        if(e.role[0]){ if(strstr(e.role,"raitor")) col=IM_COL32(255,60,60,255);
            else if(strstr(e.role,"etective")) col=IM_COL32(60,120,255,255);
            else col=IM_COL32(120,230,120,255); }
        if(g_mei.esp_box){ dl->AddRect(ImVec2(left-1,top-1),ImVec2(right+1,bot+1),IM_COL32(0,0,0,200),0,0,3.f);
            dl->AddRect(ImVec2(left,top),ImVec2(right,bot),col,0,0,1.5f); }
        if(g_mei.esp_health && e.health>=0.f){ float bx=left-6.f;
            dl->AddRectFilled(ImVec2(bx-2,top),ImVec2(bx+2,bot),IM_COL32(0,0,0,200));
            float hh=(bot-top)*e.health; ImU32 hc=IM_COL32((int)(255*(1.f-e.health)),(int)(255*e.health),40,255);
            dl->AddRectFilled(ImVec2(bx-1,bot-hh),ImVec2(bx+1,bot),hc); }
        char t1[56]=""; int tn=0;
        if(g_mei.esp_name && e.name[0]) tn+=snprintf(t1+tn,sizeof t1-tn,"%s",e.name);
        if(g_mei.esp_role && e.role[0]) tn+=snprintf(t1+tn,sizeof t1-tn," [%s]",e.role);
        if(t1[0]){ ImVec2 ts=font->CalcTextSizeA(20,FLT_MAX,0,t1);
            dl->AddText(font,20,ImVec2(cx-ts.x*0.5f+1,top-23),IM_COL32(0,0,0,220),t1);
            dl->AddText(font,20,ImVec2(cx-ts.x*0.5f,top-24),col,t1); }
        if(g_mei.esp_dist){ char db[24]; snprintf(db,sizeof db,"%.0fm",e.dist);
            ImVec2 ts=font->CalcTextSizeA(18,FLT_MAX,0,db);
            dl->AddText(font,18,ImVec2(cx-ts.x*0.5f+1,bot+4),IM_COL32(0,0,0,220),db);
            dl->AddText(font,18,ImVec2(cx-ts.x*0.5f,bot+3),IM_COL32(230,230,230,255),db); }
    }
}

static void esp_render(XrTime) {
    if(!g_espInited || !pfn_xrAcquire || !pfn_xrWait || !pfn_xrRelease) return;
    static bool have=false; static uint32_t aidx=0; uint32_t idx=0;
    if(!have){ XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if(XR_FAILED(pfn_xrAcquire(g_espSwap,&ai,&idx))) return; aidx=idx; have=true; } else idx=aidx;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wi.timeout=5000000;
    XrResult wr=pfn_xrWait(g_espSwap,&wi);
    if(wr==XR_TIMEOUT_EXPIRED) return;
    if(wr!=XR_SUCCESS){ have=false; return; }
    have=false;
    if(idx>=g_espFbs.size()){ XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}; pfn_xrRelease(g_espSwap,&ri); return; }
    uint32_t N=(uint32_t)g_espFences.size(); uint32_t slot=g_espSlot%N;
    vkWaitForFences(g_vkDev,1,&g_espFences[slot],VK_TRUE,UINT64_MAX); vkResetFences(g_vkDev,1,&g_espFences[slot]);
    ImGui::SetCurrentContext(g_espCtx);
    ImGuiIO& io=ImGui::GetIO(); io.DisplaySize=ImVec2((float)ESP_W,(float)ESP_H); io.DeltaTime=1.f/72.f;
    ImGui_ImplVulkan_NewFrame(); ImGui::NewFrame();
    esp_draw();
    ImGui::Render(); ImDrawData* dd=ImGui::GetDrawData();
    VkCommandBuffer cmd=g_espCmds[slot]; vkResetCommandBuffer(cmd,0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd,&bi);
    VkClearValue cl{}; cl.color={{0,0,0,0}};
    VkRenderPassBeginInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; rpi.renderPass=g_rp; rpi.framebuffer=g_espFbs[idx];
    rpi.renderArea.extent={ESP_W,ESP_H}; rpi.clearValueCount=1; rpi.pClearValues=&cl;
    vkCmdBeginRenderPass(cmd,&rpi,VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(dd,cmd);
    vkCmdEndRenderPass(cmd); vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount=1; si.pCommandBuffers=&cmd;
    vkQueueSubmit(g_vkQueue,1,&si,g_espFences[slot]); g_espSlot++;
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}; pfn_xrRelease(g_espSwap,&ri);
    ImGui::SetCurrentContext(g_menuCtx);
}

// ============================================================================
//  injected input action set  (real controller buttons: R-trigger click, L-stick toggle)
// ============================================================================
static XrPath xr_path(const char* s) {
    XrPath p = XR_NULL_PATH; PFN_xrStringToPath fn;
    if (xr_get("xrStringToPath", &fn)) fn(g_inst, s, &p);
    return p;
}
// Create our action set + two boolean actions. NO standalone xrSuggestInteractionProfileBindings
// here: that call REPLACES all bindings for a profile, so a standalone suggest for oculus/touch is
// wiped the moment UE suggests its own for the same profile (or vice-versa, which would break the
// game). Instead we MERGE our bindings into UE's own suggest call (hk_xrSuggest below).
static void ensure_actions_created(XrInstance inst) {
    if (g_actionSet != XR_NULL_HANDLE) return;      // once
    g_inst = inst;                                  // so xr_get/xr_path resolve against this instance
    PFN_xrCreateActionSet mkSet; PFN_xrCreateAction mkAct;
    if (!xr_get("xrCreateActionSet", &mkSet) || !xr_get("xrCreateAction", &mkAct)) return;

    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    strncpy(si.actionSetName, "mei", sizeof si.actionSetName - 1);
    strncpy(si.localizedActionSetName, "mei menu", sizeof si.localizedActionSetName - 1);
    si.priority = 0;
    if (XR_FAILED(mkSet(inst, &si, &g_actionSet))) { XLOG("xrCreateActionSet failed"); return; }

    XrActionCreateInfo ac{XR_TYPE_ACTION_CREATE_INFO}; ac.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    strncpy(ac.actionName, "menu_click", sizeof ac.actionName - 1);
    strncpy(ac.localizedActionName, "Click", sizeof ac.localizedActionName - 1);
    mkAct(g_actionSet, &ac, &g_actClick);
    XrActionCreateInfo at{XR_TYPE_ACTION_CREATE_INFO}; at.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    strncpy(at.actionName, "menu_toggle", sizeof at.actionName - 1);
    strncpy(at.localizedActionName, "Menu", sizeof at.localizedActionName - 1);
    mkAct(g_actionSet, &at, &g_actToggle);
    XrActionCreateInfo ap{XR_TYPE_ACTION_CREATE_INFO}; ap.actionType = XR_ACTION_TYPE_POSE_INPUT;
    strncpy(ap.actionName, "menu_aim", sizeof ap.actionName - 1);
    strncpy(ap.localizedActionName, "Aim", sizeof ap.localizedActionName - 1);
    mkAct(g_actionSet, &ap, &g_actAim);

    xr_get("xrGetActionStateBoolean", &pfn_xrGetBool);   // cache the per-frame poll PFN
    g_actions_built = (g_actionSet != XR_NULL_HANDLE && g_actClick != XR_NULL_HANDLE);
    XLOG("actions created (set=%p click=%p toggle=%p aim=%p)",
         (void*)g_actionSet, (void*)g_actClick, (void*)g_actToggle, (void*)g_actAim);
}
// map a profile string -> the source paths we bind our click/toggle/aim to on that controller.
static void profile_paths(const char* prof, const char** click, const char** toggle, const char** aim) {
    *click = "/user/hand/right/input/trigger/value";   // touch / touch_plus / touch_pro / index / vive / wmr
    *toggle = "/user/hand/left/input/thumbstick/click";
    *aim   = "/user/hand/right/input/aim/pose";         // aim pose exists on every interaction profile
    if (strstr(prof, "khr/simple")) {                  // simple controller has no trigger value/stick
        *click = "/user/hand/right/input/select/click";
        *toggle = "/user/hand/left/input/menu/click";
    } else if (strstr(prof, "vive_controller") || strstr(prof, "microsoft/motion")) {
        *toggle = "/user/hand/left/input/trackpad/click";   // vive/wmr have no stick click -> trackpad
    }
}

// read our two buttons; open/close on the toggle's rising edge, feed the trigger to the cursor.
static void poll_actions() {
    if (!g_actions_built || !pfn_xrGetBool) return;
    PFN_xrGetActionStateBoolean getB = pfn_xrGetBool;   // cached (no loader string-walk per frame)
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};

    XrActionStateBoolean click{XR_TYPE_ACTION_STATE_BOOLEAN}; gi.action = g_actClick;
    if (XR_SUCCEEDED(getB(g_session, &gi, &click))) {
        if (click.isActive) {
            g_actions_live = true; g_hw_input_active = true;
            g_trigger_raw = (click.currentState != XR_FALSE);
            mei_input_set_trigger(g_grabbing ? false : g_trigger_raw);   // while dragging the panel, trigger moves it, not clicks
        } else if (g_hw_input_active) {
            g_trigger_raw = false;
            mei_input_set_trigger(false);   // controller went inactive mid-press: never leave click latched
        }
    }
    XrActionStateBoolean tog{XR_TYPE_ACTION_STATE_BOOLEAN}; gi.action = g_actToggle;
    static bool prev = false;
    if (XR_SUCCEEDED(getB(g_session, &gi, &tog)) && tog.isActive) {
        g_actions_live = true;
        if (tog.currentState != XR_FALSE && !prev) {
            g_mei.menu_open = !g_mei.menu_open;
            if (g_mei.menu_open) g_needPlace = true;   // re-anchor in front of the head each open
            XLOG("menu %s (L3 stick-press)", g_mei.menu_open ? "OPEN" : "closed");
        }
        prev = (tog.currentState != XR_FALSE);
    }
}

// ============================================================================
//  hooked OpenXR entry points
// ============================================================================
typedef XrResult (XRAPI_PTR *PFN_xrEndFrame_t)(XrSession, const XrFrameEndInfo*);
static PFN_xrEndFrame_t real_xrEndFrame = nullptr;

static XrResult XRAPI_PTR hk_xrEndFrame(XrSession session, const XrFrameEndInfo* info) {
    // one-time: log the thread we run on, to confirm same-thread affinity with UE's queue submit
    // (shared-queue submit is only safe if xrEndFrame and UE submit on the SAME thread — see S1).
    static bool tid_logged = false;
    if (!tid_logged) { tid_logged = true; XLOG("hk_xrEndFrame thread tid=%d", (int)gettid()); }

    poll_actions();   // read L3 (open/close) + trigger every frame, even while the menu is closed

    // Build the backend EAGERLY (before first open) so opening the menu never hitches on the
    // one-time swapchain/pipeline/font-texture creation. Runs on this (render) thread, as required.
    if (g_gfx_captured && !g_inited && !g_failed) ensure_init();
    // ESP backend up eagerly when enabled (never blocks the menu path)
    if (g_gfx_captured && g_inited && g_mei.esp_enabled && !g_espInited && !g_espFailed) esp_ensure_init();

    if (!info) return real_xrEndFrame(session, info);

    static const XrCompositionLayerBaseHeader* scratch[34];
    uint32_t nc = info->layerCount; if (nc > 32) nc = 32;
    for (uint32_t i = 0; i < nc; i++) scratch[i] = info->layers[i];
    uint32_t extra = 0;

    // ESP overlay quad FIRST (composited UNDER the menu). VIEW space, full-FOV, transparent.
    XrCompositionLayerQuad espQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    if (g_mei.esp_enabled && g_espInited) {
        esp_render(info->displayTime);
        const double D2R = 0.017453292519943295;
        float wm = (float)(2.0 * tan((g_esp_fov_used*0.5)*D2R) * 1.0);   // spans esp_fov at 1 m
        float hm = wm / MEI_ESP_ASPECT;
        espQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        espQuad.space = g_viewspace; espQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        espQuad.subImage.swapchain = g_espSwap; espQuad.subImage.imageRect.offset = { 0, 0 };
        espQuad.subImage.imageRect.extent = { (int32_t)ESP_W, (int32_t)ESP_H }; espQuad.subImage.imageArrayIndex = 0;
        espQuad.pose.orientation = { 0,0,0,1 }; espQuad.pose.position = { 0.f, 0.f, -1.0f };
        espQuad.size = { wm, hm };
        scratch[nc + extra++] = (const XrCompositionLayerBaseHeader*)&espQuad;
    }

    // Menu quad (world-locked, ON TOP).
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    if (g_mei.menu_open && g_inited) {
        ImGui::SetCurrentContext(g_menuCtx);
        render_frame(info->displayTime);
        quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        quad.space = (g_localspace != XR_NULL_HANDLE) ? g_localspace : g_viewspace;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = g_swap; quad.subImage.imageRect.offset = { 0, 0 };
        quad.subImage.imageRect.extent = { (int32_t)g_swapW, (int32_t)g_swapH }; quad.subImage.imageArrayIndex = 0;
        if (g_localspace != XR_NULL_HANDLE) { quad.pose = g_panelPose; }
        else { quad.pose.orientation = { 0,0,0,1 }; quad.pose.position = { 0.f, 0.f, -g_mei.panel_dist }; }
        quad.size = { g_panel_wm, g_panel_hm };
        scratch[nc + extra++] = (const XrCompositionLayerBaseHeader*)&quad;
    }

    if (extra == 0) return real_xrEndFrame(session, info);
    XrFrameEndInfo mod = *info;
    mod.layerCount = nc + extra;
    mod.layers = scratch;
    return real_xrEndFrame(session, &mod);
}

typedef XrResult (XRAPI_PTR *PFN_xrCreateSession_t)(XrInstance, const XrSessionCreateInfo*, XrSession*);
static PFN_xrCreateSession_t real_xrCreateSession = nullptr;

static void capture_gfx(const XrSessionCreateInfo* ci) {
    // walk the create-info next chain for the Vulkan graphics binding
    for (const XrBaseInStructure* b = (const XrBaseInStructure*)ci->next; b; b = b->next) {
        if (b->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR ||
            b->type == XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR) {
            auto* g = (const XrGraphicsBindingVulkanKHR*)b;
            g_vkInst = g->instance; g_vkPhys = g->physicalDevice; g_vkDev = g->device;
            g_vkQF   = g->queueFamilyIndex;
            vkGetDeviceQueue(g_vkDev, g->queueFamilyIndex, g->queueIndex, &g_vkQueue);
            XLOG("captured Vk: inst=%p phys=%p dev=%p qf=%u queue=%p",
                 (void*)g_vkInst, (void*)g_vkPhys, (void*)g_vkDev, g_vkQF, (void*)g_vkQueue);
            return;
        }
    }
    XLOG("xrCreateSession: no Vulkan graphics binding found (app may use GLES?)");
}

static XrResult XRAPI_PTR hk_xrCreateSession(XrInstance instance, const XrSessionCreateInfo* ci,
                                             XrSession* out) {
    XrResult r = real_xrCreateSession(instance, ci, out);
    if (XR_SUCCEEDED(r) && out) {
        g_inst = instance; g_session = *out;
        capture_gfx(ci);
        // create our VIEW reference space (head-locked panel)
        PFN_xrCreateReferenceSpace xrCRS;
        if (xr_get("xrCreateReferenceSpace", &xrCRS)) {
            XrReferenceSpaceCreateInfo si{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
            si.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
            si.poseInReferenceSpace.orientation = { 0,0,0,1 };
            if (XR_FAILED(xrCRS(g_session, &si, &g_viewspace)))
                XLOG("xrCreateReferenceSpace(VIEW) failed");
            // world-anchored space for the world-locked panel (LOCAL = play-space origin)
            XrReferenceSpaceCreateInfo li{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
            li.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
            li.poseInReferenceSpace.orientation = { 0,0,0,1 };
            if (XR_FAILED(xrCRS(g_session, &li, &g_localspace)))
                XLOG("xrCreateReferenceSpace(LOCAL) failed");
        }
        g_gfx_captured = (g_vkDev != VK_NULL_HANDLE && g_viewspace != XR_NULL_HANDLE);
        ensure_actions_created(instance);   // ensure our set exists (usually already made at first suggest)
        XLOG("session created, gfx_captured=%d actions_built=%d", g_gfx_captured, g_actions_built);
    }
    return r;
}

// MERGE our two bindings into the app's own xrSuggestInteractionProfileBindings call, so our actions
// get bound on whatever profile UE actually uses WITHOUT replacing UE's bindings (suggest replaces
// per-profile, so appending inside UE's call is the only way both survive).
typedef XrResult (XRAPI_PTR *PFN_suggest_t)(XrInstance, const XrInteractionProfileSuggestedBinding*);
static PFN_suggest_t real_xrSuggest = nullptr;
static XrResult XRAPI_PTR hk_xrSuggest(XrInstance inst, const XrInteractionProfileSuggestedBinding* info) {
    ensure_actions_created(inst);
    if (!g_actions_built || !info || g_actClick == XR_NULL_HANDLE) return real_xrSuggest(inst, info);

    char profstr[128] = "?"; uint32_t plen = 0; PFN_xrPathToString p2s;
    if (xr_get("xrPathToString", &p2s)) p2s(inst, info->interactionProfile, sizeof profstr, &plen, profstr);
    const char* cp; const char* tp; const char* ap; profile_paths(profstr, &cp, &tp, &ap);
    XrPath cpath = xr_path(cp), tpath = xr_path(tp), apath = xr_path(ap);

    static XrActionSuggestedBinding merged[512];
    uint32_t n = info->countSuggestedBindings;
    if (n > 506) return real_xrSuggest(inst, info);           // pathological; pass through untouched
    for (uint32_t i = 0; i < n; i++) merged[i] = info->suggestedBindings[i];
    uint32_t m = n;
    if (cpath != XR_NULL_PATH) merged[m++] = { g_actClick,  cpath };
    if (tpath != XR_NULL_PATH) merged[m++] = { g_actToggle, tpath };
    if (apath != XR_NULL_PATH) merged[m++] = { g_actAim,    apath };

    XrInteractionProfileSuggestedBinding mod = *info;
    mod.suggestedBindings = merged; mod.countSuggestedBindings = m;
    XrResult r = real_xrSuggest(inst, &mod);
    if (r == XR_SUCCESS) XLOG("merged mei bindings into app suggest for %s (%u->%u)", profstr, n, m);
    else { XLOG("merged suggest rejected (r=%d) for %s -> retry app's original untouched", r, profstr);
           r = real_xrSuggest(inst, info); }   // never break the game's own bindings
    return r;
}

// splice our action set into the app's attach so our actions become part of this session
typedef XrResult (XRAPI_PTR *PFN_attach_t)(XrSession, const XrSessionActionSetsAttachInfo*);
static PFN_attach_t real_xrAttach = nullptr;
static XrResult XRAPI_PTR hk_xrAttach(XrSession s, const XrSessionActionSetsAttachInfo* info) {
    if (!g_actions_built || g_actionSet == XR_NULL_HANDLE || !info) return real_xrAttach(s, info);
    std::vector<XrActionSet> sets(info->actionSets, info->actionSets + info->countActionSets);
    sets.push_back(g_actionSet);
    XrSessionActionSetsAttachInfo mod = *info;
    mod.countActionSets = (uint32_t)sets.size(); mod.actionSets = sets.data();
    XrResult r = real_xrAttach(s, &mod);
    XLOG("attach: injected our action set (r=%d total=%u)", r, mod.countActionSets);
    return r;
}
// keep our action set active every sync so getState reads live values
typedef XrResult (XRAPI_PTR *PFN_sync_t)(XrSession, const XrActionsSyncInfo*);
static PFN_sync_t real_xrSync = nullptr;
static XrResult XRAPI_PTR hk_xrSync(XrSession s, const XrActionsSyncInfo* info) {
    if (!g_actions_built || g_actionSet == XR_NULL_HANDLE || !info) return real_xrSync(s, info);
    // reused static scratch — this runs EVERY frame (even menu-closed); no per-frame heap alloc.
    static XrActiveActionSet scratch[64];
    uint32_t nc = info->countActiveActionSets;
    if (nc > 63) return real_xrSync(s, info);   // pathological; don't touch
    for (uint32_t i = 0; i < nc; i++) scratch[i] = info->activeActionSets[i];
    scratch[nc] = { g_actionSet, XR_NULL_PATH };
    XrActionsSyncInfo mod = *info;
    mod.countActiveActionSets = nc + 1; mod.activeActionSets = scratch;
    return real_xrSync(s, &mod);
}

// the dispatch hook: hand the app our wrappers for the calls we care about
static XrResult XRAPI_PTR hk_xrGetInstanceProcAddr(XrInstance instance, const char* name,
                                                   PFN_xrVoidFunction* function) {
    XrResult r = real_xrGIPA(instance, name, function);
    if (XR_FAILED(r) || !function || !*function) return r;
    if (!strcmp(name, "xrCreateSession")) {
        real_xrCreateSession = (PFN_xrCreateSession_t)*function;
        *function = (PFN_xrVoidFunction)hk_xrCreateSession;
    } else if (!strcmp(name, "xrEndFrame")) {
        real_xrEndFrame = (PFN_xrEndFrame_t)*function;
        *function = (PFN_xrVoidFunction)hk_xrEndFrame;
    } else if (!strcmp(name, "xrAttachSessionActionSets")) {
        real_xrAttach = (PFN_attach_t)*function;
        *function = (PFN_xrVoidFunction)hk_xrAttach;
    } else if (!strcmp(name, "xrSyncActions")) {
        real_xrSync = (PFN_sync_t)*function;
        *function = (PFN_xrVoidFunction)hk_xrSync;
    } else if (!strcmp(name, "xrSuggestInteractionProfileBindings")) {
        real_xrSuggest = (PFN_suggest_t)*function;
        *function = (PFN_xrVoidFunction)hk_xrSuggest;
    }
    return r;
}

// ============================================================================
//  install
// ============================================================================
static PFN_xrGetInstanceProcAddr find_real_gipa() {
    // the app already linked the OpenXR loader; resolve the symbol from whatever provides it.
    PFN_xrGetInstanceProcAddr f =
        (PFN_xrGetInstanceProcAddr)dlsym(RTLD_DEFAULT, "xrGetInstanceProcAddr");
    if (f) return f;
    const char* libs[] = { "libopenxr_loader.so", "libopenxr_forwardloader.so",
                           "libopenxr.so", "libovr_openxr_loader.so" };
    for (const char* l : libs) {
        void* h = dlopen(l, RTLD_NOW | RTLD_NOLOAD);
        if (!h) h = dlopen(l, RTLD_NOW);
        if (h) { f = (PFN_xrGetInstanceProcAddr)dlsym(h, "xrGetInstanceProcAddr"); if (f) return f; }
    }
    return nullptr;
}

bool mei_xr_install() {
    static bool installed = false;
    if (installed) return true;              // NEVER inline-hook twice (would patch the trampoline)
    real_xrGIPA = find_real_gipa();
    if (!real_xrGIPA) return false;          // loader not mapped yet — caller retries (quietly)
    if (!inline_hook_abs((void*)real_xrGIPA, (void*)hk_xrGetInstanceProcAddr, (void**)&real_xrGIPA)) {
        XLOG("failed to hook xrGetInstanceProcAddr"); return false;
    }
    installed = true;
    XLOG("xrGetInstanceProcAddr hooked (real via tramp=%p)", (void*)real_xrGIPA);
    return true;
}

bool mei_xr_ready() { return g_inited; }
