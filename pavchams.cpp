// pavchams.cpp — self-resolving x-ray chams for Pavlov (Steam Frame build, UE5.1, arm64 Android).
//
// Loaded by libEOSSDK.so's constructor (dlopen), which already injects into the process. No
// patchelf, no SDK offsets: this build's libUnreal.so is a fully-stripped monolith, so we find
// everything against the LIVE binary at runtime and validate it:
//   * GNames (FNamePool)  : scan lib rw memory for a Blocks[] slot whose block-0 first entry
//                           decodes to "None" (FName id 0 is always "None"). self-verifying.
//   * GObjects (chunked)  : scan for {Objects**, Num, Max} whose object[0..] class names decode
//                           to readable ASCII through GNames. cross-validated.
//   * ProcessEvent        : scan .text for the ADRP/ADD pair that computes the address of the
//                           "…/UObject/ScriptCore.cpp" string (only ProcessEvent & friends live
//                           there), then walk back to the enclosing stp x29,x30 prologue.
// From there classes / properties / UFunctions resolve BY NAME through reflection, and we inline-
// hook ProcessEvent so our chams pass runs on the game thread every frame (throttled). The pass
// applies the game's own XRayMaterialTeam0/1 to each enemy pawn's Avatar mesh via SetMaterial —
// identical to the Windows PostRender reference, RHI-agnostic, zero renderer contact.

#include <dlfcn.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <time.h>
#include <cmath>
#include <android/log.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <cstdlib>

#include "mei/mei_settings.h"   // granular feature state (menu <-> core)
#include "mei/mei_input.h"      // controller-ray -> ImGui cursor bridge
#include "mei/mei_xr.h"         // OpenXR/Vulkan menu injection
#include "mei/mei_esp.h"        // shared ESP entry buffer (we project, mei_xr draws)

EspEntry     g_esp[MEI_ESP_MAX];
volatile int g_esp_n = 0;
float        g_esp_fov_used = 97.f;

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "PAVCHAMS", __VA_ARGS__)

// ---- UE5.1 struct layout (stable 4.25→5.1 arm64; reflection-by-name flags any drift) --------
#define UOBJ_CLASS_OFF     0x10   // UObject::ClassPrivate
#define UOBJ_NAME_OFF      0x18   // UObject::NamePrivate (FName; int32 ComparisonIndex @ +0)
#define USTRUCT_SUPER_OFF  0x40   // UStruct::SuperStruct
#define USTRUCT_CHILDREN   0x48   // UStruct::Children (UField*)
#define USTRUCT_CHILDPROPS 0x50   // UStruct::ChildProperties (FField*)
#define UFIELD_NEXT_OFF    0x28   // UField::Next
#define FFIELD_NEXT_OFF    0x20   // FField::Next
#define FFIELD_NAME_OFF    0x28   // FField::NamePrivate (FName)
#define FPROP_OFFSET_OFF   0x4C   // FProperty::Offset_Internal
// ---- extra UE5.1 layout for the SDK dumper (all verified vs the AndUEDumper FField-5.x profile) ----
#define UOBJ_OUTER_OFF     0x20   // UObject::OuterPrivate
#define USTRUCT_PROPSIZE   0x58   // UStruct::PropertiesSize (int32) = struct byte size
#define UFUNC_FLAGS_OFF    0xB0   // UFunction::FunctionFlags (uint32)
#define UFUNC_PARMSIZE_OFF 0xB6   // UFunction::ParmsSize (uint16)
#define UFUNC_FUNC_OFF     0xD8   // UFunction::Func (native thunk ptr)
#define FPROP_ARRAYDIM_OFF 0x34   // FProperty::ArrayDim (int32)
#define FPROP_ELEMSIZE_OFF 0x38   // FProperty::ElementSize (int32)
#define FPROP_FLAGS_OFF    0x40   // FProperty::PropertyFlags (uint64)
#define FPROP_SUB_OFF      0x78   // first subclass-specific field (PropertyClass/Struct/Inner/Enum/…)
#define UENUM_NAMES_OFF    0x40   // UEnum::Names (TArray<TPair<FName,int64>>, 16B pairs)
// EPropertyFlags (param classification)
#define CPF_Parm           0x0000000000000080ULL
#define CPF_OutParm        0x0000000000000100ULL
#define CPF_ReturnParm     0x0000000000000400ULL
#define CPF_ConstParm      0x0000000000000800ULL
#define CPF_ReferenceParm  0x0000000008000000ULL
// FNamePool / FName
#define FNAME_BLOCK_BITS   16
#define OBJ_PER_CHUNK      (64 * 1024)
#define FUOBJECTITEM_SIZE  24

// ===========================================================================
//  resolved-at-runtime state
// ===========================================================================
static uintptr_t g_base = 0, g_text_lo = 0, g_text_hi = 0, g_lib_hi = 0;
static void**    g_name_blocks = nullptr;         // &FNamePool::Blocks[0]
static int       g_name_shift  = 6;               // FNameEntryHeader: Len = header >> shift (auto)
static void*     g_gobjects    = nullptr;         // FChunkedFixedUObjectArray*
static uintptr_t g_scriptcore_va = 0;

typedef void (*PFN_PE)(void* obj, void* func, void* params);
static PFN_PE g_ProcessEvent = nullptr;           // -> trampoline (runs original PE)
static uintptr_t g_pe_rel = 0;                    // resolved ProcessEvent, module-relative (for the SDK dump)

// ---- self memory map -------------------------------------------------------
// g_read  : every readable region in the process (block/chunk/object memory is malloc'd, so it
//           lives in heap/anon regions, NOT inside libUnreal.so — validation must see all of it).
// g_scan  : libUnreal.so's writable regions only (where the GNames/GObjects GLOBALS themselves sit).
struct Region { uintptr_t lo, hi; };
static Region g_read[16384]; static int g_nread = 0;
static Region g_scan[8192];  static int g_nscan = 0;   // ALL writable regions (.bss is often anon)

static bool addr_readable(uintptr_t a) {
    int lo = 0, hi = g_nread - 1;                     // g_read is sorted by lo
    while (lo <= hi) { int m = (lo + hi) / 2;
        if (a < g_read[m].lo) hi = m - 1;
        else if (a >= g_read[m].hi) lo = m + 1;
        else return true; }
    return false;
}

static void scan_maps() {
    g_nread = 0; g_nscan = 0; g_text_lo = g_text_hi = 0;
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        uintptr_t lo, hi; char perms[8]; int off, dev1, dev2; long ino; char path[400];
        path[0] = 0;
        int nf = sscanf(line, "%lx-%lx %4s %x %x:%x %ld %399[^\n]",
                        &lo, &hi, perms, &off, &dev1, &dev2, &ino, path);
        if (nf < 7) continue;
        // trim leading spaces on path
        char* p = path; while (*p == ' ') p++;
        if (perms[0] == 'r' && g_nread < 16384) g_read[g_nread++] = { lo, hi };
        // globals (GUObjectArray / FNamePool) live in .bss — an anonymous rw region, OR one named
        // libUnreal.so. Scan those only; NEVER device maps (GPU/ashmem fault the CPU on access).
        bool dev = strstr(p, "/dev/") || strstr(p, "kgsl") || strstr(p, "mali") ||
                   strstr(p, "dmabuf") || strstr(p, "/system/") || strstr(p, "facebook") ||
                   strstr(p, "[stack") || strstr(p, "/vendor/");
        bool anon = (p[0] == 0) || (p[0] == '[' && strstr(p, "anon")) || strstr(p, "libUnreal.so") ||
                    strstr(p, "malloc") || strstr(p, "scudo") || strstr(p, "bss");
        if (perms[0] == 'r' && perms[1] == 'w' && !dev && anon && g_nscan < 8192)
            g_scan[g_nscan++] = { lo, hi };
        if (strstr(p, "libUnreal.so")) {
            if (!g_base || lo < g_base) g_base = lo;
            if (hi > g_lib_hi) g_lib_hi = hi;
            if (perms[2] == 'x') { if (!g_text_lo || lo < g_text_lo) g_text_lo = lo;
                                   if (hi > g_text_hi) g_text_hi = hi; }
        }
    }
    fclose(f);
}

// ---- fault guard: some readable maps still fault the CPU; skip them instead of dying ---------
static __thread sigjmp_buf g_fjmp;
static __thread volatile int g_fguard = 0;
static const int FAULT_SIGS[5] = { SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGABRT };
static struct sigaction g_old[5];
static void fault_handler(int sig, siginfo_t* si, void* uc) {
    if (g_fguard) siglongjmp(g_fjmp, 1);
    for (int i = 0; i < 5; i++) if (FAULT_SIGS[i] == sig) {        // chain to ART/original
        struct sigaction* o = &g_old[i];
        if ((o->sa_flags & SA_SIGINFO) && o->sa_sigaction) o->sa_sigaction(sig, si, uc);
        else if (o->sa_handler && o->sa_handler != SIG_DFL && o->sa_handler != SIG_IGN) o->sa_handler(sig);
        else { signal(sig, SIG_DFL); raise(sig); }
        return;
    }
}
static void install_fault() {
    struct sigaction sa{}; sa.sa_sigaction = fault_handler; sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int i = 0; i < 5; i++) sigaction(FAULT_SIGS[i], &sa, &g_old[i]);
}
#define GUARD_REGION_ELSE_SKIP  g_fguard = 1; if (sigsetjmp(g_fjmp, 1)) { g_fguard = 0; continue; }

// ===========================================================================
//  FName decode (once g_name_blocks is known)
// ===========================================================================
// decode against an explicit block table + shift (used by both the live decoder and the auto-tuner)
static bool decode_name(void** blocks, int shift, int32_t idx, char* out, size_t cap) {
    out[0] = 0;
    if (!blocks || idx < 0) return false;
    uint32_t block = (uint32_t)idx >> FNAME_BLOCK_BITS;
    uint32_t off   = ((uint32_t)idx & ((1u << FNAME_BLOCK_BITS) - 1)) * 2;
    uint8_t* blk = (uint8_t*)blocks[block];
    if (!addr_readable((uintptr_t)blk)) return false;
    uint8_t* e = blk + off;
    if (!addr_readable((uintptr_t)e + 2)) return false;
    uint16_t hdr = *(uint16_t*)e;
    if (hdr & 1) return false;                       // wide — skip for validation
    int len = hdr >> shift;
    if (len < 1 || len > 200) return false;
    if (!addr_readable((uintptr_t)(e + 2 + len))) return false;
    if ((size_t)len >= cap) len = (int)cap - 1;
    memcpy(out, e + 2, len); out[len] = 0;
    return true;
}
static void fname_to_str(int32_t idx, char* out, size_t cap) {
    decode_name(g_name_blocks, g_name_shift, idx, out, cap);
}
// reverse FName lookup: walk the name-pool blocks linearly, return the FNameEntryId for `target`
static int32_t fname_find(const char* target) {
    int tlen = (int)strlen(target);
    for (int b = 0; b < 8192; b++) {
        if (!addr_readable((uintptr_t)&g_name_blocks[b])) break;
        uint8_t* base = (uint8_t*)g_name_blocks[b];
        if (!addr_readable((uintptr_t)base)) continue;
        uint32_t off = 0;
        for (int guard = 0; guard < 300000; guard++) {
            uint8_t* e = base + off;
            if (!addr_readable((uintptr_t)e + 2)) break;
            uint16_t hdr = *(uint16_t*)e;
            if (hdr == 0) break;                          // end of block
            bool wide = hdr & 1; int len = hdr >> g_name_shift;
            if (len <= 0 || len > 1024) break;
            int bytelen = wide ? len * 2 : len;
            if (!addr_readable((uintptr_t)(e + 2 + bytelen))) break;
            if (!wide && len == tlen && !memcmp(e + 2, target, tlen))
                return (b << 16) | (off / 2);             // FNameEntryId
            off += 2 + bytelen; off = (off + 1) & ~1u;    // align to 2
        }
    }
    return -1;
}
static void obj_name(void* o, char* out, size_t cap) {
    if (!o) { out[0] = 0; return; }
    fname_to_str(*(int32_t*)((uint8_t*)o + UOBJ_NAME_OFF), out, cap);
}
static void field_name(void* fld, char* out, size_t cap) {
    fname_to_str(*(int32_t*)((uint8_t*)fld + FFIELD_NAME_OFF), out, cap);
}

static bool printable(const char* s) {
    if (!s[0]) return false;
    for (const char* p = s; *p; p++)
        if (!((*p >= 'A'&&*p<='Z')||(*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')||*p=='_'||*p=='/')) return false;
    return true;
}
static bool in_text(uintptr_t a) { return a >= g_text_lo && a < g_text_hi; }
static bool in_lib(uintptr_t a)  { return a >= g_base && a < g_lib_hi; }
// a valid UObject/UClass vtable: the vtable pointer sits in the lib (RELRO/.data.rel.ro), and its
// first virtual-function slot points into .text.
static bool valid_vtable(uintptr_t vt) {
    if (!in_lib(vt) || !addr_readable(vt)) return false;
    return in_text(*(uintptr_t*)vt);
}

// ===========================================================================
//  GObjects locator — structural (object & class vtables point into libUnreal .text; no names)
// ===========================================================================
static void* obj_at_in(void* arr, int32_t i) {
    uint8_t** chunks = *(uint8_t***)arr;               // Objects
    if (!addr_readable((uintptr_t)chunks)) return nullptr;
    uint8_t* chunk = chunks[i / OBJ_PER_CHUNK];
    if (!addr_readable((uintptr_t)chunk)) return nullptr;
    return *(void**)(chunk + (size_t)(i % OBJ_PER_CHUNK) * FUOBJECTITEM_SIZE);
}
static bool find_gobjects() {
    for (int r = 0; r < g_nscan; r++) {
        GUARD_REGION_ELSE_SKIP
        for (uintptr_t s = g_scan[r].lo; s + 0x20 <= g_scan[r].hi; s += 8) {
            int32_t num = *(int32_t*)(s + 0x14);
            int32_t max = *(int32_t*)(s + 0x10);
            if (num < 1000 || num > 6000000 || max < num || max > 8000000) continue;
            if (!addr_readable((uintptr_t)*(void**)s)) continue;   // Objects**
            int good = 0, seen = 0;
            for (int i = 0; i < 24 && seen < 12; i++) {
                void* o = obj_at_in((void*)s, i);
                if (!o) continue;
                seen++;
                if (!addr_readable((uintptr_t)o)) continue;
                uintptr_t ovt = *(uintptr_t*)o;                    // UObject vtable (in .data.rel.ro)
                void* c = *(void**)((uint8_t*)o + UOBJ_CLASS_OFF); // ClassPrivate
                if (!valid_vtable(ovt) || !addr_readable((uintptr_t)c)) continue;
                if (valid_vtable(*(uintptr_t*)c)) good++;          // UClass vtable
            }
            if (good >= 8) {
                g_gobjects = (void*)s;
                LOG("GObjects @ %p (lib+0x%lx) Num=%d Max=%d good=%d",
                    (void*)s, (uintptr_t)s - g_base, num, max, good);
                return true;
            }
        }
        g_fguard = 0;
    }
    return false;
}
static int32_t objects_num() { return *(int32_t*)((uint8_t*)g_gobjects + 0x14); }
static void* object_at(int32_t i) { return obj_at_in(g_gobjects, i); }

// ===========================================================================
//  GNames locator — auto-tune (find block table + header shift by decoding real object names)
// ===========================================================================
static bool find_gnames() {
    // gather real name indices from live objects
    int32_t idxs[32]; int ni = 0;
    int32_t n = objects_num();
    for (int32_t i = 0; i < n && ni < 32; i++) {
        void* o = object_at(i);
        if (!o || !addr_readable((uintptr_t)o)) continue;
        int32_t ci = *(int32_t*)((uint8_t*)o + UOBJ_NAME_OFF);
        if (ci > 0 && ci < (1 << 28)) idxs[ni++] = ci;
    }
    if (ni < 12) return false;

    // GNames lives in the same .bss region as GObjects — scan only that one (all-rw is far too slow)
    uintptr_t blo = 0, bhi = 0;
    for (int r = 0; r < g_nscan; r++)
        if ((uintptr_t)g_gobjects >= g_scan[r].lo && (uintptr_t)g_gobjects < g_scan[r].hi)
            { blo = g_scan[r].lo; bhi = g_scan[r].hi; break; }
    if (!blo) return false;

    const int shifts[3] = { 6, 4, 1 };
    g_fguard = 1;
    if (sigsetjmp(g_fjmp, 1)) { g_fguard = 0; return false; }
    {
        for (uintptr_t s = blo; s + 8 <= bhi; s += 8) {
            void** cand = (void**)s;
            if (!addr_readable((uintptr_t)cand[0])) continue;   // Blocks[0] must be readable
            for (int si = 0; si < 3; si++) {
                int shift = shifts[si], good = 0;
                char buf[128];
                for (int k = 0; k < ni; k++)
                    if (decode_name(cand, shift, idxs[k], buf, sizeof buf) && printable(buf)) good++;
                if (good >= ni - 2) {
                    g_name_blocks = cand; g_name_shift = shift;
                    char sample[128]; decode_name(cand, shift, idxs[0], sample, sizeof sample);
                    LOG("GNames Blocks[] @ %p (lib+0x%lx) shift=%d good=%d/%d e.g. '%s'",
                        (void*)cand, (uintptr_t)cand - g_base, shift, good, ni, sample);
                    return true;
                }
            }
        }
        g_fguard = 0;
    }
    return false;
}

// ===========================================================================
//  reflection
// ===========================================================================
static void* obj_class(void* o) { return o ? *(void**)((uint8_t*)o + UOBJ_CLASS_OFF) : nullptr; }
static void* struct_super(void* s) { return *(void**)((uint8_t*)s + USTRUCT_SUPER_OFF); }
static bool name_eq(void* o, const char* w) { char n[256]; obj_name(o, n, sizeof n); return !strcmp(n, w); }

static int32_t prop_offset(void* cls, const char* want) {
    for (void* s = cls; addr_readable((uintptr_t)s); s = struct_super(s)) {
        for (void* f = *(void**)((uint8_t*)s + USTRUCT_CHILDPROPS);
             addr_readable((uintptr_t)f); f = *(void**)((uint8_t*)f + FFIELD_NEXT_OFF)) {
            char n[256]; field_name(f, n, sizeof n);
            if (!strcmp(n, want)) return *(int32_t*)((uint8_t*)f + FPROP_OFFSET_OFF);
        }
        if (!struct_super(s)) break;
    }
    return -1;
}
static void* find_func(void* cls, const char* want) {
    for (void* s = cls; addr_readable((uintptr_t)s); s = struct_super(s)) {
        for (void* c = *(void**)((uint8_t*)s + USTRUCT_CHILDREN);
             addr_readable((uintptr_t)c); c = *(void**)((uint8_t*)c + UFIELD_NEXT_OFF)) {
            if (name_eq(c, want)) return c;
        }
        if (!struct_super(s)) break;
    }
    return nullptr;
}
static void* find_class(const char* want) {
    int32_t n = objects_num();
    for (int32_t i = 0; i < n; i++) {
        void* o = object_at(i);
        if (!o || !addr_readable((uintptr_t)o)) continue;
        if (!name_eq(o, want)) continue;
        void* c = obj_class(o);
        char cn[128]; obj_name(c, cn, sizeof cn);
        if (strstr(cn, "Class")) return o;
    }
    return nullptr;
}
// find a UScriptStruct by name (its class name contains "ScriptStruct") — used to resolve struct
// field offsets by name, so RPC param layouts never rely on hardcoded offsets.
static void* find_scriptstruct(const char* want) {
    int32_t n = objects_num();
    for (int32_t i = 0; i < n; i++) {
        void* o = object_at(i);
        if (!o || !addr_readable((uintptr_t)o) || !name_eq(o, want)) continue;
        char cn[64]; obj_name(obj_class(o), cn, sizeof cn);
        if (strstr(cn, "ScriptStruct")) return o;
    }
    return nullptr;
}
static bool is_a(void* o, void* target) {
    for (void* c = obj_class(o); addr_readable((uintptr_t)c); c = struct_super(c)) {
        if (c == target) return true;
        if (!struct_super(c)) break;
    }
    return false;
}
static void* rd_obj(void* o, int32_t off) { return off < 0 ? nullptr : *(void**)((uint8_t*)o + off); }
static bool  rd_bool(void* o, int32_t off){ return off < 0 ? false : (*(uint8_t*)((uint8_t*)o + off) != 0); }

// ===========================================================================
//  ProcessEvent locator — xref to the ScriptCore.cpp path string
// ===========================================================================
static uintptr_t find_string_va(const char* needle) {
    // the string is in libUnreal's rodata — search only within the lib mapping
    size_t nl = strlen(needle);
    for (int r = 0; r < g_nread; r++) {
        uintptr_t lo = g_read[r].lo, hi = g_read[r].hi;
        if (lo < g_base || hi > g_lib_hi) continue;
        GUARD_REGION_ELSE_SKIP
        for (uintptr_t a = lo; a + 64 < hi; a++) {
            if (*(char*)a == needle[0] && !memcmp((void*)a, needle, nl)) { g_fguard = 0; return a; }
        }
        g_fguard = 0;
    }
    return 0;
}
// walk back from an instruction to the enclosing function prologue: stp x29,x30,[sp,#-imm]!
static uintptr_t prologue_before(uintptr_t pc) {
    for (uintptr_t b = pc; b > pc - 0x6000 && b >= g_text_lo; b -= 4) {
        uint32_t p = *(uint32_t*)b;
        if ((p & 0xffc00000u) == 0xa9800000u) {          // STP pre-index, 64-bit
            uint32_t rt = p & 0x1f, rt2 = (p >> 10) & 0x1f, rn = (p >> 5) & 0x1f;
            if (rt == 29 && rt2 == 30 && rn == 31) return b;
        }
        if (p == 0xd503233fu || p == 0xd503237fu) return b;   // paciasp / pacibsp prologue
    }
    return 0;
}
// does this function pointer sit in a live UObject's vtable? (ProcessEvent does; other
// ScriptCore functions like CallFunction do not) — the unambiguous disambiguator.
static bool in_object_vtable(uintptr_t fn, int* out_index) {
    int checked = 0;
    for (int32_t i = 0; i < objects_num() && checked < 40; i++) {
        void* o = object_at(i);
        if (!o || !addr_readable((uintptr_t)o)) continue;
        uintptr_t vt = *(uintptr_t*)o;
        if (!in_lib(vt) || !addr_readable(vt)) continue;
        checked++;
        for (int j = 0; j < 220; j++) {
            uintptr_t slot = vt + (uintptr_t)j * 8;
            if (!addr_readable(slot)) break;
            if (*(uintptr_t*)slot == fn) { if (out_index) *out_index = j; return true; }
        }
    }
    return false;
}
// find the function that references `target` AND lives in an object vtable
static uintptr_t xref_func(uintptr_t target, const char* what) {
    uintptr_t tpage = target & ~0xfffULL; uint32_t tlow = target & 0xfff;
    int n_page = 0, n_pair = 0;
    uintptr_t last_pc = 0;
    for (uintptr_t pc = g_text_lo; pc + 8 <= g_text_hi; pc += 4) {
        uint32_t i1 = *(uint32_t*)pc;
        if ((i1 & 0x9f000000u) != 0x90000000u) continue;      // ADRP
        uint32_t rd = i1 & 0x1f;
        int64_t immlo = (i1 >> 29) & 3, immhi = (i1 >> 5) & 0x7ffff;
        int64_t imm = ((immhi << 2) | immlo); imm = (imm << 43) >> 43;
        uintptr_t page = (pc & ~0xfffULL) + (uintptr_t)(imm << 12);
        if (page != tpage) continue;
        n_page++;
        for (int k = 1; k <= 20 && pc + 4 * k + 4 <= g_text_hi; k++) {
            uint32_t i2 = *(uint32_t*)(pc + 4 * k);
            bool add = (i2 & 0xff800000u) == 0x91000000u && ((i2 >> 5) & 0x1f) == rd
                       && ((i2 >> 10) & 0xfff) == tlow;
            // LDR (unsigned imm, 64-bit): ldr xT,[xRn,#imm] with imm*8 low == tlow
            bool ldr = (i2 & 0xffc00000u) == 0xf9400000u && ((i2 >> 5) & 0x1f) == rd
                       && (((i2 >> 10) & 0xfff) * 8) == tlow;
            if (!add && !ldr) continue;
            n_pair++; last_pc = pc;
            uintptr_t fn = prologue_before(pc);
            int idx = -1;
            if (fn && in_object_vtable(fn, &idx)) {
                LOG("%s @ %p (lib+0x%lx) vtable_idx=%d [xref pc=%p]",
                    what, (void*)fn, fn - g_base, idx, (void*)pc);
                return fn;
            }
            break;
        }
    }
    LOG("%s xref unresolved: page_hits=%d pair_hits=%d last_pc=%p", what, n_page, n_pair, (void*)last_pc);
    return 0;
}
static uintptr_t func_size(uintptr_t fn) {
    if (!in_text(fn)) return 0;
    for (uintptr_t p = fn + 8; p < fn + 0x8000 && p < g_text_hi; p += 4) {
        uint32_t w = *(uint32_t*)p;
        if ((w & 0xffc00000u) == 0xa9800000u) {          // next stp x29,x30 pre-index
            uint32_t rt = w & 0x1f, rt2 = (w >> 10) & 0x1f, rn = (w >> 5) & 0x1f;
            if (rt == 29 && rt2 == 30 && rn == 31) return p - fn;
        }
    }
    return 0;
}
// map the UObject vtable across many distinct classes; log slots shared by ~all classes whose
// target is a large .text function — ProcessEvent is the standout.
static void analyze_vtable() {
    uintptr_t vts[80]; void* cls[80]; int nvt = 0;
    for (int32_t i = 0; i < objects_num() && nvt < 80; i++) {
        void* o = object_at(i);
        if (!o || !addr_readable((uintptr_t)o)) continue;
        void* c = *(void**)((uint8_t*)o + UOBJ_CLASS_OFF);
        bool dup = false; for (int k = 0; k < nvt; k++) if (cls[k] == c) { dup = true; break; }
        if (dup) continue;
        uintptr_t vt = *(uintptr_t*)o;
        if (!in_lib(vt) || !addr_readable(vt)) continue;
        cls[nvt] = c; vts[nvt] = vt; nvt++;
    }
    LOG("vtable analysis over %d distinct classes:", nvt);
    for (int j = 0; j < 200; j++) {
        // modal pointer at slot j
        uintptr_t best = 0; int bestc = 0;
        for (int a = 0; a < nvt; a++) {
            uintptr_t va = *(uintptr_t*)(vts[a] + (uintptr_t)j * 8);
            if (!in_text(va)) continue;
            int c = 0; for (int b = 0; b < nvt; b++)
                if (*(uintptr_t*)(vts[b] + (uintptr_t)j * 8) == va) c++;
            if (c > bestc) { bestc = c; best = va; }
        }
        if (best && bestc * 100 / nvt >= 85) {
            uintptr_t sz = func_size(best);
            if (sz >= 200)                               // ProcessEvent is a big function
                LOG("  vidx=%d ptr=lib+0x%lx share=%d/%d size=0x%lx",
                    j, best - g_base, bestc, nvt, sz);
        }
    }
}
static void* find_object(const char* want) {
    int32_t n = objects_num();
    for (int32_t i = 0; i < n; i++) {
        void* o = object_at(i);
        if (o && addr_readable((uintptr_t)o) && name_eq(o, want)) return o;
    }
    return nullptr;
}
// ProcessEvent allocs its param frame with alloca(Function->PropertiesSize) -> a `sub Xd, sp, Xm`
// (subtract from sp by a REGISTER). Normal functions only do `sub sp,sp,#imm`. Unique, no execution.
static bool has_dynamic_sp_sub(uintptr_t f, uintptr_t sz) {
    for (uintptr_t p = f; p < f + sz; p += 4) {
        uint32_t i = *(uint32_t*)p;
        // SUB (shifted register), 64-bit: 0xCB000000; Rn == 31 (sp) is the alloca marker
        if ((i & 0xFF200000u) == 0xCB000000u && ((i >> 5) & 0x1f) == 31) return true;
    }
    return false;
}
// pick ProcessEvent statically: the unique, shared, mid/large UObject virtual whose body allocas off sp
static uintptr_t find_pe_by_sig() {
    void* any = nullptr;
    for (int32_t i = 0; i < objects_num() && !any; i++) {
        void* o = object_at(i);
        if (o && addr_readable((uintptr_t)o) && in_lib(*(uintptr_t*)o)) any = o;
    }
    if (!any) return 0;
    uintptr_t vt = *(uintptr_t*)any;
    uintptr_t cand[16]; int ncand = 0;
    for (int j = 5; j < 160 && ncand < 16; j++) {
        uintptr_t f = *(uintptr_t*)(vt + (uintptr_t)j * 8);
        if (!in_text(f)) continue;
        uintptr_t sz = func_size(f);
        if (sz < 0x200 || sz > 0x1400) continue;
        int occ = 0; for (int k = 0; k < 160; k++) if (*(uintptr_t*)(vt + (uintptr_t)k * 8) == f) occ++;
        if (occ > 2) continue;                           // stub/default virtual
        if (!has_dynamic_sp_sub(f, sz)) continue;
        bool dup = false; for (int c = 0; c < ncand; c++) if (cand[c] == f) dup = true;
        if (dup) continue;
        LOG("PE candidate: vidx=%d lib+0x%lx size=0x%lx", j, f - g_base, sz);
        cand[ncand++] = f;
    }
    if (ncand == 0) { LOG("PE sig: no alloca-based virtual found"); return 0; }
    LOG("ProcessEvent (sig) @ lib+0x%lx  (%d alloca candidate(s))", cand[0] - g_base, ncand);
    return cand[0];   // no execution — static pick only
}
static uintptr_t find_processevent() {
    g_scriptcore_va = find_string_va("/CoreUObject/Private/UObject/ScriptCore.cpp");
    if (g_scriptcore_va) {
        uintptr_t pe = xref_func(g_scriptcore_va, "ProcessEvent");
        if (pe) return pe;
    }
    return find_pe_by_sig();
}

// ===========================================================================
//  arm64 inline hook of ProcessEvent  (16-byte abs-branch patch + trampoline)
// ===========================================================================
static void* g_tramp = nullptr;
static void handler(void* obj, void* func, void* params);   // fwd

// ---- 3D text menu: resolve the spawn/text/viewpoint API and dump signatures once ----
static void* cdo_GStatics = nullptr, *fn_BeginSpawn = nullptr, *fn_FinishSpawn = nullptr;
static void* c_TextActor = nullptr, *fn_SetText = nullptr, *fn_GetViewPoint = nullptr;
static void dump_params(const char* tag, void* fn) {
    if (!fn) { LOG("  %s = null", tag); return; }
    LOG("  %s params:", tag);
    for (void* p = *(void**)((uint8_t*)fn + USTRUCT_CHILDPROPS); addr_readable((uintptr_t)p);
         p = *(void**)((uint8_t*)p + FFIELD_NEXT_OFF)) {
        char pn[48]; field_name(p, pn, sizeof pn);
        void* pc = *(void**)((uint8_t*)p + 0x8); char pcn[40] = "?";
        if (addr_readable((uintptr_t)pc)) { int32_t ni = *(int32_t*)pc; fname_to_str(ni, pcn, sizeof pcn); }
        LOG("    %s : %s @ %d", pn, pcn, *(int32_t*)((uint8_t*)p + FPROP_OFFSET_OFF));
    }
}
static void* fn_Conv = nullptr, *cdo_TextLib = nullptr, *fn_SetActorLoc = nullptr;
static void* fn_SetWorldSize = nullptr, *fn_SetColor = nullptr;
static void* fn_CompLoc = nullptr, *fn_CompRot = nullptr;
static int32_t o_TextRender = -1, o_Camera = -1;
static void* g_textactor = nullptr, *g_textcomp = nullptr;
static void menu_resolve() {
    static bool done = false; if (done) return; done = true;
    void* gs = find_class("GameplayStatics");
    cdo_GStatics = find_object("Default__GameplayStatics");
    fn_BeginSpawn = gs ? find_func(gs, "BeginDeferredActorSpawnFromClass") : nullptr;
    fn_FinishSpawn = gs ? find_func(gs, "FinishSpawningActor") : nullptr;
    c_TextActor = find_class("TextRenderActor");
    void* trc = find_class("TextRenderComponent");
    fn_SetText = trc ? find_func(trc, "K2_SetText") : nullptr;
    if (!fn_SetText && trc) fn_SetText = find_func(trc, "SetText");
    o_TextRender = c_TextActor ? prop_offset(c_TextActor, "TextRender") : -1;
    void* tl = find_class("KismetTextLibrary");
    cdo_TextLib = find_object("Default__KismetTextLibrary");
    fn_Conv = tl ? find_func(tl, "Conv_StringToText") : nullptr;
    fn_SetActorLoc = c_TextActor ? find_func(c_TextActor, "K2_SetActorLocation") : nullptr;
    fn_SetWorldSize = trc ? find_func(trc, "SetWorldSize") : nullptr;
    fn_SetColor = trc ? find_func(trc, "SetTextRenderColor") : nullptr;
    LOG("MENU: Begin=%p Finish=%p TextActor=%p SetText=%p Conv=%p WorldSize=%p Color=%p SetLoc=%p",
        fn_BeginSpawn, fn_FinishSpawn, c_TextActor, fn_SetText, fn_Conv, fn_SetWorldSize, fn_SetColor, fn_SetActorLoc);
}

static bool is_relocatable_verbatim(uint32_t in) {
    if ((in & 0x9f000000u) == 0x90000000u) return false;  // ADRP
    if ((in & 0x9f000000u) == 0x10000000u) return false;  // ADR
    if ((in & 0x7c000000u) == 0x14000000u) return false;  // B / BL
    if ((in & 0xff000010u) == 0x54000000u) return false;  // B.cond
    if ((in & 0x7e000000u) == 0x34000000u) return false;  // CBZ/CBNZ
    if ((in & 0x7e000000u) == 0x36000000u) return false;  // TBZ/TBNZ
    if ((in & 0x3b000000u) == 0x18000000u) return false;  // LDR literal
    return true;                                          // stp/mov/sub etc. — safe to copy
}
static bool inline_hook(uintptr_t fn) {
    g_pe_rel = in_lib(fn) ? fn - g_base : 0;   // record for the SDK dump's Offsets.hpp
    uint32_t* src = (uint32_t*)fn;
    for (int i = 0; i < 4; i++)
        if (!is_relocatable_verbatim(src[i])) {
            LOG("PE prologue has PC-relative instr at +%d (0x%08x) — aborting hook", i*4, src[i]);
            return false;
        }
    // trampoline: [4 original instrs][ldr x17,#8; br x17; .quad fn+16]
    uint8_t* t = (uint8_t*)mmap(nullptr, 64, PROT_READ | PROT_WRITE | PROT_EXEC,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (t == MAP_FAILED) { LOG("tramp mmap failed"); return false; }
    memcpy(t, src, 16);
    uint32_t* w = (uint32_t*)(t + 16);
    w[0] = 0x58000051;                 // LDR x17, #8
    w[1] = 0xd61f0220;                 // BR  x17
    *(uint64_t*)(t + 24) = fn + 16;
    __builtin___clear_cache((char*)t, (char*)t + 64);
    g_tramp = t;
    g_ProcessEvent = (PFN_PE)t;

    // patch fn: ldr x17,#8; br x17; .quad handler
    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t page = fn & ~(uintptr_t)(ps - 1);
    if (mprotect((void*)page, ps * 2, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        LOG("mprotect PE failed"); return false;
    }
    uint32_t* d = (uint32_t*)fn;
    d[0] = 0x58000051;                 // LDR x17, #8
    d[1] = 0xd61f0220;                 // BR  x17
    *(uint64_t*)(fn + 8) = (uint64_t)&handler;
    __builtin___clear_cache((char*)fn, (char*)fn + 16);
    LOG("ProcessEvent hooked (tramp=%p)", t);
    return true;
}

// ===========================================================================
//  chams pass  (runs inside the PE handler => game thread, throttled)
// ===========================================================================
static bool  g_ready = false;
static void* c_PavlovPawn = nullptr;
static void* c_PawnBase = nullptr;
static void* fn_IsDead = nullptr;
static void* fn_SetMaterial = nullptr;
static void* fn_CreateMID = nullptr;   // MeshComponent::CreateDynamicMaterialInstance (custom cham colors)
static void* fn_SetVecParam = nullptr; // MaterialInstanceDynamic::SetVectorParameterValue
static int32_t g_colorNameId = -1;     // FName id of "Color" (the xray material's tint param)
static void* fn_GetMaterial = nullptr;                       // UMeshComponent::GetMaterial (for ESP restore)
static void* g_chamMesh[256]; static void* g_chamOrig[256]; static int g_nCham = 0;   // orig material cache
static void* g_midMesh[256]; static void* g_midObj[256]; static int g_nMid = 0;        // per-mesh custom-color MID cache
static void* mid_for(void* mesh) { for (int i=0;i<g_nMid;i++) if (g_midMesh[i]==mesh) {
    void* m=g_midObj[i]; return (m && addr_readable((uintptr_t)m) && in_lib(*(uintptr_t*)m)) ? m : nullptr; } return nullptr; }
static void mid_store(void* mesh, void* mid) { for (int i=0;i<g_nMid;i++) if (g_midMesh[i]==mesh) { g_midObj[i]=mid; return; }
    if (g_nMid<256){ g_midMesh[g_nMid]=mesh; g_midObj[g_nMid]=mid; g_nMid++; } else { g_midMesh[0]=mesh; g_midObj[0]=mid; } }
static int32_t o_Avatar = -1, o_XRay0 = -1, o_XRay1 = -1, o_bValid = -1, o_TeamId = -1, o_bDead = -1;
static inline bool pawn_dead(void* o) { return o_bDead >= 0 && *(uint8_t*)((uint8_t*)o + o_bDead) != 0; }
static void* c_Mesh = nullptr;
static void* fn_SetOverlay = nullptr;
static void* c_Ghost = nullptr;     // the ACTUAL player body class in Shack (BP_PavlovGhost_C : GhostPawn)
static void* fn_GetXRay = nullptr;  // GhostPawn::GetXRayMaterial
static int32_t g_ghost_mat_off = -1;// offset on the ghost holding the xray material (found read-only)
static int32_t o_SkelAsset = -1;    // SkeletalMeshComponent::SkeletalMeshAsset (renderable check)
static void* fn_SetVis = nullptr;   // SceneComponent::SetVisibility (hide-test diagnostic)
static void* fn_SetHidden = nullptr;// AActor::SetActorHiddenInGame (anti-smoke)
static void* fn_GetItem = nullptr;  // PavlovPawn::GetItemOfClass
static void* c_Gun = nullptr;       // Gun_Base_C
static void* c_VRGun = nullptr;     // VRGun
static void* c_VRMagazine = nullptr;   // VRMagazine (ancestry match for infinite-ammo sweep)
static void* fn_AddMoveInput = nullptr; // APawn::AddMovementInput (smooth noclip vertical)
// silent-aim reflection
static void* fn_GetLoc = nullptr, *fn_GetRot = nullptr, *fn_SetRot = nullptr, *fn_SetLoc = nullptr;
static void* fn_SockLoc = nullptr, *fn_LookAt = nullptr, *cdo_KML = nullptr;
static int32_t o_AvatarSkin = -1, o_SkullSocket = -1;
static void* g_fire[16]; static int g_nfire = 0;    // gun fire UFunctions (fire-path hook)
static void* g_kick[8]; static int g_nkick = 0;     // kick RPCs to DROP (anti-votekick)
static void* g_authfn[24]; static const char* g_authnm[24]; static int g_nauthfn = 0;   // auth/voice RPCs to TRACE (log-only)
static void* fn_ChangeName = nullptr;               // ServerChangeName (name spoof for custom-map admin)
// voice injection (soundboard TX bypass): AVoiceRouter::ServerOnVoice(FPavlovVoicePacket{TArray<u8> Data})
// is the client-authoritative voice send — the packet Data is an Opus-encoded blob. We probe the live
// packet cadence/size first, then build+send our own. Resolved in resolve_names.
static void* c_VoiceRouter = nullptr; static void* fn_ServerOnVoice = nullptr;
static void* fn_ClientOnVoiceBunch = nullptr;   // receive-side: FVoicePacketBunch{Players@0x0, Packets@0x10}
// Voice TX proof (Phase 1): a ring of real incoming Opus frames (full Data incl. the 6-byte Pavlov
// header). On PLAY we replay them via ServerOnVoice (seq byte rewritten) to prove the transmit path.
#define VCAP_MAX 128
#define VCAP_LEN 512
static uint8_t g_vcap[VCAP_MAX][VCAP_LEN]; static int g_vcaplen[VCAP_MAX];
static int  g_vcap_w = 0, g_vcap_n = 0;        // ring write head + count
static bool g_vreplay = false; static int g_vreplay_i = 0; static long g_vreplay_ms = 0;
static uint8_t g_vseq = 0;
static void* fn_SetPlayerSkin = nullptr;            // PavlovPawn::SetPlayerSkin(FName) — player skin changer
// ---- voice chat (Quest fix) ----------------------------------------------------------------------
// The Frame build gates Android mic capture behind AndroidRuntimeSettings.bAndroidVoiceEnabled and
// never lights it up on Quest (VoiceDevice ends empty). We: force the flag on the settings CDO,
// unmute, (re)enable the lobby + net-voice, and kick CheckAndEnableVoiceCapture to re-init capture.
static void*   cdo_ARS         = nullptr; static int32_t o_AndroidVoice = -2;   // AndroidRuntimeSettings.bAndroidVoiceEnabled
static void*   g_gus           = nullptr; static int32_t o_MicMuted = -2, o_VoiceDev = -2;  // PavlovGameUserSettings
static void*   c_GUS           = nullptr; static void* fn_SetMicMuted = nullptr, *fn_SetVoiceDevice = nullptr;
static void*   fn_CheckEnVoice = nullptr;  // PavlovGameInstance::CheckAndEnableVoiceCapture()
static void*   fn_SetVoiceChat = nullptr, *fn_IsVoiceChatEn = nullptr;   // OnlineLobby
static void*   c_OnlineLobby   = nullptr;
static void*   fn_ClientEnNetVoice = nullptr;   // PlayerController::ClientEnableNetworkVoice(bool)
static void*   fn_ToggleSpeaking   = nullptr;   // PlayerController::ToggleSpeaking(bool) — start/stop broadcast
static int32_t o_Pawn_VoiceActive  = -2;        // PavlovPawn.bVoiceActive
static void*   cdo_PavStatics  = nullptr; static void* fn_GetVoiceLevelPeak = nullptr, *fn_ResetVoiceDev = nullptr;
static void*   fn_GetVoiceSource = nullptr;     // PavlovGameInstance::GetVoiceSource()
// VoiceSource — the actual capture object (GI.VoiceSource @0x348). Direct mic control.
static void*   c_VoiceSource = nullptr; static int32_t o_GI_VoiceSrc = -2;
static void*   fn_VS_SetCapture = nullptr, *fn_VS_IsCapturing = nullptr, *fn_VS_IsBroadcasting = nullptr, *fn_VS_ResetCap = nullptr;
// ---- trigger-kill (ported from the PC internal: APavlovPlayerController::ServerReportBulletHit) ----
static void* fn_ReportHit = nullptr;                // ServerReportBulletHit(FClientBulletHit)
static void* c_KillGun = nullptr, *c_KillBullet = nullptr;   // GunClass / BulletClass for the report
static int32_t o_HeadBone = -2;                     // PavlovPawn::HeadBoneName (FName)
// FClientBulletHit field offsets — defaults from the PC SDK, overridden by-name at runtime if the
// ScriptStruct is found (so we never rely on a guessed layout).
static int32_t bh_Target=0x00, bh_Hit=0x08, bh_Head=0x20, bh_Pen=0x21,
               bh_Bullet=0x28, bh_Gun=0x30, bh_Origin=0x38, bh_Bone=0x50, bh_Size=0x60;
static bool    bh_resolved = false;
// ---- ESP + TTT ----
static void*   fn_ServerBuy = nullptr;              // APavlovPlayerController::ServerBuy(FName)
static void*   fn_ServerGive = nullptr;             // APavlovPawn::ServerGive(FName Equipment, FName Skin) — free spawn
static int32_t o_PawnPS = -2, o_PS_Name = -2, o_PawnHC = -2, o_HC_H = -2, o_HC_MH = -2, o_PS_Role = -2;
static void* fn_GMChangeName = nullptr; static int32_t o_AuthGM = -1;   // GameModeBase::ChangeName (long names)
static char g_wantName[64] = {0};                   // desired name from name.txt (direct-write spoof)
static void* g_knife[6]; static int g_nknife = 0;   // knife throw UFunctions (homing-knife hook)
static void* fn_SetPhysVel = nullptr; static int32_t o_RootComp = -1;
static void* fn_SetCollision = nullptr;             // AActor::SetActorEnableCollision (noclip)
static void* g_thrownKnife = nullptr; static long g_throwMs = 0;
static void* g_drawhud[8]; static int g_ndrawhud = 0;  // ReceiveDrawHUD UFunctions (Canvas source)
static void* c_Canvas = nullptr; static void* fn_DrawText = nullptr;
static int g_cfg = 2;                                // cached config (set by chams_pass; read per-PE)
static void* g_bots[128]; static int g_nbots = 0;   // enemy pawns cached by chams_pass (aim reads this)
static void* g_aim_target = nullptr;                // current silent-aim target (highlighted)
static void* g_localMe    = nullptr;                // local pawn cached by chams_pass (aim reads this — no find_world at 90Hz)
static int32_t g_myteam = -1;                       // local player's team
struct FVec { double x, y, z; };
struct FRot { double pitch, yaw, roll; };
static FVec g_aim_targetHead{};                     // head pos of current aim target (for trigger-kill)
static FVec  aim_getloc(void* a) { struct { FVec r; } p{}; if (fn_GetLoc) g_ProcessEvent(a, fn_GetLoc, &p); return p.r; }
static FRot  aim_getrot(void* a) { struct { FRot r; } p{}; if (fn_GetRot) g_ProcessEvent(a, fn_GetRot, &p); return p.r; }
static void  aim_setrot(void* a, FRot r) { struct { FRot R; uint8_t tp, ret; } p{ r, 0, 0 }; if (fn_SetRot) g_ProcessEvent(a, fn_SetRot, &p); }
static FVec  aim_sockloc(void* mesh, uint64_t fn) { struct { uint64_t name; FVec r; } p{ fn, {} }; if (fn_SockLoc) g_ProcessEvent(mesh, fn_SockLoc, &p); return p.r; }
// K2_SetActorLocation(FVector NewLocation, bool bSweep, FHitResult SweepHitResult, bool bTeleport)->bool.
// The FHitResult out-param is large (~200B with UE5.1 doubles) so the buffer must be big enough or
// ProcessEvent smashes the stack. Resolve NewLocation's offset by reflection; zero everything else.
static int32_t o_NewLoc = -2;
static void  aim_setloc(void* a, FVec v) {
    if (!fn_SetLoc) return;
    if (o_NewLoc == -2) o_NewLoc = prop_offset(fn_SetLoc, "NewLocation");
    uint8_t p[512]; memset(p, 0, sizeof p);
    int off = (o_NewLoc >= 0) ? o_NewLoc : 0;
    if (off + (int)sizeof(FVec) <= (int)sizeof p) *(FVec*)(p + off) = v;
    g_ProcessEvent(a, fn_SetLoc, p);
}
// SetPhysicsLinearVelocity(FVector NewVel, bool bAddToCurrent, FName BoneName) — homing knife steering
static void  set_phys_vel(void* comp, FVec v) {
    if (!fn_SetPhysVel || !comp) return;
    uint8_t p[48]; memset(p, 0, sizeof p); *(FVec*)p = v;   // NewVel@0; rest (bAdd/BoneName) zeroed
    g_ProcessEvent(comp, fn_SetPhysVel, p);
}
// FindLookAtRotation is just atan2 math — compute it in C (no flaky UFunction/CDO dependency)
static FRot  look_at(FVec a, FVec b) {
    double dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    FRot r; r.yaw = atan2(dy, dx) * 57.29577951308232;
    r.pitch = atan2(dz, sqrt(dx * dx + dy * dy)) * 57.29577951308232; r.roll = 0;
    return r;
}
static double angnorm(double a) { while (a > 180) a -= 360; while (a < -180) a += 360; return a; }

// held gun via GetItemOfClass(ItemClass, false, true) -> AActor*
static void* get_item(void* pawn, void* cls) {
    if (!fn_GetItem || !cls || !pawn) return nullptr;
    struct { void* ItemClass; uint8_t b1; uint8_t b2; char pad[6]; void* Ret; }
        p{ cls, 0, 1, {0}, nullptr };
    g_ProcessEvent(pawn, fn_GetItem, &p);
    return p.Ret;
}
// ONLINE-ROBUST held gun. GetItemOfClass returns null online on this build, so fall back to the last
// gun we saw actually FIRE (captured in the handler fire-path — unambiguously the weapon in your hand).
static void* g_heldGun = nullptr;
static void* resolve_gun(void* pawn) {
    void* gun = get_item(pawn, c_Gun);
    if (!gun || !addr_readable((uintptr_t)gun)) gun = get_item(pawn, c_VRGun);
    if (gun && addr_readable((uintptr_t)gun) && in_lib(*(uintptr_t*)gun)) return gun;
    // fallback: the last-fired gun, re-validated (still a live VRGun in the engine)
    if (g_heldGun && addr_readable((uintptr_t)g_heldGun) && in_lib(*(uintptr_t*)g_heldGun)
        && c_VRGun && is_a(g_heldGun, c_VRGun)) return g_heldGun;
    return nullptr;
}
// zero the four recoil/spread floats on the local held gun (offsets by name, per gun class)
// magazine ammo offsets (VRMagazine::Bullets/MaxBullets), resolved once from a live mag's class
static int32_t g_magBul = -1, g_magMax = -1;
static int g_gunGen = 0;   // bumped by the menu "Refresh mods" action -> do_norecoil re-resolves gun offsets
// aggressive infinite ammo: cached so the handler can top up mag+chamber every PE (beats server depletion
// locally so an auto-fire gun never runs dry -> never needs the broken bolt-cock reload).
static void* g_ammoGun = nullptr; static int32_t g_ammoMagOff = -1, g_ammoBIC = -1;
static void keep_loaded() { /* infinite ammo removed (broke cocking) */ }
// movement (PavlovMovementComponent) — the pawn has no reflected pointer to it, so find the
// instance whose Outer == our pawn. Cached per pawn (only re-scans on respawn/match change).
static int32_t g_mSprint = -1, g_mAds = -1, g_mWalk = -1, g_mCrouch = -1;
static void* g_walkComp = nullptr; static float g_walkOrig = 0.f, g_crouchOrig = 0.f;
static float g_mSprintOrig = -1.f, g_mAdsOrig = -1.f;   // originals (restore when movement disabled)
static void* g_mcPawn = nullptr; static void* g_mc = nullptr;
static void* g_myPS = nullptr; static int32_t g_devOff = -2;   // local PlayerState + bDev (self-view dev tag; -2=unresolved)
static void* g_hc = nullptr; static int32_t g_hcDmg = -2, g_hcH = -2, g_hcMH = -2;   // godmode (health comp)
static void keep_god() {   // no-damage + pin health; per-PE so a hit can't slip between passes (offline)
    if (!g_hc || !addr_readable((uintptr_t)g_hc) || !in_lib(*(uintptr_t*)g_hc)) return;
    if (g_hcDmg >= 0) *(float*)((uint8_t*)g_hc + g_hcDmg) = 0.f;              // DamageMultiplier
    if (g_hcH >= 0 && g_hcMH >= 0) { float mx = *(float*)((uint8_t*)g_hc + g_hcMH);
        if (mx > 0.f) *(float*)((uint8_t*)g_hc + g_hcH) = mx; }               // Health = MaxHealth
}
static void* find_movecomp(void* pawn) {
    if (pawn == g_mcPawn && g_mc && addr_readable((uintptr_t)g_mc)) return g_mc;
    // The scan below walks the whole object array. On death/respawn the cache misses, so without a
    // throttle chams_pass (10Hz) re-scans every tick -> post-death lag. Cap the rescan to ~3Hz.
    static long last_scan = 0;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    long ms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (ms - last_scan < 350) return g_mc;   // between attempts: keep last (may be null), don't scan
    last_scan = ms;
    static void* mcClass = nullptr;
    if (!mcClass) mcClass = find_class("PavlovMovementComponent");
    g_mcPawn = pawn; g_mc = nullptr;
    if (!mcClass) return nullptr;
    int32_t n = objects_num();
    for (int32_t i = 0; i < n; i++) {
        void* o = object_at(i);
        if (!o || !addr_readable((uintptr_t)o) || !in_lib(*(uintptr_t*)o)) continue;
        if (obj_class(o) != mcClass) continue;
        void* outer = *(void**)((uint8_t*)o + 0x20);        // UObject::Outer (owning actor)
        if (outer == pawn) { g_mc = o; return o; }
    }
    return nullptr;
}
// Apply movement speeds. Called per-PE from the handler so ours win over the game's per-frame writes
// (esp. the ADS slowdown). Lower normal walk, high ADS (fast while aiming). Walk = all directions.
static void apply_speed() {
    void* mc = g_mc;
    if (!mc || !addr_readable((uintptr_t)mc) || !in_lib(*(uintptr_t*)mc)) return;
    if (g_mei.master_enabled && g_mei.move_enabled) {             // ON: cheat speeds
        if (g_mSprint >= 0) *(float*)((uint8_t*)mc + g_mSprint) = g_mei.move_sprint;
        if (g_mAds    >= 0) *(float*)((uint8_t*)mc + g_mAds)    = g_mei.move_ads;
        if (g_mWalk   >= 0 && g_walkOrig   > 0.f) *(float*)((uint8_t*)mc + g_mWalk)   = g_walkOrig   * g_mei.move_walk;
        if (g_mCrouch >= 0 && g_crouchOrig > 0.f) *(float*)((uint8_t*)mc + g_mCrouch) = g_crouchOrig * g_mei.move_crouch;
    } else {                                                      // OFF: restore captured originals
        if (g_mSprint >= 0 && g_mSprintOrig >= 0.f) *(float*)((uint8_t*)mc + g_mSprint) = g_mSprintOrig;
        if (g_mAds    >= 0 && g_mAdsOrig    >= 0.f) *(float*)((uint8_t*)mc + g_mAds)    = g_mAdsOrig;
        if (g_mWalk   >= 0 && g_walkOrig    >  0.f) *(float*)((uint8_t*)mc + g_mWalk)   = g_walkOrig;
        if (g_mCrouch >= 0 && g_crouchOrig  >  0.f) *(float*)((uint8_t*)mc + g_mCrouch) = g_crouchOrig;
    }
}
// resolve the "Automatic/FullAuto" value of the gun FireMode enum (UEnum Names@0x40 num@0x48). Cached.
static int g_fireAuto = -999;
static int fire_auto_value() {
    if (g_fireAuto != -999) return g_fireAuto;
    int32_t n = objects_num();
    for (int i = 0; i < n; i++) { void* o = object_at(i);
        if (!o || !addr_readable((uintptr_t)o)) continue;
        char cn[24]; obj_name(obj_class(o), cn, sizeof cn); if (strcmp(cn, "Enum")) continue;
        char nm[48]; obj_name(o, nm, sizeof nm); if (!strstr(nm, "FireMode")) continue;
        void* data = *(void**)((uint8_t*)o + 0x40); int num = *(int32_t*)((uint8_t*)o + 0x48);
        if (!addr_readable((uintptr_t)data)) break;
        for (int j = 0; j < num && j < 64; j++) {
            char en[64]; fname_to_str(*(int32_t*)((uint8_t*)data + j*16), en, sizeof en);
            if (strstr(en, "Auto") || strstr(en, "Full")) {
                g_fireAuto = (int)*(int64_t*)((uint8_t*)data + j*16 + 8);
                LOG("fireauto: '%s' = %d", en, g_fireAuto); return g_fireAuto; }
        }
        LOG("fireauto: enum '%s' has no Auto member", nm);
    }
    g_fireAuto = -1; return -1;
}
// helpers: write a float/byte at offset, and restore an original value (both guard off<0).
static inline void wf(void* o, int32_t off, float v)  { if (off >= 0) *(float*)((uint8_t*)o + off) = v; }
static inline void wb(void* o, int32_t off, uint8_t v){ if (off >= 0) *(uint8_t*)((uint8_t*)o + off) = v; }
static void do_norecoil(void* pawn) {
    static void* rcls[48]; static int32_t rs[48], rt[48], rm[48], ra[48];
    static int32_t ofr[48], ofd[48], obd[48];                    // FireRate/FireDelay/BurstDelay (rapid)
    static int32_t olsm[48], orpb[48], orc[48], omrc[48], obic[48], omag[48];
    static int32_t ofm[48], odc[48], obl[48], ocl[48], olb[48], obta[48];   // auto-fire: FireMode + bolt/cock
    // extra recoil drivers on VRGun that some guns actually use (base fields the old set missed)
    static const char* REX_NAMES[6] = { "RecoilRatio", "RecoilCurveScale", "PivotRecoilMul",
                                        "PivotRecoilLateralMul", "RecoilAngleLateralMul", "DynamicPivotRecoil" };
    static int32_t rex[48][6];
    // ORIGINAL values captured on first sighting (unmodified gun) -> restored when a feature is OFF.
    static float o_rs[48], o_olsm[48], o_rt[48], o_rm[48], o_ra[48], o_orpb[48], o_ofr[48], o_ofd[48], o_obd[48], o_orc[48], o_omrc[48];
    static uint8_t o_ofm[48];
    static int rn = 0;
    static int myGen = 0; if (myGen != g_gunGen) { rn = 0; myGen = g_gunGen; }   // "Refresh mods" -> re-resolve
    void* gun = resolve_gun(pawn);   // online-robust (get_item or last-fired gun fallback)
    if (!gun || !addr_readable((uintptr_t)gun) || !in_lib(*(uintptr_t*)gun)) return;
    void* gc = obj_class(gun);
    int ci = 0; for (; ci < rn; ci++) if (rcls[ci] == gc) break;
    if (ci == rn && rn < 48) { rcls[rn] = gc;
        rs[rn]  = prop_offset(gc, "BulletSpraySpread");
        rt[rn]  = prop_offset(gc, "RecoilTraslationMul");
        rm[rn]  = prop_offset(gc, "RecoilMul");
        ra[rn]  = prop_offset(gc, "RecoilAngleMul");
        ofr[rn] = prop_offset(gc, "FireRate");     // rapid fire: config intervals (from SDK dump)
        ofd[rn] = prop_offset(gc, "FireDelay");
        obd[rn] = prop_offset(gc, "BurstDelay");
        olsm[rn]= prop_offset(gc, "LimitedBulletSpraySpreadMul");  // accuracy
        orpb[rn]= prop_offset(gc, "RecoilPerBullet");
        orc[rn] = prop_offset(gc, "ReloadCooldown");              // no-reload
        omrc[rn]= prop_offset(gc, "ManualReloadCooldown");
        obic[rn]= prop_offset(gc, "bBulletInChamber");
        omag[rn]= prop_offset(gc, "Magazine");                    // -> VRMagazine (infinite ammo)
        ofm[rn] = prop_offset(gc, "FireMode");                    // auto-fire (incl. snipers)
        odc[rn] = prop_offset(gc, "bDisableCocking");
        obl[rn] = prop_offset(gc, "bBoltLocked");
        ocl[rn] = prop_offset(gc, "bChargingLocked");
        olb[rn] = prop_offset(gc, "bLockBolt");
        obta[rn]= prop_offset(gc, "bBoltLockedTillAmmo");
        for (int e = 0; e < 6; e++) rex[rn][e] = prop_offset(gc, REX_NAMES[e]);   // extra recoil drivers
        // snapshot originals from the (still-unmodified) instance so we can restore on disable
        #define SNAPF(o,off) o[rn] = (off >= 0) ? *(float*)((uint8_t*)gun + off) : 0.f
        SNAPF(o_rs,rs[rn]); SNAPF(o_olsm,olsm[rn]); SNAPF(o_rt,rt[rn]); SNAPF(o_rm,rm[rn]);
        SNAPF(o_ra,ra[rn]); SNAPF(o_orpb,orpb[rn]); SNAPF(o_ofr,ofr[rn]); SNAPF(o_ofd,ofd[rn]);
        SNAPF(o_obd,obd[rn]); SNAPF(o_orc,orc[rn]); SNAPF(o_omrc,omrc[rn]);
        o_ofm[rn] = (ofm[rn] >= 0) ? *(uint8_t*)((uint8_t*)gun + ofm[rn]) : 0;
        #undef SNAPF
        char cn[48]; obj_name(gc, cn, sizeof cn);
        LOG("gun '%s' spread@%d rc@%d mrc@%d bic@%d mag@%d rpb@%d (orig captured)",
            cn, rs[rn], orc[rn], omrc[rn], obic[rn], omag[rn], orpb[rn]);
        ci = rn++; }
    if (ci >= 48) return;
    // effective flags: master AND feature. When master (or the feature) is OFF -> restore originals.
    const bool M = g_mei.master_enabled;
    const bool f_acc = M && g_mei.perfect_accuracy, f_rec = M && g_mei.no_recoil;
    const bool f_rapid = M && g_mei.rapid_fire, f_auto = M && g_mei.force_auto, f_nrl = M && g_mei.no_reload;
    // perfect accuracy: zero the spread floats when ON, restore originals when OFF
    if (f_acc) { wf(gun, rs[ci], 0.f); wf(gun, olsm[ci], 0.f); }
    else       { wf(gun, rs[ci], o_rs[ci]); wf(gun, olsm[ci], o_olsm[ci]); }
    // no recoil: zero the recoil floats when ON, restore when OFF
    if (f_rec) { wf(gun, rt[ci],0.f); wf(gun, rm[ci],0.f); wf(gun, ra[ci],0.f); wf(gun, orpb[ci],0.f);
                 for (int e = 0; e < 6; e++) wf(gun, rex[ci][e], 0.f); }   // extra drivers some guns use
    else       { wf(gun, rt[ci],o_rt[ci]); wf(gun, rm[ci],o_rm[ci]); wf(gun, ra[ci],o_ra[ci]); wf(gun, orpb[ci],o_orpb[ci]); }
    g_ammoGun = nullptr;   // re-published below if rapid/auto is on
    {
        // rapid fire: shrink the intervals when ON, restore captured originals when OFF
        if (f_rapid) {
            wf(gun, ofd[ci], 0.02f); wf(gun, obd[ci], 0.02f);
            if (ofr[ci] >= 0) { float* fr = (float*)((uint8_t*)gun + ofr[ci]);
                if (*fr > 0.005f && *fr < 5.f) *fr = 0.03f;   // seconds/shot -> tiny (faster)
                else if (*fr >= 5.f) *fr = 1200.f;            // rounds/min -> high (faster)
            }
        } else { wf(gun, ofd[ci], o_ofd[ci]); wf(gun, obd[ci], o_obd[ci]); wf(gun, ofr[ci], o_ofr[ci]); }
        // auto-fire: force FireMode to Automatic when ON. Apply-only (do NOT restore the byte enum, and
        // NEVER touch cocking/bolt) — the known-good build did exactly this; restoring FireMode from a
        // snapshot was a new write that could brick the gun's fire state on spawn.
        if (f_auto && ofm[ci] >= 0) {
            int av = fire_auto_value(); if (av >= 0) *(uint8_t*)((uint8_t*)gun + ofm[ci]) = (uint8_t)av; }
        (void)odc; (void)obl; (void)ocl; (void)olb; (void)obta; (void)obic; (void)omag; (void)g_magBul; (void)g_magMax;
        // no-reload: zero cooldowns when ON, restore originals when OFF
        if (f_nrl) { wf(gun, orc[ci], 0.f); wf(gun, omrc[ci], 0.f); }
        else       { wf(gun, orc[ci], o_orc[ci]); wf(gun, omrc[ci], o_omrc[ci]); }
        // INFINITE AMMO (opt-in "try"): top up Magazine->Bullets to MaxBullets. The old unconditional write
        // crashed on join (deref of a half-initialised magazine during spawn), so gate it HARD: only for the
        // gun we've actually seen fire (== g_heldGun -> stable, past spawn), a fully-validated magazine, and
        // only when Bullets is a sane value below MaxBullets. Server-authoritative online (offline/custom best).
        g_ammoGun = nullptr;
        if (M && g_mei.infinite_ammo && omag[ci] >= 0 && gun == g_heldGun) {
            void* mag = *(void**)((uint8_t*)gun + omag[ci]);
            if (mag && addr_readable((uintptr_t)mag) && in_lib(*(uintptr_t*)mag)) {
                char mcn[48]; obj_name(obj_class(mag), mcn, sizeof mcn);
                if (strstr(mcn, "Magazine")) {                       // confirm it's really a magazine object
                    if (g_magBul < 0) { void* mgc = obj_class(mag);
                        g_magBul = prop_offset(mgc, "Bullets"); g_magMax = prop_offset(mgc, "MaxBullets"); }
                    if (g_magBul >= 0 && g_magMax >= 0) {
                        int32_t mx = *(int32_t*)((uint8_t*)mag + g_magMax);
                        int32_t cur = *(int32_t*)((uint8_t*)mag + g_magBul);
                        if (mx > 0 && mx < 10000 && cur >= 0 && cur < mx)
                            *(int32_t*)((uint8_t*)mag + g_magBul) = mx;   // top up only when below full
                    }
                }
            }
        }
    }
    // movement + godmode moved OUT to do_movement()/do_godmode() so they work with NO weapon held.
}

// MOVEMENT: apply speeds on the pawn's PavlovMovementComponent — runs on `me` regardless of a held gun.
static void do_movement(void* pawn) {
    void* mc = nullptr;
    bool wantMc = g_mei.master_enabled && (g_mei.move_enabled || g_mei.noclip);
    if (wantMc) mc = find_movecomp(pawn);   // scan throttled inside
    else if (g_mc && g_mcPawn == pawn && addr_readable((uintptr_t)g_mc) && in_lib(*(uintptr_t*)g_mc)) mc = g_mc;
    static int dbg = 0;
    if (!mc || !addr_readable((uintptr_t)mc) || !in_lib(*(uintptr_t*)mc)) {
        if (dbg++ < 12) LOG("move: NO movecomp (mc=%p pawn=%p move_enabled=%d)", mc, pawn, g_mei.move_enabled);
        return; }
    // NOCLIP: proper UE cheat-fly (smooth + online-safe per commit f1a210e). bCheatFlying + MOVE_Flying +
    // zero gravity = smooth 3D flight via the stick with no ground snapping / gravity fighting. Collision
    // off = pass through walls. PLUS friend's explicit RIGHT-STICK up/down for manual vertical control on
    // top of the flight. Capture originals on the enable EDGE (before the per-frame apply overwrites them).
    { void* mcc = obj_class(mc);
      static int32_t o_mm = -2, o_fly = -2, o_cheat = -2, o_grav = -2;
      if (o_mm == -2)    o_mm    = prop_offset(mcc, "MovementMode");
      if (o_fly == -2)   o_fly   = prop_offset(mcc, "MaxFlySpeed");
      if (o_cheat == -2) o_cheat = prop_offset(mcc, "bCheatFlying");
      if (o_grav == -2)  o_grav  = prop_offset(mcc, "GravityScale");
      static bool ncPrev = false; static float flyOrig = 0.f, gravOrig = 1.f;
      bool nc = g_mei.master_enabled && g_mei.noclip;
      if (nc != ncPrev) {                                    // toggle edge: capture / restore
          if (nc) { if (o_fly >= 0)  flyOrig  = *(float*)((uint8_t*)mc + o_fly);
                    if (o_grav >= 0) gravOrig = *(float*)((uint8_t*)mc + o_grav); }
          else {
              if (o_cheat >= 0) *(uint8_t*)((uint8_t*)mc + o_cheat) = 0;
              if (o_grav >= 0)  *(float*)((uint8_t*)mc + o_grav) = (gravOrig > 0.f ? gravOrig : 1.f);
              if (o_fly >= 0 && flyOrig > 0.f) *(float*)((uint8_t*)mc + o_fly) = flyOrig;
              if (o_mm >= 0)    *(uint8_t*)((uint8_t*)mc + o_mm) = 1;             // MOVE_Walking
          }
          if (fn_SetCollision) { struct { uint8_t on; char p[7]; } p{ (uint8_t)(nc?0:1), {0} }; g_ProcessEvent(pawn, fn_SetCollision, &p); }
          ncPrev = nc;
      }
      if (nc) {                                              // per-frame hold (idempotent same-value writes)
          if (o_mm >= 0)    *(uint8_t*)((uint8_t*)mc + o_mm) = 5;                 // MOVE_Flying
          if (o_cheat >= 0) *(uint8_t*)((uint8_t*)mc + o_cheat) = 1;              // bCheatFlying → smooth
          if (o_grav >= 0)  *(float*)((uint8_t*)mc + o_grav) = 0.f;               // kill gravity pull
          if (o_fly >= 0)   *(float*)((uint8_t*)mc + o_fly) = 800.f * g_mei.fly_speed;  // decoupled from walk mult
          // vertical is handled by Pavlov's OWN flying locomotion (fly where you aim/move) — do NOT inject
          // our own AddMovementInput/SetActorLocation here: do_movement runs per-ProcessEvent (dozens of
          // times/frame), so any per-call movement input accumulates and jitters. The mode writes above
          // are idempotent (same value) so they're safe to repeat; movement stays the native path.
      } }
    if (g_mSprint == -1) { void* mcc = obj_class(mc);
        g_mSprint = prop_offset(mcc, "SprintSpeedMultiplier");
        g_mAds    = prop_offset(mcc, "ADSSpeedMultiplier");
        g_mWalk   = prop_offset(mcc, "MaxWalkSpeed");
        g_mCrouch = prop_offset(mcc, "MaxWalkSpeedCrouched");
        char mcn[48]; obj_name(mcc, mcn, sizeof mcn);
        LOG("move: class '%s' sprint@%d ads@%d walk@%d crouch@%d", mcn, g_mSprint, g_mAds, g_mWalk, g_mCrouch); }
    if (mc != g_walkComp) {   // capture ORIGINALS once per component (for restore)
        g_walkComp = mc;
        // Only accept a BASELINE value — on respawn the component may still hold OUR cheat value
        // (e.g. 2940 = 600*4.9); capturing that as "orig" and multiplying again compounds the speed and
        // breaks movement after death. Pavlov baselines are ~600 walk / ~300 crouch, so reject anything
        // above a sane ceiling and keep the last good baseline (default 600/300 if never captured).
        if (g_mWalk   >= 0) { float w = *(float*)((uint8_t*)mc + g_mWalk);
            if (w > 1.f && w < 1000.f) g_walkOrig = w; else if (g_walkOrig <= 0.f) g_walkOrig = 600.f; }
        if (g_mCrouch >= 0) { float w = *(float*)((uint8_t*)mc + g_mCrouch);
            if (w > 1.f && w < 700.f)  g_crouchOrig = w; else if (g_crouchOrig <= 0.f) g_crouchOrig = 300.f; }
        if (g_mSprint >= 0) { float s = *(float*)((uint8_t*)mc + g_mSprint); if (s > 0.f && s < 3.f) g_mSprintOrig = s; }
        if (g_mAds    >= 0) { float s = *(float*)((uint8_t*)mc + g_mAds);    if (s > 0.f && s < 3.f) g_mAdsOrig    = s; }
        LOG("move: orig walk=%.1f crouch=%.1f sprint=%.2f ads=%.2f", g_walkOrig, g_crouchOrig, g_mSprintOrig, g_mAdsOrig); }
    apply_speed();   // cheat speeds if enabled, else restore originals
}
// GODMODE: pin health / zero damage on the pawn's HealthComponent — also weapon-independent.
static void do_godmode(void* pawn) {
    static int32_t o_hcp = -2; static float dmgOrig = -1.f;
    if (o_hcp == -2) o_hcp = prop_offset(obj_class(pawn), "HealthComponent");
    if (o_hcp < 0) { g_hc = nullptr; return; }
    void* hc = *(void**)((uint8_t*)pawn + o_hcp);
    if (!hc || !addr_readable((uintptr_t)hc) || !in_lib(*(uintptr_t*)hc)) { g_hc = nullptr; return; }
    if (g_hcDmg == -2) { void* c = obj_class(hc);
        g_hcDmg = prop_offset(c, "DamageMultiplier"); g_hcH = prop_offset(c, "Health"); g_hcMH = prop_offset(c, "MaxHealth");
        if (g_hcDmg >= 0) dmgOrig = *(float*)((uint8_t*)hc + g_hcDmg); }
    if (g_mei.master_enabled && g_mei.godmode) { g_hc = hc; keep_god(); }
    else { g_hc = nullptr; if (g_hcDmg >= 0) *(float*)((uint8_t*)hc + g_hcDmg) = (dmgOrig >= 0.f ? dmgOrig : 1.f); }
}
// INFINITE AMMO (friend's "force every magazine BP to 10k"): instead of topping up only the held gun,
// sweep GObjects and pin MaxBullets + Bullets high on EVERY *Magazine* object — this hits both class
// default objects (so new mags spawn at 10k) and every live instance. Per-class cache of the magazine
// verdict + Bullets/MaxBullets offsets (no FName decode per object). Sanity-check the current value so
// a half-initialised magazine during spawn is skipped (that was the old crash). Fault-guarded by caller.
// Server-authoritative online -> best on offline/custom/community; official servers may re-cap.
#define INF_AMMO 9999
static void* g_magClsC[512]; static int8_t g_magClsB[512];
static int32_t g_magClsBul[512], g_magClsMax[512]; static int g_magClsN = 0;
static bool cls_mag_offsets(void* cls, int32_t* bulOff, int32_t* maxOff) {
    if (!addr_readable((uintptr_t)cls)) return false;
    for (int i = 0; i < g_magClsN; i++) if (g_magClsC[i] == cls) {
        *bulOff = g_magClsBul[i]; *maxOff = g_magClsMax[i]; return g_magClsB[i]; }
    // ANCESTRY match: any subclass of VRMagazine (per-gun mag BPs, stripper clips, etc.) — a plain
    // name-contains-"Magazine" check missed the guns whose mag class is named differently.
    bool b = false;
    if (c_VRMagazine) { for (void* k = cls; addr_readable((uintptr_t)k); k = struct_super(k)) {
        if (k == c_VRMagazine) { b = true; break; } if (!struct_super(k)) break; } }
    if (!b) { char c[64]; obj_name(cls, c, sizeof c); b = strstr(c, "Magazine") != nullptr; }  // fallback
    int32_t bo = -1, mo = -1;
    if (b) { bo = prop_offset(cls, "Bullets"); mo = prop_offset(cls, "MaxBullets");
             b = (bo >= 0 && mo >= 0); }             // only treat as usable if both fields resolve
    if (g_magClsN < 512) { g_magClsC[g_magClsN] = cls; g_magClsB[g_magClsN] = b;
        g_magClsBul[g_magClsN] = bo; g_magClsMax[g_magClsN] = mo; g_magClsN++; }
    *bulOff = bo; *maxOff = mo; return b;
}
static void do_infammo() {
    if (!(g_mei.master_enabled && g_mei.infinite_ammo)) return;
    static long last = 0; struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    long ms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (ms - last < 200) return; last = ms;          // ~5Hz full sweep
    int32_t n = objects_num();
    for (int i = 0; i < n; i++) { void* o = object_at(i);
        if (!o || !addr_readable((uintptr_t)o) || !in_lib(*(uintptr_t*)o)) continue;
        int32_t bo, mo; if (!cls_mag_offsets(obj_class(o), &bo, &mo)) continue;
        int32_t* mx = (int32_t*)((uint8_t*)o + mo);
        int32_t* cur = (int32_t*)((uint8_t*)o + bo);
        if (!addr_readable((uintptr_t)mx) || !addr_readable((uintptr_t)cur)) continue;
        if (*mx <= 0 || *mx > 1000000) continue;      // garbage / half-init -> skip (old crash guard)
        if (*mx  != INF_AMMO) *mx  = INF_AMMO;
        if (*cur != INF_AMMO) *cur = INF_AMMO;        // keep it topped so it never counts to reload
    }
}
// ANTI-FLASH / SMOKE: GlobalPlayerEffects drives the flash/smoke blind via an opacity curve sampled at
// `Time`. Pin Time past the curve's end so the blind opacity reads ~0 -> screen stays clear.
// pointer-cached "is this a smoke actor/component" verdict (anti-smoke)
static void* g_smkC[256]; static uint8_t g_smkB[256]; static int g_smkN = 0;
static bool cls_is_smoke(void* cls) {
    if (!addr_readable((uintptr_t)cls)) return false;
    for (int i = 0; i < g_smkN; i++) if (g_smkC[i] == cls) return g_smkB[i];
    char c[64]; obj_name(cls, c, sizeof c);
    bool b = (strstr(c, "Smoke") || strstr(c, "smoke")) && !strstr(c, "Grenade") && !strstr(c, "Commandlet");
    if (g_smkN < 256) { g_smkC[g_smkN] = cls; g_smkB[g_smkN] = b; g_smkN++; }
    return b;
}
static void do_antiflash() {
    if (!(g_mei.master_enabled && g_mei.anti_flash)) return;
    static void* c_gpe = nullptr; if (!c_gpe) c_gpe = find_class("GlobalPlayerEffects");
    static void* c_actor = nullptr; if (!c_actor) c_actor = find_class("Actor");
    if (!c_gpe) return;
    static int32_t o_dis = -2, o_fc = -2, o_sc = -2;
    if (o_dis == -2) o_dis = prop_offset(c_gpe, "bDisabled");
    if (o_fc  == -2) o_fc  = prop_offset(c_gpe, "FlashedOpacityCurve");
    if (o_sc  == -2) o_sc  = prop_offset(c_gpe, "TracerOpacityCurve");
    // The GlobalPlayerEffects instance keeps changing (transient/GC), so caching one leaves the ACTIVE
    // one un-zeroed -> the flash flickers in/out. Zero EVERY instance's opacity curve on a fast throttle.
    static long last = 0; struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    long ms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (ms - last < 300) return; last = ms;   // ~3Hz full sweep of all instances
    int32_t n = objects_num();
    for (int i = 0; i < n; i++) { void* o = object_at(i);
        if (!o || !addr_readable((uintptr_t)o) || !in_lib(*(uintptr_t*)o)) continue;
        void* oc = obj_class(o);
        // ANTI-SMOKE (best-effort): hide any live *Smoke*-named actor/component client-side. Note: on most
        // maps the smoke cloud is Niagara/render FX that isn't a reflectable object, so this only helps
        // where the smoke IS a named actor. The screen flash (above) is the reliable part.
        if (cls_is_smoke(oc)) {
            char snm[24]; obj_name(o, snm, sizeof snm); if (!strncmp(snm, "Default__", 9)) continue;
            g_fguard = 1; if (!sigsetjmp(g_fjmp, 1)) {
                if (c_actor && is_a(o, c_actor)) { if (fn_SetHidden) { struct { uint8_t b, p[7]; } p{1,{0}}; g_ProcessEvent(o, fn_SetHidden, &p); } }
                else if (fn_SetVis) { struct { uint8_t v, p; } vp{0,1}; g_ProcessEvent(o, fn_SetVis, &vp); }   // component
            } g_fguard = 0;
            continue;
        }
        if (!is_a(o, c_gpe)) continue;
        char nm[24]; obj_name(o, nm, sizeof nm); if (!strncmp(nm, "Default__", 9)) continue;
        if (o_dis >= 0) *(uint8_t*)((uint8_t*)o + o_dis) = 1;                 // bool kill-switch per instance
        int32_t offs[2] = { o_fc, o_sc };
        for (int ci = 0; ci < 2; ci++) { if (offs[ci] < 0) continue;
            void* curve = *(void**)((uint8_t*)o + offs[ci]);
            if (!curve || !addr_readable((uintptr_t)curve) || !in_lib(*(uintptr_t*)curve)) continue;
            uint8_t* keys = *(uint8_t**)((uint8_t*)curve + 0x30 + 0x70);      // FloatCurve.Keys.Data
            int32_t cnt   = *(int32_t*)((uint8_t*)curve + 0x30 + 0x70 + 8);   // .Num
            if (!keys || !addr_readable((uintptr_t)keys) || cnt <= 0 || cnt > 1000) continue;
            for (int k = 0; k < cnt; k++) { float* v = (float*)(keys + (size_t)k * 0x1c + 0x8);
                if (addr_readable((uintptr_t)v)) *v = 0.f; }
        }
    }
}// per-class verdict cache — decode each class name ONCE, then pointer-compare (kills per-frame decodes)
static void* g_clsC[256]; static uint8_t g_clsB[256]; static int g_clsN = 0;
// does `cls` derive from `base` (walk the super chain)?
static bool cls_derives(void* cls, void* base) {
    if (!base) return false;
    for (void* c = cls; addr_readable((uintptr_t)c); c = struct_super(c)) {
        if (c == base) return true;
        if (!struct_super(c)) break;
    }
    return false;
}
static bool cls_is_body(void* cls) {
    if (!addr_readable((uintptr_t)cls)) return false;
    for (int i = 0; i < g_clsN; i++) if (g_clsC[i] == cls) return g_clsB[i];
    char c[48]; obj_name(cls, c, sizeof c);
    // name-based (custom-skin bodies) OR structural: anything deriving from the PavlovPawn base catches
    // standard pawns in custom modes whose class name doesn't contain "Pavlov"/"Pawn" (no custom skin).
    bool isBody = (strstr(c,"Pavlov") && (strstr(c,"Pawn") || strstr(c,"Ghost"))) ||
                  strstr(c,"Hidden") || strstr(c,"Monster") || strstr(c,"Hide") || strstr(c,"Aurora") ||
                  cls_derives(cls, c_PavlovPawn) || cls_derives(cls, c_PawnBase);
    bool b = isBody && !strstr(c,"Controller") && !strstr(c,"Default") && !strstr(c,"Spectator") &&
             !strstr(c,"Boot") && !strstr(c,"Proxy") && !strstr(c,"Info") && !strstr(c,"State") &&
             !strstr(c,"Component") && !strstr(c,"Manager") && !strstr(c,"GameMode");
    if (g_clsN < 256) { g_clsC[g_clsN] = cls; g_clsB[g_clsN] = b; g_clsN++; }
    return b;
}
// enemy body filter: pointer-cached class verdict + cheap CDO skip
static bool is_body_name(void* o) {
    if (!cls_is_body(obj_class(o))) return false;
    char nm[40]; obj_name(o, nm, sizeof nm);
    return strncmp(nm, "Default__", 9) != 0;
}
// Determine if the current game mode is FFA/no-teams (target everyone). Reads PavlovGameState's
// GameModeType byte and resolves its enum name via the EPavlovGameModeType UEnum (build-robust).
// Cached (gs + enum pointers) so steady-state is just a byte read; refreshes on stale gs.
static bool g_ffaMode = false;
static void resolve_ffa() {
    static void* c_gs = nullptr, *g_gs = nullptr, *g_uenum = nullptr; static int32_t o_gmt = -2;
    static int dbg = 0;
    if (!c_gs) c_gs = find_class("PavlovGameState");
    if (!c_gs) { if (dbg++ < 3) LOG("ffa: no PavlovGameState class"); return; }
    if (!g_gs || !addr_readable((uintptr_t)g_gs) || obj_class(g_gs) != c_gs) {   // (re)find the gamestate
        g_gs = nullptr; int32_t n = objects_num();
        for (int i = 0; i < n; i++) { void* o = object_at(i);
            if (!o || !addr_readable((uintptr_t)o) || obj_class(o) != c_gs) continue;
            char nm[32]; obj_name(o, nm, sizeof nm); if (!strncmp(nm, "Default__", 9)) continue;
            g_gs = o; break; }
        if (!g_gs) { if (dbg++ < 3) LOG("ffa: no gamestate instance"); return; }
    }
    if (o_gmt == -2) o_gmt = prop_offset(c_gs, "GameModeType");
    if (o_gmt < 0) { if (dbg++ < 3) LOG("ffa: no GameModeType offset"); return; }
    int v = *(uint8_t*)((uint8_t*)g_gs + o_gmt);
    if (!g_uenum) { int32_t n = objects_num();
        for (int i = 0; i < n; i++) { void* o = object_at(i);
            if (!o || !addr_readable((uintptr_t)o)) continue;
            char nm[48]; obj_name(o, nm, sizeof nm);
            if (!strcmp(nm, "EPavlovGameModeType")) { g_uenum = o; break; } } }
    char nm[64] = "";
    if (g_uenum) { void* data = *(void**)((uint8_t*)g_uenum + 0x40);   // UEnum::Names TArray data@0x40 num@0x48
        int num = *(int32_t*)((uint8_t*)g_uenum + 0x48);
        if (addr_readable((uintptr_t)data)) for (int i = 0; i < num && i < 128; i++) {
            int64_t val = *(int64_t*)((uint8_t*)data + i*16 + 8);
            if (val == v) { fname_to_str(*(int32_t*)((uint8_t*)data + i*16), nm, sizeof nm); break; } } }
    const char* s = strstr(nm, "::"); s = s ? s + 2 : nm;   // mode name after the enum prefix
    g_ffaMode = !strcmp(s,"TTT") || !strcmp(s,"Deathmatch") || !strcmp(s,"LastManStanding") ||
                !strcmp(s,"GunGame") || !strcmp(s,"WW2GunGame") || !strcmp(s,"Zwar") || strstr(s,"FFA") != nullptr;
    static int lg = 0; if (lg++ < 8) LOG("ffa: gmType=%d name='%s' ffa=%d", v, nm, g_ffaMode);
}
// silent aim: pick the enemy head nearest the gun's current aim (<=3deg cone), point the gun at it
static void do_aim(void* me, bool rotate, bool tp) {
    // get_item runs a UFunction (GetItemOfClass); online it can catch the pawn mid-replication and
    // fault inside libUnreal -> must be fault-guarded (esp. at 30Hz continuous aim).
    void* gun = nullptr;
    g_fguard = 1; if (sigsetjmp(g_fjmp, 1)) { g_fguard = 0; return; }
    gun = resolve_gun(me);   // online-robust (get_item or last-fired gun fallback)
    g_fguard = 0;
    static int ec = 0; if (rotate && ec++ < 20)
        LOG("do_aim: GetLoc=%p GetRot=%p SetRot=%p gun=%p nbots=%d", fn_GetLoc, fn_GetRot, fn_SetRot, gun, g_nbots);
    if (!fn_GetLoc || !fn_GetRot || !fn_SetRot) return;   // look_at is pure C now
    if (!gun || !addr_readable((uintptr_t)gun) || !in_lib(*(uintptr_t*)gun)) return;
    FVec gunLoc{}; FRot gunRot{};
    g_fguard = 1; if (sigsetjmp(g_fjmp, 1)) { g_fguard = 0; return; }
    gunLoc = aim_getloc(gun); gunRot = aim_getrot(gun); g_fguard = 0;
    // read MY team fresh from the local pawn (background g_myteam is racy / sometimes -1)
    int32_t myteam = (me && o_TeamId >= 0 && addr_readable((uintptr_t)me)) ? *(int32_t*)((uint8_t*)me + o_TeamId) : -1;
    // FFA/no-teams -> target everyone; team modes -> skip teammates. From GameModeType (reference-faithful).
    resolve_ffa();
    bool teamMode = !g_ffaMode;
    void* best = nullptr; FVec bestHead{}; double bestScore = 361.0;
    for (int i = 0; i < g_nbots; i++) {
        void* o = g_bots[i];
        g_fguard = 1; if (sigsetjmp(g_fjmp, 1)) { g_fguard = 0; continue; }   // per-bot guard
        if (o && addr_readable((uintptr_t)o) && o != me && in_lib(*(uintptr_t*)o) && cls_is_body(obj_class(o))) {
            bool skip = (teamMode && myteam >= 0 && o_TeamId >= 0 && *(int32_t*)((uint8_t*)o + o_TeamId) == myteam);
            if (pawn_dead(o)) skip = true;                 // never aim at corpses
            char cn[40]; obj_name(obj_class(o), cn, sizeof cn);
            if (strstr(cn, "Ghost")) skip = true;
            void* avatar = (o_Avatar >= 0) ? rd_obj(o, o_Avatar) : nullptr;
            if (!skip && avatar && addr_readable((uintptr_t)avatar) && in_lib(*(uintptr_t*)avatar)) {
                // accurate head = skull socket on the avatar mesh (via AvatarSkin->SkullSocket FName)
                uint64_t skull = 0;
                if (o_AvatarSkin >= 0) { void* skin = rd_obj(o, o_AvatarSkin);
                    if (skin && addr_readable((uintptr_t)skin)) {
                        if (o_SkullSocket < 0) o_SkullSocket = prop_offset(obj_class(skin), "SkullSocket");
                        if (o_SkullSocket >= 0) skull = *(uint64_t*)((uint8_t*)skin + o_SkullSocket); } }
                FVec head{};
                if (skull && fn_SockLoc) head = aim_sockloc(avatar, skull);
                // require a real head near the gun (skip origin / far-garbage -> never aim at self)
                if (!(head.x == 0 && head.y == 0 && head.z == 0)) {
                    head.z += g_mei.aim_head_z;   // socket sits at the crown; drop to head centre (tunable)
                    double d = fabs(head.x - gunLoc.x) + fabs(head.y - gunLoc.y) + fabs(head.z - gunLoc.z);
                    if (d > 1.0 && d < 500000.0) {
                        FRot to = look_at(gunLoc, head);
                        double dp = fabs(angnorm(to.pitch - gunRot.pitch));
                        double dy = fabs(angnorm(to.yaw - gunRot.yaw));
                        double cone = g_mei.aim_fov * 0.5; if (cone < 1.0) cone = 1.0;
                        if (dp <= cone && dy <= cone) { double sc = dp + dy;
                            if (sc < bestScore) { bestScore = sc; best = o; bestHead = head; } }
                    }
                }
            }
        }
        g_fguard = 0;
    }
    g_aim_target = best;
    if (best) g_aim_targetHead = bestHead;   // remember head pos for trigger-kill (ServerReportBulletHit)
    static int ac = 0; if (rotate && ac++ < 30)
        LOG("aim: nbots=%d best=%p score=%.0f", g_nbots, best, bestScore);
    if (rotate && best) { g_fguard = 1; if (!sigsetjmp(g_fjmp, 1)) {
        aim_setrot(gun, look_at(gunLoc, bestHead));   // bestHead already includes aim_head_z offset
    } g_fguard = 0; }
    // WALLBANG (bullet TP): teleport the gun to just behind the target head along the aim direction so
    // the server's bullet trace originates past the wall -> the shot ignores geometry. Fire tick only.
    if (tp && best) { g_fguard = 1; if (!sigsetjmp(g_fjmp, 1)) {
        FRot to = look_at(gunLoc, bestHead);
        const double D2R = 0.017453292519943295;
        double cp = cos(to.pitch*D2R), sp = sin(to.pitch*D2R), cy = cos(to.yaw*D2R), sy = sin(to.yaw*D2R);
        FVec fwd{ cp*cy, cp*sy, sp };
        const double STANDOFF = 45.0;   // sit just OFF the head (not inside it); still past the wall
        FVec np{ bestHead.x - fwd.x*STANDOFF, bestHead.y - fwd.y*STANDOFF, bestHead.z - fwd.z*STANDOFF };
        aim_setrot(gun, to); aim_setloc(gun, np);
    } g_fguard = 0; }
}

static void resolve_names() {
    c_PavlovPawn = find_class("PavlovPawn");
    if (!c_PavlovPawn) { LOG("PavlovPawn not found"); return; }
    o_Avatar = prop_offset(c_PavlovPawn, "Avatar");
    o_XRay0  = prop_offset(c_PavlovPawn, "XRayMaterialTeam0");
    o_XRay1  = prop_offset(c_PavlovPawn, "XRayMaterialTeam1");
    fn_IsDead= find_func(c_PavlovPawn, "IsDead");
    c_PawnBase = struct_super(c_PavlovPawn);          // PavlovPawnBase — catches all pawn types
    o_TeamId = prop_offset(c_PavlovPawn, "TeamId");
    o_bDead  = prop_offset(c_PavlovPawn, "bDead");    // skip dead pawns (frozen ragdoll chams)
    c_Mesh = find_class("SkeletalMeshComponent");    // only skinned meshes (renderable bodies)
    fn_SetOverlay = c_Mesh ? find_func(c_Mesh, "SetOverlayMaterial") : nullptr;
    fn_SetMaterial = c_Mesh ? find_func(c_Mesh, "SetMaterial") : nullptr;
    fn_CreateMID = c_Mesh ? find_func(c_Mesh, "CreateDynamicMaterialInstance") : nullptr;   // custom cham colors
    { void* cmid = find_class("MaterialInstanceDynamic");
      fn_SetVecParam = cmid ? find_func(cmid, "SetVectorParameterValue") : nullptr; }
    LOG("customcolor: CreateMID=%p SetVecParam=%p", fn_CreateMID, fn_SetVecParam);
    fn_GetMaterial = c_Mesh ? find_func(c_Mesh, "GetMaterial") : nullptr;   // for ESP restore-on-disable
    fn_CompLoc = c_Mesh ? find_func(c_Mesh, "K2_GetComponentLocation") : nullptr;
    o_SkelAsset = c_Mesh ? prop_offset(c_Mesh, "SkeletalMeshAsset") : -1;
    if (o_SkelAsset < 0 && c_Mesh) o_SkelAsset = prop_offset(c_Mesh, "SkinnedAsset");
    void* csc = find_class("SceneComponent");
    fn_SetVis = csc ? find_func(csc, "SetVisibility") : nullptr;
    { void* cac = find_class("Actor"); fn_SetHidden = cac ? find_func(cac, "SetActorHiddenInGame") : nullptr; }
    fn_GetItem = find_func(c_PavlovPawn, "GetItemOfClass");
    c_Gun = find_class("Gun_Base_C");
    c_VRGun = find_class("VRGun");
    LOG("GetItemOfClass=%p Gun=%p VRGun=%p", fn_GetItem, c_Gun, c_VRGun);
    fn_ServerGive = find_func(c_PavlovPawn, "ServerGive");   // ServerGive(FName Equipment@0, FName Skin@8)
    LOG("ServerGive=%p", fn_ServerGive);
    // silent-aim funcs (Actor + component + KismetMath)
    fn_GetLoc = find_func(c_PavlovPawn, "K2_GetActorLocation");
    fn_GetRot = find_func(c_PavlovPawn, "K2_GetActorRotation");
    fn_SetRot = find_func(c_PavlovPawn, "K2_SetActorRotation");
    fn_SetLoc = find_func(c_PavlovPawn, "K2_SetActorLocation");   // bullet TP (server traces from replicated pos)
    fn_AddMoveInput = find_func(c_PavlovPawn, "AddMovementInput"); // smooth noclip vertical (flying integrator)
    c_VRMagazine = find_class("VRMagazine");                       // infinite-ammo ancestry match
    LOG("AddMovementInput=%p VRMagazine=%p", fn_AddMoveInput, c_VRMagazine);
    fn_SockLoc = c_Mesh ? find_func(c_Mesh, "GetSocketLocation") : nullptr;
    void* kml = find_class("KismetMathLibrary");
    fn_LookAt = kml ? find_func(kml, "FindLookAtRotation") : nullptr;
    cdo_KML = find_object("Default__KismetMathLibrary");
    o_AvatarSkin = prop_offset(c_PavlovPawn, "AvatarSkin");
    LOG("aim: GetLoc=%p GetRot=%p SetRot=%p SockLoc=%p LookAt=%p KML=%p AvatarSkin@%d",
        fn_GetLoc, fn_GetRot, fn_SetRot, fn_SockLoc, fn_LookAt, cdo_KML, o_AvatarSkin);
    // homing knife: collect the knife throw UFunctions + the velocity func + RootComponent offset
    { void* ck = find_class("Knife");
      const char* kt[3] = { "StartKnifeThrow", "OnKnifeThrow", "MulticastThrow" };
      g_nknife = 0;
      for (int k = 0; k < 3 && ck; k++) { void* fn = find_func(ck, kt[k]);
          if (fn) { g_knife[g_nknife++] = fn; LOG("knife throw '%s' = %p", kt[k], fn); } }
      void* cprim = find_class("PrimitiveComponent");
      fn_SetPhysVel = cprim ? find_func(cprim, "SetPhysicsLinearVelocity") : nullptr;
      void* cact = find_class("Actor");
      o_RootComp = cact ? prop_offset(cact, "RootComponent") : -1;
      fn_SetCollision = cact ? find_func(cact, "SetActorEnableCollision") : nullptr;   // noclip
      LOG("knife: nthrow=%d SetPhysVel=%p RootComp@%d", g_nknife, fn_SetPhysVel, o_RootComp); }
    // anti-votekick: collect the client-side kick RPCs so the handler can DROP them (ignore the kick)
    { void* cpc = find_class("PlayerController");
      const char* kn[3] = { "ClientWasKicked", "ClientReturnToMainMenuWithTextReason", "ClientReturnToMainMenu" };
      g_nkick = 0;
      for (int k = 0; k < 3 && cpc; k++) { void* fn = find_func(cpc, kn[k]);
          if (fn) { g_kick[g_nkick++] = fn; LOG("kick RPC '%s' = %p", kn[k], fn); } }
      fn_ChangeName = cpc ? find_func(cpc, "ServerChangeName") : nullptr;   // name spoof (custom-map admin)
      // AUTH TRACE: resolve the join-handshake RPCs so the handler can log the LIVE sequence + params
      // (which gate fires, what the host demands, what our client actually sends).
      { void* cpvc0 = find_class("PavlovPlayerController");
        const char* an[] = { "ClientAuthenticate", "ServerAuthenticate", "ClientRequestAttestation",
                             "ServerSubmitAttestation", "ServerAnticheatMessage", "ClientAnticheatMessage",
                             "OnAuthTimedout", "ClientOnConnected", "ServerAuthenticateGuest" };
        g_nauthfn = 0;
        for (auto nm : an) { void* fn = cpvc0 ? find_func(cpvc0, nm) : nullptr;
          if (fn && g_nauthfn < 24) { g_authfn[g_nauthfn] = fn; g_authnm[g_nauthfn] = nm; g_nauthfn++;
            LOG("auth RPC '%s' = %p", nm, fn); } } }
      // VOICE PROBE: trace the voice-router RPCs so we can SEE real packet Data sizes + cadence when
      // people talk (that's the Opus framing we must match), and grab ServerOnVoice for injection.
      { c_VoiceRouter = find_class("VoiceRouter");
        const char* vn[] = { "ServerOnVoice", "ClientOnVoiceBunch", "OnEncodedVoiceData", "ReplayOnVoiceBunch_Client" };
        for (auto nm : vn) { void* fn = c_VoiceRouter ? find_func(c_VoiceRouter, nm) : nullptr;
          if (fn && g_nauthfn < 24) { g_authfn[g_nauthfn] = fn; g_authnm[g_nauthfn] = nm; g_nauthfn++;
            LOG("voice RPC '%s' = %p", nm, fn); } }
        fn_ServerOnVoice = c_VoiceRouter ? find_func(c_VoiceRouter, "ServerOnVoice") : nullptr;
        fn_ClientOnVoiceBunch = c_VoiceRouter ? find_func(c_VoiceRouter, "ClientOnVoiceBunch") : nullptr;
        LOG("voice: VoiceRouter=%p ServerOnVoice=%p ClientOnVoiceBunch=%p", c_VoiceRouter, fn_ServerOnVoice, fn_ClientOnVoiceBunch); }
      // trigger-kill RPC + the gun/bullet classes the PC reference uses for a guaranteed headshot
      void* cpvc = find_class("PavlovPlayerController");
      fn_ReportHit = cpvc ? find_func(cpvc, "ServerReportBulletHit") : nullptr;
      // gun/bullet classes for the report: try strong ones, else leave null (falls back to held gun).
      const char* gnames[] = { "Gun_AntiTank_C", "Gun_50cal_C", "Gun_HuntingRifle_C", "Gun_AR9_C" };
      for (auto g : gnames) { c_KillGun = find_class(g); if (c_KillGun) break; }
      const char* bnames[] = { "Bullet_50Cal_C", "Bullet_762_C", "Bullet_556_C", "Bullet_9mm_C", "Bullet_Base_C" };
      for (auto b : bnames) { c_KillBullet = find_class(b); if (c_KillBullet) break; }
      o_HeadBone   = c_PavlovPawn ? prop_offset(c_PavlovPawn, "HeadBoneName") : -1;
      // resolve FClientBulletHit field offsets by name (fall back to the SDK defaults set above)
      void* bhs = find_scriptstruct("ClientBulletHit");
      if (bhs) {
          #define BHF(var,nm) { int32_t o = prop_offset(bhs, nm); if (o >= 0) var = o; }
          BHF(bh_Target,"Target") BHF(bh_Hit,"Hit") BHF(bh_Head,"bHeadshot") BHF(bh_Pen,"bPenetrated")
          BHF(bh_Bullet,"BulletClass") BHF(bh_Gun,"GunClass") BHF(bh_Origin,"Origin") BHF(bh_Bone,"BoneName")
          #undef BHF
          bh_resolved = true;
      }
      LOG("triggerkill: ReportHit=%p KillGun=%p KillBullet=%p HeadBone@%d struct=%p (Target@%d Hit@%d HS@%d Gun@%d Bullet@%d Origin@%d Bone@%d)",
          fn_ReportHit, c_KillGun, c_KillBullet, o_HeadBone, bhs, bh_Target, bh_Hit, bh_Head, bh_Gun, bh_Bullet, bh_Origin, bh_Bone);
      // ESP + TTT plumbing: buy RPC, per-pawn PlayerState/HealthComponent, name, health, role field
      fn_ServerBuy = cpvc ? find_func(cpvc, "ServerBuy") : nullptr;
      o_PawnPS = c_PavlovPawn ? prop_offset(c_PavlovPawn, "PlayerState") : -1;
      o_PawnHC = c_PavlovPawn ? prop_offset(c_PavlovPawn, "HealthComponent") : -1;
      { void* cps = find_class("PlayerState"); o_PS_Name = cps ? prop_offset(cps, "PlayerNamePrivate") : -1; }
      { void* chc = find_class("HealthComponent"); if (chc) { o_HC_H = prop_offset(chc, "Health"); o_HC_MH = prop_offset(chc, "MaxHealth"); } }
      // TTT role: try a client-visible field on PavlovPlayerState (best-effort; server usually hides it)
      { void* cps2 = find_class("PavlovPlayerState");
        const char* rn[] = { "TTTRole", "RoleName", "Role", "CurrentRole", "PlayerRole" };
        for (auto r : rn) { if (cps2) { int32_t o = prop_offset(cps2, r); if (o >= 0) { o_PS_Role = o; break; } } }
        if (o_PS_Role < 0) o_PS_Role = -1; }
      LOG("esp/ttt: ServerBuy=%p PawnPS@%d PawnHC@%d PSName@%d HC.H@%d HC.MH@%d PSRole@%d",
          fn_ServerBuy, o_PawnPS, o_PawnHC, o_PS_Name, o_HC_H, o_HC_MH, o_PS_Role);
      void* cgm = find_class("GameModeBase");
      fn_GMChangeName = cgm ? find_func(cgm, "ChangeName") : nullptr;       // proper rename (any length, allocates)
      void* cw2 = find_class("World"); o_AuthGM = cw2 ? prop_offset(cw2, "AuthorityGameMode") : -1;
      LOG("ServerChangeName=%p GM.ChangeName=%p AuthGM@%d", fn_ChangeName, fn_GMChangeName, o_AuthGM);
      fn_ClientEnNetVoice = cpc ? find_func(cpc, "ClientEnableNetworkVoice") : nullptr;   // stock UE VoIP enable
      fn_ToggleSpeaking   = cpc ? find_func(cpc, "ToggleSpeaking") : nullptr; }           // start/stop broadcast
    // VOICE (Quest fix): resolve the whole voice pipeline by name.
    { void* cars = find_class("AndroidRuntimeSettings");
      cdo_ARS = find_object("Default__AndroidRuntimeSettings");
      if (cars) o_AndroidVoice = prop_offset(cars, "bAndroidVoiceEnabled");
      c_GUS = find_class("PavlovGameUserSettings");
      if (c_GUS) { o_MicMuted = prop_offset(c_GUS, "bMicMuted"); o_VoiceDev = prop_offset(c_GUS, "VoiceDevice");
                   fn_SetMicMuted = find_func(c_GUS, "SetMicMuted"); fn_SetVoiceDevice = find_func(c_GUS, "SetVoiceDevice"); }
      void* cgi = find_class("PavlovGameInstance");
      if (cgi) { fn_CheckEnVoice = find_func(cgi, "CheckAndEnableVoiceCapture"); fn_GetVoiceSource = find_func(cgi, "GetVoiceSource"); }
      c_OnlineLobby = find_class("OnlineLobby");
      if (c_OnlineLobby) { fn_SetVoiceChat = find_func(c_OnlineLobby, "SetVoiceChat"); fn_IsVoiceChatEn = find_func(c_OnlineLobby, "IsVoiceChatEnabled"); }
      void* cst = find_class("PavlovStatics"); cdo_PavStatics = find_object("Default__PavlovStatics");
      if (cst) { fn_GetVoiceLevelPeak = find_func(cst, "GetVoiceLevelPeak"); fn_ResetVoiceDev = find_func(cst, "ResetVoiceCaptureDevice"); }
      if (cgi) o_GI_VoiceSrc = prop_offset(cgi, "VoiceSource");
      c_VoiceSource = find_class("VoiceSource");
      if (c_VoiceSource) { fn_VS_SetCapture = find_func(c_VoiceSource, "SetCapture");
                           fn_VS_IsCapturing = find_func(c_VoiceSource, "IsCapturing");
                           fn_VS_IsBroadcasting = find_func(c_VoiceSource, "IsBroadcasting");
                           fn_VS_ResetCap = find_func(c_VoiceSource, "ResetCaptureDevice"); }
      { void* cpp = find_class("PavlovPawn"); if (cpp) o_Pawn_VoiceActive = prop_offset(cpp, "bVoiceActive"); }
      LOG("voice: VoiceSrc cls=%p GI.VoiceSrc@%d SetCapture=%p IsCapturing=%p IsBroadcasting=%p ResetCap=%p ToggleSpeak=%p bVoiceActive@%d",
          c_VoiceSource, o_GI_VoiceSrc, fn_VS_SetCapture, fn_VS_IsCapturing, fn_VS_IsBroadcasting, fn_VS_ResetCap, fn_ToggleSpeaking, o_Pawn_VoiceActive);
      LOG("voice: ARScdo=%p AndroidVoice@%d GUS=%p MicMuted@%d VoiceDev@%d SetMic=%p SetDev=%p",
          cdo_ARS, o_AndroidVoice, c_GUS, o_MicMuted, o_VoiceDev, fn_SetMicMuted, fn_SetVoiceDevice);
      LOG("voice: CheckEnVoice=%p GetVoiceSrc=%p Lobby=%p SetVoiceChat=%p IsVoiceEn=%p ClientEnNet=%p PeakLvl=%p ResetDev=%p",
          fn_CheckEnVoice, fn_GetVoiceSource, c_OnlineLobby, fn_SetVoiceChat, fn_IsVoiceChatEn, fn_ClientEnNetVoice, fn_GetVoiceLevelPeak, fn_ResetVoiceDev); }
    // collect the guns' fire UFunctions on the gun classes...
    const char* fnames[9] = { "Fired", "Fire", "OnFire", "TryFire", "Shoot",
                              "FireWeapon", "PullTrigger", "BeginFire", "ServerFire" };
    void* gclasses[2] = { c_Gun, c_VRGun };
    g_nfire = 0;
    for (int gc = 0; gc < 2; gc++) if (gclasses[gc])
        for (int f = 0; f < 9 && g_nfire < 16; f++) {
            void* fn = find_func(gclasses[gc], fnames[f]);
            if (fn) { bool dup = false; for (int k = 0; k < g_nfire; k++) if (g_fire[k] == fn) dup = true;
                if (!dup) { g_fire[g_nfire++] = fn; LOG("fire func '%s' = %p", fnames[f], fn); } }
        }
    // ...and globally: any UFunction whose name contains Fire/Fired/Shoot owned by a gun/weapon class
    { int32_t nn = objects_num();
      for (int32_t i = 0; i < nn && g_nfire < 16; i++) {
        void* o = object_at(i);
        if (!o || !addr_readable((uintptr_t)o)) continue;
        char cc[40]; obj_name(obj_class(o), cc, sizeof cc);
        if (strcmp(cc, "Function")) continue;
        char fn[64]; obj_name(o, fn, sizeof fn);
        if (!(strstr(fn,"Fire")||strstr(fn,"Fired")||strstr(fn,"Shoot"))) continue;
        // only the actual shot — reject Can/Mode/Dry/Cycle/Changed/Reload/Ammo/Rate/Anim/Sound/FX
        if (strstr(fn,"Can")||strstr(fn,"Mode")||strstr(fn,"Dry")||strstr(fn,"Cycle")||strstr(fn,"Changed")||
            strstr(fn,"Reload")||strstr(fn,"Ammo")||strstr(fn,"Rate")||strstr(fn,"Anim")||strstr(fn,"Sound")||
            strstr(fn,"Effect")||strstr(fn,"Loop")||strstr(fn,"Stop")||strstr(fn,"End")) continue;
        void* outer = *(void**)((uint8_t*)o + 0x20);
        char on[48] = ""; if (addr_readable((uintptr_t)outer)) obj_name(outer, on, sizeof on);
        if (!(strstr(on,"Gun")||strstr(on,"Weapon")||strstr(on,"VRGun"))) continue;
        bool dup = false; for (int k = 0; k < g_nfire; k++) if (g_fire[k] == o) dup = true;
        if (!dup) { g_fire[g_nfire++] = o; LOG("fire func(global) '%s' owner '%s' = %p", fn, on, o); }
      } }
    // DUMP: gun/vrgun RPCs that carry the shot vector (Fire/Shoot/Server/Trace/Hit) + their params
    for (int gc = 0; gc < 2; gc++) { void* cls = gclasses[gc]; if (!cls) continue;
      for (void* fn = *(void**)((uint8_t*)cls + USTRUCT_CHILDREN); addr_readable((uintptr_t)fn);
           fn = *(void**)((uint8_t*)fn + UFIELD_NEXT_OFF)) {
        char nm[64]; obj_name(fn, nm, sizeof nm);
        if (strstr(nm,"Fire")||strstr(nm,"Shoot")||strstr(nm,"Server")||strstr(nm,"Trace")||strstr(nm,"Hit")||strstr(nm,"Shot"))
            dump_params(nm, fn);
        if (!*(void**)((uint8_t*)fn + UFIELD_NEXT_OFF)) break;
      } }
    c_Ghost = find_class("GhostPawn");
    // Canvas draw: collect ReceiveDrawHUD UFunctions + resolve K2_DrawText on UCanvas
    c_Canvas = find_class("Canvas");
    fn_DrawText = c_Canvas ? find_func(c_Canvas, "K2_DrawText") : nullptr;
    { int32_t nn = objects_num(); g_ndrawhud = 0;
      for (int32_t i = 0; i < nn && g_ndrawhud < 8; i++) {
        void* o = object_at(i);
        if (!o || !addr_readable((uintptr_t)o)) continue;
        char cc[40]; obj_name(obj_class(o), cc, sizeof cc);
        if (strcmp(cc, "Function")) continue;
        char fn[64]; obj_name(o, fn, sizeof fn);
        if (strcmp(fn, "ReceiveDrawHUD")) continue;
        g_drawhud[g_ndrawhud++] = o; LOG("ReceiveDrawHUD = %p", o);
      } }
    LOG("Canvas=%p K2_DrawText=%p ndrawhud=%d", c_Canvas, fn_DrawText, g_ndrawhud);
    LOG("PavlovPawn=%p Ghost=%p MeshComp=%p SetOverlay=%p XRay0@%d XRay1@%d Team@%d",
        c_PavlovPawn, c_Ghost, c_Mesh, fn_SetOverlay, o_XRay0, o_XRay1, o_TeamId);
    g_ready = (fn_SetOverlay && c_Mesh && c_PavlovPawn && o_XRay0 >= 0);
    fn_SetPlayerSkin = find_func(c_PavlovPawn, "SetPlayerSkin");   // SetPlayerSkin(FName PlayerSkin)
    LOG("SetPlayerSkin=%p", fn_SetPlayerSkin);
}
// world graph offsets — resolved BY NAME on this build (the frida values were a different build)
static int32_t O_WORLD_GI = -1, O_GI_LP = -1, O_LP_PC = -1, O_PC_ACKPAWN = -1, O_CTRL_PAWN = -1;
static void resolve_graph() {
    if (O_WORLD_GI >= 0) return;
    void* cw = find_class("World"); void* cgi = find_class("GameInstance");
    void* cpl = find_class("Player"); void* cpc = find_class("PlayerController");
    void* cctl = find_class("Controller");
    if (cw)  O_WORLD_GI   = prop_offset(cw,  "OwningGameInstance");
    if (cgi) O_GI_LP      = prop_offset(cgi, "LocalPlayers");
    if (cpl) O_LP_PC      = prop_offset(cpl, "PlayerController");
    if (cpc) O_PC_ACKPAWN = prop_offset(cpc, "AcknowledgedPawn");
    if (cctl) O_CTRL_PAWN = prop_offset(cctl, "Pawn");
    LOG("graph: World.GI@%d GI.LP@%d Player.PC@%d PC.Ack@%d Ctrl.Pawn@%d",
        O_WORLD_GI, O_GI_LP, O_LP_PC, O_PC_ACKPAWN, O_CTRL_PAWN);
}
static void* g_world = nullptr;
// Walk World -> GI -> LocalPlayers[0] -> PlayerController -> (Acknowledged|Controlled) Pawn.
// Returns our pawn, or null if this world doesn't currently yield one.
// After a lobby/round switch the engine leaves LocalPlayer->PlayerController NULL (diagnosed:
// worlds=1 GI=1 LP=1 PC=0), which zeros local_pawn/local_controller and breaks everything pawn-based
// (target-kill included). Find the PC another way: scan for the PlayerController whose ->Player back-
// points to our LocalPlayer (a link the engine keeps). Cached + throttled — off the 90Hz pawn path.
static void* g_localPC = nullptr;
static void* resolve_local_pc(void* lp) {
    static int32_t o_player = -2; static void* pcCls = nullptr;
    if (!pcCls) pcCls = find_class("PlayerController");
    if (!pcCls || !lp) return nullptr;
    if (o_player == -2) o_player = prop_offset(pcCls, "Player");
    if (o_player < 0) return nullptr;
    if (g_localPC && addr_readable((uintptr_t)g_localPC) && in_lib(*(uintptr_t*)g_localPC) && is_a(g_localPC, pcCls) &&
        *(void**)((uint8_t*)g_localPC + o_player) == lp) return g_localPC;   // cache still valid
    static long last = 0; struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    long ms = ts.tv_sec*1000 + ts.tv_nsec/1000000; if (ms - last < 300) return nullptr; last = ms;
    g_localPC = nullptr;
    int32_t n = objects_num();
    for (int32_t i = 0; i < n; i++) { void* o = object_at(i);
        if (!o || !addr_readable((uintptr_t)o) || !in_lib(*(uintptr_t*)o) || !is_a(o, pcCls)) continue;
        char nm[24]; obj_name(o, nm, sizeof nm); if (!strncmp(nm, "Default__", 9)) continue;
        if (*(void**)((uint8_t*)o + o_player) == lp) { g_localPC = o; return o; }
    }
    return nullptr;
}
static void* pawn_from_world(void* w) {
    if (!w || !addr_readable((uintptr_t)w) || O_WORLD_GI < 0 || O_GI_LP < 0 || O_LP_PC < 0) return nullptr;
    void* gi = *(void**)((uint8_t*)w + O_WORLD_GI);
    if (!addr_readable((uintptr_t)gi)) return nullptr;
    void* lpData = *(void**)((uint8_t*)gi + O_GI_LP);
    int32_t lpNum = *(int32_t*)((uint8_t*)gi + O_GI_LP + 8);
    if (!addr_readable((uintptr_t)lpData) || lpNum < 1) return nullptr;
    void* lp = ((void**)lpData)[0];
    if (!addr_readable((uintptr_t)lp)) return nullptr;
    void* pc = *(void**)((uint8_t*)lp + O_LP_PC);
    if (!addr_readable((uintptr_t)pc)) pc = resolve_local_pc(lp);   // LP->PC null after travel -> find by ->Player
    if (!addr_readable((uintptr_t)pc)) return nullptr;
    void* pawn = (O_PC_ACKPAWN >= 0) ? *(void**)((uint8_t*)pc + O_PC_ACKPAWN) : nullptr;
    if (!addr_readable((uintptr_t)pawn) && O_CTRL_PAWN >= 0) pawn = *(void**)((uint8_t*)pc + O_CTRL_PAWN);
    return addr_readable((uintptr_t)pawn) ? pawn : nullptr;
}
// Cache the world only while it actually yields our pawn. On match move the old world stops
// yielding a pawn -> we re-scan for the new one. This is what fixes cross-match breakage.
static void* find_world() {
    resolve_graph();
    if (O_WORLD_GI < 0) return nullptr;
    if (g_world && pawn_from_world(g_world)) return g_world;  // cached world still ours
    g_world = nullptr;
    void* wc = find_class("World");
    if (!wc) return nullptr;
    int32_t n = objects_num();
    for (int32_t i = 0; i < n; i++) {
        void* o = object_at(i);
        if (!o || !addr_readable((uintptr_t)o) || obj_class(o) != wc) continue;
        char nm[32]; obj_name(o, nm, sizeof nm);
        if (!strncmp(nm, "Default__", 9)) continue;
        if (pawn_from_world(o)) { g_world = o; return o; }    // pick the world that yields our pawn
    }
    return nullptr;
}
static void* local_pawn() {
    void* w = find_world(); if (!w) return nullptr;
    return pawn_from_world(w);
}
static void resolve_setmaterial(void* mesh) {
    if (fn_SetMaterial) return;
    fn_SetMaterial = find_func(obj_class(mesh), "SetMaterial");
    char cn[64]; obj_name(obj_class(mesh), cn, sizeof cn);
    LOG("mesh class '%s' SetMaterial=%p", cn, fn_SetMaterial);
}
// the pawn's XRayMaterialTeam fields are null on the client — find the xray material asset directly
static void* g_xray_mat = nullptr;
static void* g_xray0 = nullptr, *g_xray1 = nullptr;   // team-colored xray materials
static bool is_material(void* o) {
    if (!o || !addr_readable((uintptr_t)o)) return false;
    void* c = obj_class(o);
    if (!addr_readable((uintptr_t)c)) return false;
    char cn[64]; obj_name(c, cn, sizeof cn);
    return strstr(cn, "Material") != nullptr;
}
static void* read_xray(void* pawn) {
    void* m1 = rd_obj(pawn, o_XRay1), *m0 = rd_obj(pawn, o_XRay0);
    if (is_material(m1)) return m1;
    if (is_material(m0)) return m0;
    return nullptr;
}
// call GhostPawn::GetXRayMaterial — a UFUNCTION that loads + returns the real see-through material
static void load_xray_via_ghost() {
    if (g_xray_mat) return;
    void* ghost = find_object("Default__GhostPawn");
    void* gcls = ghost ? obj_class(ghost) : find_class("GhostPawn");
    void* fn = gcls ? find_func(gcls, "GetXRayMaterial") : nullptr;
    if (!ghost || !fn) { LOG("ghost=%p GetXRayMaterial=%p", ghost, fn); return; }
    // dump the function's parameters (name @ offset) so we call it right
    for (void* p = *(void**)((uint8_t*)fn + USTRUCT_CHILDPROPS); addr_readable((uintptr_t)p);
         p = *(void**)((uint8_t*)p + FFIELD_NEXT_OFF)) {
        char pn[64]; field_name(p, pn, sizeof pn);
        LOG("  GetXRayMaterial param '%s' @ %d", pn, *(int32_t*)((uint8_t*)p + FPROP_OFFSET_OFF));
    }
    for (int team = 1; team >= 0 && !g_xray_mat; team--) {
        uint8_t buf[64]; memset(buf, 0, sizeof buf); *(int32_t*)buf = team;
        g_fguard = 1;
        if (sigsetjmp(g_fjmp, 1)) { g_fguard = 0; continue; }
        g_ProcessEvent(ghost, fn, buf);
        g_fguard = 0;
        LOG("GetXRayMaterial(team=%d) buf: %p %p %p %p %p", team,
            *(void**)(buf+0), *(void**)(buf+8), *(void**)(buf+16), *(void**)(buf+24), *(void**)(buf+32));
        for (int off = 0; off <= 56; off += 8) {
            void* p = *(void**)(buf + off);
            if (is_material(p)) { g_xray_mat = p;
                char mn[80]; obj_name(p, mn, sizeof mn);
                LOG("GetXRayMaterial(team=%d) -> '%s' @ %p", team, mn, p); break; }
        }
    }
}
// load one material asset via KismetSystemLibrary::LoadAsset_Blocking (synchronous)
static void* load_mat(void* ksl, void* fn, const char* pkgpath, const char* asset) {
    int32_t pkg = fname_find(pkgpath), a = fname_find(asset);
    if (pkg < 0 || a < 0) return nullptr;
    // TSoftObjectPtr: FSoftObjectPath at +16 = FTopLevelAssetPath{Package@16, Asset@24}; ret@48
    uint8_t buf[80]; memset(buf, 0, sizeof buf);
    *(int32_t*)(buf + 16) = pkg; *(int32_t*)(buf + 24) = a;
    g_fguard = 1;
    if (sigsetjmp(g_fjmp, 1)) { g_fguard = 0; return nullptr; }
    g_ProcessEvent(ksl, fn, buf);
    g_fguard = 0;
    void* r = *(void**)(buf + 48);
    if (is_material(r)) return r;      // usually in an already-mapped heap region -> no slow scan_maps
    scan_maps();                       // only if it landed in a new region, pay the /proc/self/maps cost
    return is_material(r) ? r : nullptr;
}
// force-load BOTH team xray materials (team-colored chams). Reloads if GC'd.
static void force_load_xray() {
    if (is_material(g_xray0) && is_material(g_xray1)) { g_xray_mat = g_xray1; return; }
    // reload fast (load_mat no longer pays scan_maps unless needed) so chams recover quickly after a GC
    // collects the material during heavy death/respawn. Light throttle to avoid hammering LoadAsset.
    static long last_ms = 0;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    long nowms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (nowms - last_ms < 250) return;
    last_ms = nowms;
    void* ksl = find_object("Default__KismetSystemLibrary");
    void* fn = ksl ? find_func(obj_class(ksl), "LoadAsset_Blocking") : nullptr;
    if (!ksl || !fn) return;
    if (!is_material(g_xray0))
        g_xray0 = load_mat(ksl, fn, "/Game/Gameplay/Misc/PlayerXRay/M_PlayerXRay_Team0", "M_PlayerXRay_Team0");
    if (!is_material(g_xray1))
        g_xray1 = load_mat(ksl, fn, "/Game/Gameplay/Misc/PlayerXRay/M_PlayerXRay_Team1", "M_PlayerXRay_Team1");
    g_xray_mat = is_material(g_xray1) ? g_xray1 : g_xray0;
    static int lc = 0; if (lc++ < 4) LOG("xray mats: t0=%p t1=%p", g_xray0, g_xray1);
}
static void find_xray_material() {
    if (g_xray_mat) return;
    force_load_xray();
    if (g_xray_mat) return;
    // primary: the real material by name, once the game has loaded it (e.g. after spectating once)
    int32_t n = objects_num();
    for (int32_t i = 0; i < n; i++) {
        void* o = object_at(i);
        if (!o || !addr_readable((uintptr_t)o)) continue;
        char on[80]; obj_name(o, on, sizeof on);
        if (!strstr(on, "PlayerXRay")) continue;         // M_PlayerXRay_Team0/1/Base
        if (!is_material(o)) continue;
        LOG("FOUND xray material '%s' @ %p", on, o);
        g_xray_mat = o;
        // prefer Team1 if present
        if (strstr(on, "Team1")) return;
    }
    if (g_xray_mat) return;
    static bool done = false;
    if (done) return;
    load_xray_via_ghost();
    if (g_xray_mat) { done = true; LOG("g_xray_mat = %p (ghost)", g_xray_mat); return; }
    // CDOs of the classes that actually declare XRayMaterialTeam (not the base)
    const char* cdos[2] = { "Default__BP_PavlovPawn_C", "Default__PavlovPawn" };
    for (int c = 0; c < 2 && !g_xray_mat; c++) {
        void* cdo = find_object(cdos[c]);
        if (cdo) { void* m = read_xray(cdo);
            if (m) { char mn[80]; obj_name(m, mn, sizeof mn);
                LOG("xray mat from %s -> '%s' @ %p", cdos[c], mn, m); g_xray_mat = m; } }
    }
    // diag: do the CDOs exist, and what do their XRay fields actually hold?
    for (int c = 0; c < 2; c++) {
        void* cdo = find_object(cdos[c]);
        if (!cdo) { LOG("CDO %s: NOT FOUND", cdos[c]); continue; }
        void* r1 = rd_obj(cdo, o_XRay1);
        char rc[64] = "null"; if (addr_readable((uintptr_t)r1)) obj_name(obj_class(r1), rc, sizeof rc);
        LOG("CDO %s XRay1=%p (class '%s')", cdos[c], r1, rc);
    }
    // any live PavlovPawn that has it set
    if (!g_xray_mat) {
        int32_t n = objects_num();
        for (int32_t i = 0; i < n && !g_xray_mat; i++) {
            void* o = object_at(i);
            if (!o || !addr_readable((uintptr_t)o) || !is_a(o, c_PavlovPawn)) continue;
            void* m = read_xray(o);
            if (m) { char mn[80]; obj_name(m, mn, sizeof mn);
                LOG("xray mat from live pawn -> '%s' @ %p", mn, m); g_xray_mat = m; }
        }
    }
    done = true;   // stop the ghost/CDO probes; the name-scan above keeps retrying every pass
    LOG("g_xray_mat = %p", g_xray_mat);
}

static __thread bool g_in_pass = false;

static bool call_isdead(void* pawn) {
    if (!fn_IsDead) return false;
    struct { uint8_t ret; } p{0};
    g_ProcessEvent(pawn, fn_IsDead, &p);
    return p.ret != 0;
}
static void call_setoverlay(void* mesh, void* mat) {
    struct { void* Material; } p{ mat };
    g_ProcessEvent(mesh, fn_SetOverlay, &p);
}
static void call_setmaterial(void* mesh, int32_t idx, void* mat) {
    if (!fn_SetMaterial) return;
    struct { int32_t Index; char _pad[4]; void* Material; } p{ idx, {0}, mat };
    g_ProcessEvent(mesh, fn_SetMaterial, &p);
}
static void* get_material(void* mesh, int32_t idx) {         // GetMaterial(idx) -> UMaterialInterface*
    if (!fn_GetMaterial || !mesh) return nullptr;
    struct { int32_t Index; char _pad[4]; void* Ret; } p{ idx, {0}, nullptr };
    g_ProcessEvent(mesh, fn_GetMaterial, &p);
    return p.Ret;
}
// snapshot a mesh's ORIGINAL material[0] the first time we override it (so ESP can be turned OFF).
static void cham_snapshot(void* mesh) {
    for (int i = 0; i < g_nCham; i++) if (g_chamMesh[i] == mesh) return;   // already captured
    if (g_nCham >= 256) return;
    void* orig = get_material(mesh, 0);
    g_chamMesh[g_nCham] = mesh; g_chamOrig[g_nCham] = orig; g_nCham++;
}
// restore every overridden mesh to its captured original, then clear the cache (called on disable).
static void cham_restore_all() {
    for (int i = 0; i < g_nCham; i++) {
        void* mesh = g_chamMesh[i];
        if (!mesh || !addr_readable((uintptr_t)mesh) || !in_lib(*(uintptr_t*)mesh)) continue;
        g_fguard = 1; if (!sigsetjmp(g_fjmp, 1)) call_setmaterial(mesh, 0, g_chamOrig[i]); g_fguard = 0;
    }
    g_nCham = 0;
}
static bool is_owned_by(void* o, void* pawn) {           // walk Outer chain
    for (int d = 0; d < 8 && o && addr_readable((uintptr_t)o); d++) {
        if (o == pawn) return true;
        o = *(void**)((uint8_t*)o + 0x20);               // UObject::Outer
    }
    return false;
}

#define CHAMS_TXT "/sdcard/Android/data/com.vankrupt.pavlov/files/chams.txt"
static int cfg_value() {
    // The mei menu (g_mei) is now the source of truth. chams.txt stays as a manual escape hatch:
    // an explicit 8/9 still forces the one-shot whitelist/SDK dump from adb without the menu.
    int file_v = -1;
    FILE* f = fopen(CHAMS_TXT, "r");
    if (f) { int v = -1; if (fscanf(f, "%d", &v) == 1) file_v = v; fclose(f); }
    if (file_v == 7) g_mei.menu_open = true;   // bring-up override: force the panel visible via adb
    if (file_v == 8 || file_v == 9) return file_v;
    // menu action buttons also request the dumps
    if (g_mei.act_dump_sdk)       { return 9; }
    if (g_mei.act_dump_whitelist) { return 8; }
    return mei_legacy_cfg(g_mei);   // 0 / 2 / 3 / 4 / 6 derived from the granular toggles
}
static void cfg_write(int v) { FILE* f = fopen(CHAMS_TXT, "w"); if (f) { fprintf(f, "%d", v); fclose(f); } }

// build an FText from an ASCII string (KismetTextLibrary::Conv_StringToText), write 16 bytes to out
static bool make_text(const char* s, void* outFText) {
    if (!fn_Conv || !cdo_TextLib) return false;
    static char16_t w[128]; int n = 0; for (; s[n] && n < 127; n++) w[n] = (unsigned char)s[n]; w[n] = 0;
    uint8_t buf[64]; memset(buf, 0, sizeof buf);
    *(void**)(buf + 0) = w; *(int32_t*)(buf + 8) = n + 1; *(int32_t*)(buf + 12) = n + 1;  // FString@0
    g_ProcessEvent(cdo_TextLib, fn_Conv, buf);            // ReturnValue FText @16
    memcpy(outFText, buf + 16, 16);
    return true;
}
static void set_text(void* comp, const char* s) {
    if (!fn_SetText || !comp) return;
    uint8_t ft[16]; if (!make_text(s, ft)) return;
    g_ProcessEvent(comp, fn_SetText, ft);                 // K2_SetText Value FText @0
}
static void* local_controller() {
    void* w = find_world(); if (!w || O_GI_LP < 0 || O_LP_PC < 0) return nullptr;
    void* gi = *(void**)((uint8_t*)w + O_WORLD_GI); if (!addr_readable((uintptr_t)gi)) return nullptr;
    void* lpData = *(void**)((uint8_t*)gi + O_GI_LP);
    int32_t num = *(int32_t*)((uint8_t*)gi + O_GI_LP + 8);
    if (!addr_readable((uintptr_t)lpData) || num < 1) return nullptr;
    void* lp = ((void**)lpData)[0]; if (!addr_readable((uintptr_t)lp)) return nullptr;
    void* pc = *(void**)((uint8_t*)lp + O_LP_PC);
    if (!addr_readable((uintptr_t)pc)) pc = resolve_local_pc(lp);   // lobby-switch fallback (LP->PC null)
    return addr_readable((uintptr_t)pc) ? pc : nullptr;
}
static void menu_spawn(void* me) {
    if (g_textactor || !fn_BeginSpawn || !fn_FinishSpawn || !c_TextActor || !cdo_GStatics) return;
    void* world = find_world(); if (!world) return;
    FVec loc = aim_getloc(me); loc.z += 80.0;
    uint8_t b[160]; memset(b, 0, sizeof b);
    *(void**)(b + 0) = world;                             // WorldContextObject
    *(void**)(b + 8) = c_TextActor;                       // ActorClass
    // SpawnTransform (FTransform) @16: Quat(x16,y24,z32,w40) Transl(48,56,64) Scale(72,80,88)
    *(double*)(b + 40) = 1.0;
    *(double*)(b + 48) = loc.x; *(double*)(b + 56) = loc.y; *(double*)(b + 64) = loc.z;
    *(double*)(b + 72) = 1.0; *(double*)(b + 80) = 1.0; *(double*)(b + 88) = 1.0;
    b[112] = 1;                                           // CollisionHandling = AlwaysSpawn
    g_ProcessEvent(cdo_GStatics, fn_BeginSpawn, b);
    void* actor = *(void**)(b + 128);
    if (!actor || !addr_readable((uintptr_t)actor)) { LOG("spawn: begin -> %p", actor); return; }
    uint8_t f[128]; memset(f, 0, sizeof f);
    *(void**)(f + 0) = actor;
    *(double*)(f + 32) = 1.0;
    *(double*)(f + 64) = 1.0; *(double*)(f + 72) = 1.0; *(double*)(f + 80) = 1.0;
    g_ProcessEvent(cdo_GStatics, fn_FinishSpawn, f);
    g_textactor = actor;
    g_textcomp = (o_TextRender >= 0) ? rd_obj(actor, o_TextRender) : nullptr;
    char cn[48] = "?"; if (addr_readable((uintptr_t)g_textcomp)) obj_name(obj_class(g_textcomp), cn, sizeof cn);
    LOG("spawn: actor=%p textcomp=%p(%s)", actor, g_textcomp, cn);
    void* tc = g_textcomp ? g_textcomp : actor;
    if (fn_SetWorldSize) { struct { float s; } p{ 48.f }; g_ProcessEvent(tc, fn_SetWorldSize, &p); }
    if (fn_SetColor)     { struct { uint32_t c; } p{ 0xFF00FF00u }; g_ProcessEvent(tc, fn_SetColor, &p); } // green
    set_text(tc, "ratman4080");
}
// keep the text 150u in front of the camera, facing you, each tick
static void menu_tick() {
    if (!g_textactor || !g_textcomp) return;
    void* me = local_pawn(); if (!me || !addr_readable((uintptr_t)me)) return;
    // real head pose = the VR HeadCamera component's world transform (pawn actor sits at VR origin)
    if (o_Camera < 0) o_Camera = prop_offset(obj_class(me), "HeadCamera");   // was never resolved before!
    void* cam = (o_Camera >= 0) ? rd_obj(me, o_Camera) : nullptr;
    if (!cam || !addr_readable((uintptr_t)cam)) return;
    if (!fn_CompLoc) fn_CompLoc = find_func(obj_class(cam), "K2_GetComponentLocation");
    if (!fn_CompRot) fn_CompRot = find_func(obj_class(cam), "K2_GetComponentRotation");
    if (!fn_CompLoc || !fn_CompRot) return;
    FVec loc; { struct { FVec r; } p{}; g_ProcessEvent(cam, fn_CompLoc, &p); loc = p.r; }
    FRot rot; { struct { FRot r; } p{}; g_ProcessEvent(cam, fn_CompRot, &p); rot = p.r; }
    double yr = rot.yaw * 0.01745329252, pr = rot.pitch * 0.01745329252;
    double fx = cos(pr) * cos(yr), fy = cos(pr) * sin(yr), fz = sin(pr);
    FVec pos{ loc.x + fx * 60.0, loc.y + fy * 60.0, loc.z + fz * 60.0 };
    // anchor the text to the held gun (always in view, in your hand) + a little up
    void* gun = get_item(me, c_Gun);
    if (!gun || !addr_readable((uintptr_t)gun)) gun = get_item(me, c_VRGun);
    if (gun && addr_readable((uintptr_t)gun) && in_lib(*(uintptr_t*)gun)) {
        FVec gp = aim_getloc(gun); if (!(gp.x == 0 && gp.y == 0)) { pos = gp; pos.z += 20.0; }
    }
    void* tc = g_textcomp ? g_textcomp : g_textactor;   // re-apply size/color each tick (in case they reset)
    if (fn_SetWorldSize) { struct { float s; } p{ 12.f }; g_ProcessEvent(tc, fn_SetWorldSize, &p); }
    if (fn_SetColor)     { struct { uint32_t c; } p{ 0xFF00FF00u }; g_ProcessEvent(tc, fn_SetColor, &p); }
    static int mc = 0; if (mc++ < 10)
        LOG("menu_tick: cam=%p loc=(%.0f,%.0f,%.0f) pos=(%.0f,%.0f,%.0f) yaw=%.0f", cam, loc.x, loc.y, loc.z, pos.x, pos.y, pos.z, rot.yaw);
    if (fn_SetActorLoc) { uint8_t lb[256]; memset(lb, 0, sizeof lb);
        *(double*)(lb + 0) = pos.x; *(double*)(lb + 8) = pos.y; *(double*)(lb + 16) = pos.z;
        g_ProcessEvent(g_textactor, fn_SetActorLoc, lb); }
    if (fn_SetRot) { uint8_t rb[32]; memset(rb, 0, sizeof rb);
        FRot r{ 0, rot.yaw + 180.0, 0 }; *(FRot*)(rb) = r;
        g_ProcessEvent(g_textactor, fn_SetRot, rb); }
    const char* mode = "OFF";
    switch (g_cfg) { case 2: mode="CHAMS+NORECOIL"; break; case 3: mode="+SILENT AIM"; break;
        case 4: mode="FULL (rapid/ammo/speed)"; break; case 6: mode="ONLINE AIMBOT"; break;
        case 5: mode="HIDE DBG"; break; case 9: mode="SDK DUMP"; break; }
    char buf[96]; snprintf(buf, sizeof buf, "ratman4080\nmode %d: %s", g_cfg, mode);
    set_text(g_textcomp, buf);
}

// cache: is this class a SkeletalMeshComponent (for mesh-asset targeting of custom-skin enemies)
static bool cls_is_skelmesh(void* cls) {
    static void* cc[64]; static uint8_t cb[64]; static int cn = 0;
    if (!addr_readable((uintptr_t)cls)) return false;
    for (int i = 0; i < cn; i++) if (cc[i] == cls) return cb[i];
    char c[48]; obj_name(cls, c, sizeof c);
    bool b = strstr(c, "SkeletalMeshComponent") != nullptr;
    if (cn < 64) { cc[cn] = cls; cb[cn] = b; cn++; }
    return b;
}
static bool is_body_asset(void* mesh) {
    if (o_SkelAsset < 0) return false;
    void* a = rd_obj(mesh, o_SkelAsset);
    if (!a || !addr_readable((uintptr_t)a)) return false;
    char an[64]; obj_name(a, an, sizeof an);
    return strstr(an,"Hidden")||strstr(an,"Monster")||strstr(an,"Soldier")||strstr(an,"Shack")||
           strstr(an,"Aurora")||strstr(an,"Player")||strstr(an,"Character");
}
// FField type name via FFieldClass (FField::ClassPrivate@0x8 -> FFieldClass::Name@0x0)
static void field_type(void* f, char* out, size_t cap) {
    out[0] = 0;
    void* fc = *(void**)((uint8_t*)f + 0x8);
    if (!addr_readable((uintptr_t)fc)) return;
    fname_to_str(*(int32_t*)((uint8_t*)fc + 0x0), out, cap);
}
// ONE-SHOT full SDK dump: every UClass/ScriptStruct -> name:super, props(+off type name), funcs.
// Runs on the background thread; per-struct fault-guarded. Trigger: chams.txt = 9.
// find the object holding a "StaffWhitelist" (or VIP/admin) TArray<FString> and log every entry.
static void dump_whitelist() {
    const char* props[6] = { "StaffWhitelist", "AdminList", "VIPList", "Whitelist", "Admins", "Staff" };
    static void* chk[8192]; static int nchk = 0;   // per-class cache so prop_offset isn't spammed (persists across retries)
    int32_t n = objects_num();
    for (int32_t i = 0; i < n; i++) {
        void* o = object_at(i);
        if (!o || !addr_readable((uintptr_t)o) || !in_lib(*(uintptr_t*)o)) continue;
        char nm[64]; obj_name(o, nm, sizeof nm); if (!strncmp(nm, "Default__", 9)) continue;
        void* c = obj_class(o); if (!addr_readable((uintptr_t)c)) continue;
        bool seen = false; for (int k = 0; k < nchk; k++) if (chk[k] == c) { seen = true; break; }
        if (seen) continue; if (nchk < 8192) chk[nchk++] = c;
        for (int pi = 0; pi < 6; pi++) {
            int32_t off = prop_offset(c, props[pi]); if (off < 0) continue;
            char cn[64]; obj_name(c, cn, sizeof cn);
            void* aData = *(void**)((uint8_t*)o + off);
            int32_t aNum = *(int32_t*)((uint8_t*)o + off + 8);
            LOG("WHITELIST '%s' on class '%s' @%d count=%d", props[pi], cn, off, aNum);
            g_fguard = 1; if (!sigsetjmp(g_fjmp, 1)) {
                if (aData && addr_readable((uintptr_t)aData) && aNum > 0 && aNum < 300) {
                    for (int k = 0; k < aNum; k++) { uint8_t* el = (uint8_t*)aData + (size_t)k * 16;
                        char16_t* sd = *(char16_t**)el; int32_t sl = *(int32_t*)(el + 8);
                        char s[80] = ""; if (sd && addr_readable((uintptr_t)sd) && sl > 0 && sl < 79)
                            for (int j = 0; j < sl && sd[j]; j++) s[j] = (char)sd[j];
                        LOG("  [%d] '%s'", k, s);
                    }
                }
            } g_fguard = 0;
        }
    }
    LOG("WHITELIST dump done (scanned %d classes)", nchk);
}
// ============================================================================
//  SDK DUMPER v2 — a proper reflection dump modelled on AndUEDumper/CAuthDumper,
//  but IN-PROCESS (we already own GObjects/GNames/ProcessEvent). One trigger
//  (chams.txt=9 / menu) writes a full SDK/ tree to the app files dir:
//    SDK/Offsets.hpp   — module-relative globals + validated member offsets
//    SDK/Objects.txt   — every reflected object: index, lib+off, class, full path
//    SDK/SDK.hpp       — C++ enums/structs/classes w/ offset+size comments,
//                        UFunction param structs (in/out/ret flags), grouped by package
//    SDK/Metadata.json — structured: packages[]{enums,structs,classes,functions,params}
//    sdk_dump.txt      — legacy flat human summary (kept for old workflows)
// ============================================================================
#define SDK_DIR "/sdcard/Android/data/com.vankrupt.pavlov/files/SDK"
static const char* g_files = "/sdcard/Android/data/com.vankrupt.pavlov/files";

static int32_t prop_elemsize(void* p) { return addr_readable((uintptr_t)p+FPROP_ELEMSIZE_OFF) ? *(int32_t*)((uint8_t*)p+FPROP_ELEMSIZE_OFF) : 0; }
static int32_t prop_arraydim(void* p) { return addr_readable((uintptr_t)p+FPROP_ARRAYDIM_OFF) ? *(int32_t*)((uint8_t*)p+FPROP_ARRAYDIM_OFF) : 1; }
static uint64_t prop_flags(void* p)   { return addr_readable((uintptr_t)p+FPROP_FLAGS_OFF)    ? *(uint64_t*)((uint8_t*)p+FPROP_FLAGS_OFF) : 0; }
static void* prop_sub(void* p, int i) { void* a=(uint8_t*)p+FPROP_SUB_OFF+(size_t)i*8; return addr_readable((uintptr_t)a)?*(void**)a:nullptr; }
static void obj_outer(void* o, void** out) { *out = (o && addr_readable((uintptr_t)o+UOBJ_OUTER_OFF)) ? *(void**)((uint8_t*)o+UOBJ_OUTER_OFF) : nullptr; }

// outermost object = the UPackage (its name is the "/Script/X" or "/Game/.." path)
static void* package_of(void* o) {
    void* cur = o, *nx = nullptr;
    for (int i = 0; i < 64 && cur; i++) { obj_outer(cur, &nx); if (!nx) return cur; cur = nx; }
    return cur;
}
// short package name: last path segment of the package ("/Script/Engine" -> "Engine")
static void pkg_short(void* pkg, char* out, size_t cap) {
    char full[128]; obj_name(pkg, full, sizeof full);
    const char* s = strrchr(full, '/'); s = s ? s + 1 : full;
    strncpy(out, s[0] ? s : "Unknown", cap - 1); out[cap-1] = 0;
    for (char* q = out; *q; q++) if (!((*q>='A'&&*q<='Z')||(*q>='a'&&*q<='z')||(*q>='0'&&*q<='9')||*q=='_')) *q = '_';
}
// class prefix: 'A' for AActor-derived classes, 'U' otherwise. Cached (super-walk is not free).
static bool is_actor_class(void* cls) {
    static void* cc[8192]; static uint8_t cb[8192]; static int nc = 0;
    if (!cls || !addr_readable((uintptr_t)cls)) return false;
    for (int i = 0; i < nc; i++) if (cc[i] == cls) return cb[i];
    bool r = false; void* s = cls;
    for (int g = 0; g < 64 && addr_readable((uintptr_t)s); g++) {
        char n[48]; obj_name(s, n, sizeof n); if (!strcmp(n, "Actor")) { r = true; break; }
        void* sp = struct_super(s); if (!sp || sp == s) break; s = sp;
    }
    if (nc < 8192) { cc[nc] = cls; cb[nc] = r; nc++; }
    return r;
}
static char cls_prefix(void* cls) { return is_actor_class(cls) ? 'A' : 'U'; }

// C++-ish type string for a property, recursing through Array/Map/Set inners.
static void prop_typestr(void* p, char* out, size_t cap, int depth) {
    out[0] = 0;
    if (!p || !addr_readable((uintptr_t)p) || depth > 5) { strncpy(out, "uint8_t", cap-1); out[cap-1]=0; return; }
    char k[48]; field_type(p, k, sizeof k);
    char nm[80];
    if (!strcmp(k,"ObjectProperty")||!strcmp(k,"ObjectPtrProperty")) { void* c=prop_sub(p,0); obj_name(c,nm,sizeof nm); snprintf(out,cap,"class %c%s*", cls_prefix(c), nm[0]?nm:"Object"); }
    else if (!strcmp(k,"WeakObjectProperty"))  { void* c=prop_sub(p,0); obj_name(c,nm,sizeof nm); snprintf(out,cap,"TWeakObjectPtr<%c%s>", cls_prefix(c), nm[0]?nm:"Object"); }
    else if (!strcmp(k,"SoftObjectProperty"))  { void* c=prop_sub(p,0); obj_name(c,nm,sizeof nm); snprintf(out,cap,"TSoftObjectPtr<%c%s>", cls_prefix(c), nm[0]?nm:"Object"); }
    else if (!strcmp(k,"LazyObjectProperty"))  { void* c=prop_sub(p,0); obj_name(c,nm,sizeof nm); snprintf(out,cap,"TLazyObjectPtr<%c%s>", cls_prefix(c), nm[0]?nm:"Object"); }
    else if (!strcmp(k,"ClassProperty"))       { void* c=prop_sub(p,1); obj_name(c,nm,sizeof nm); snprintf(out,cap,"TSubclassOf<%c%s>", cls_prefix(c), nm[0]?nm:"Object"); }
    else if (!strcmp(k,"SoftClassProperty"))   { void* c=prop_sub(p,1); obj_name(c,nm,sizeof nm); snprintf(out,cap,"TSoftClassPtr<%c%s>", cls_prefix(c), nm[0]?nm:"Object"); }
    else if (!strcmp(k,"StructProperty"))      { void* c=prop_sub(p,0); obj_name(c,nm,sizeof nm); snprintf(out,cap,"struct F%s", nm[0]?nm:"Struct"); }
    else if (!strcmp(k,"ArrayProperty"))       { char in[96]; prop_typestr(prop_sub(p,0),in,sizeof in,depth+1); snprintf(out,cap,"TArray<%s>", in); }
    else if (!strcmp(k,"SetProperty"))         { char in[96]; prop_typestr(prop_sub(p,0),in,sizeof in,depth+1); snprintf(out,cap,"TSet<%s>", in); }
    else if (!strcmp(k,"MapProperty"))         { char kk[96],vv[96]; prop_typestr(prop_sub(p,0),kk,sizeof kk,depth+1); prop_typestr(prop_sub(p,1),vv,sizeof vv,depth+1); snprintf(out,cap,"TMap<%s, %s>", kk, vv); }
    else if (!strcmp(k,"EnumProperty"))        { void* e=prop_sub(p,1); obj_name(e,nm,sizeof nm); snprintf(out,cap,"%s", nm[0]?nm:"Enum"); }
    else if (!strcmp(k,"ByteProperty"))        { void* e=prop_sub(p,0); if(e&&addr_readable((uintptr_t)e)){obj_name(e,nm,sizeof nm);snprintf(out,cap,"TEnumAsByte<%s>",nm);} else strncpy(out,"uint8_t",cap-1); out[cap-1]=0; }
    else if (!strcmp(k,"InterfaceProperty"))   { void* c=prop_sub(p,0); obj_name(c,nm,sizeof nm); snprintf(out,cap,"TScriptInterface<I%s>", nm[0]?nm:"face"); }
    else if (!strcmp(k,"BoolProperty"))        strncpy(out,"bool",cap-1),out[cap-1]=0;
    else if (!strcmp(k,"FloatProperty"))       strncpy(out,"float",cap-1),out[cap-1]=0;
    else if (!strcmp(k,"DoubleProperty"))      strncpy(out,"double",cap-1),out[cap-1]=0;
    else if (!strcmp(k,"IntProperty"))         strncpy(out,"int32_t",cap-1),out[cap-1]=0;
    else if (!strcmp(k,"Int64Property"))       strncpy(out,"int64_t",cap-1),out[cap-1]=0;
    else if (!strcmp(k,"Int16Property"))       strncpy(out,"int16_t",cap-1),out[cap-1]=0;
    else if (!strcmp(k,"Int8Property"))        strncpy(out,"int8_t",cap-1),out[cap-1]=0;
    else if (!strcmp(k,"UInt64Property"))      strncpy(out,"uint64_t",cap-1),out[cap-1]=0;
    else if (!strcmp(k,"UInt32Property"))      strncpy(out,"uint32_t",cap-1),out[cap-1]=0;
    else if (!strcmp(k,"UInt16Property"))      strncpy(out,"uint16_t",cap-1),out[cap-1]=0;
    else if (!strcmp(k,"NameProperty"))        strncpy(out,"FName",cap-1),out[cap-1]=0;
    else if (!strcmp(k,"StrProperty"))         strncpy(out,"FString",cap-1),out[cap-1]=0;
    else if (!strcmp(k,"TextProperty"))        strncpy(out,"FText",cap-1),out[cap-1]=0;
    else if (!strcmp(k,"DelegateProperty"))    strncpy(out,"FScriptDelegate",cap-1),out[cap-1]=0;
    else if (strstr(k,"Multicast"))            strncpy(out,"FMulticastScriptDelegate",cap-1),out[cap-1]=0;
    else if (!strcmp(k,"FieldPathProperty"))   strncpy(out,"FFieldPath",cap-1),out[cap-1]=0;
    else { snprintf(out, cap, "%.*s", (int)cap-1, k[0]?k:"uint8_t"); }   // fallback: raw kind
}
// JSON string escaper (into a fixed buffer)
static void json_esc(const char* s, char* out, size_t cap) {
    size_t w=0; for (const char* p=s; *p && w+2<cap; p++) {
        char c=*p; if (c=='"'||c=='\\'){ out[w++]='\\'; out[w++]=c; }
        else if (c=='\n'){ out[w++]='\\'; out[w++]='n'; }
        else if ((unsigned char)c<0x20){ /* skip control */ }
        else out[w++]=c;
    } out[w]=0;
}

// enum -> "Name=Value" pairs (UEnum::Names : TArray<TPair<FName,int64>>, 16B stride)
static int dump_enum(FILE* hpp, FILE* js, void* e, bool jsfirst) {
    void* names = *(void**)((uint8_t*)e + UENUM_NAMES_OFF);
    int32_t cnt = *(int32_t*)((uint8_t*)e + UENUM_NAMES_OFF + 8);
    char en[80]; obj_name(e, en, sizeof en);
    if (!addr_readable((uintptr_t)names) || cnt < 1 || cnt > 4096) return 0;
    if (hpp) fprintf(hpp, "enum class %s : uint8_t {\n", en);   // reflected UEnum name already carries the E prefix
    if (js)  fprintf(js, "%s\n      {\"name\":\"%s\",\"members\":[", jsfirst?"":",", en);
    for (int i = 0; i < cnt; i++) {
        uint8_t* pair = (uint8_t*)names + (size_t)i * 16;
        if (!addr_readable((uintptr_t)pair + 16)) break;
        char full[96]; fname_to_str(*(int32_t*)pair, full, sizeof full);
        int64_t val = *(int64_t*)(pair + 8);
        const char* mn = strrchr(full, ':'); mn = mn ? mn + 1 : full;   // strip "EName::"
        if (hpp) fprintf(hpp, "    %-40s = %lld,\n", mn, (long long)val);
        if (js)  fprintf(js, "%s{\"name\":\"%s\",\"value\":%lld}", i?",":"", mn, (long long)val);
    }
    if (hpp) fprintf(hpp, "};\n\n");
    if (js)  fprintf(js, "]}");
    return 1;
}

// one UStruct/UClass: fields (offset,size,type,name) + UFunctions (flags, param struct with in/out/ret)
static void dump_struct(FILE* hpp, FILE* js, void* o, bool is_class, bool jsfirst) {
    char nm[80]; obj_name(o, nm, sizeof nm);
    void* sup = struct_super(o); char sn[80] = "";
    if (addr_readable((uintptr_t)sup)) obj_name(sup, sn, sizeof sn);
    int32_t size = addr_readable((uintptr_t)o+USTRUCT_PROPSIZE) ? *(int32_t*)((uint8_t*)o+USTRUCT_PROPSIZE) : 0;
    char pfx = is_class ? cls_prefix(o) : 'F';
    char spfx = is_class ? (sn[0] ? cls_prefix(sup) : 'U') : 'F';
    if (hpp) {
        if (sn[0]) fprintf(hpp, "// 0x%x bytes\nstruct %c%s : public %c%s {\n", size, pfx, nm, spfx, sn);
        else       fprintf(hpp, "// 0x%x bytes\nstruct %c%s {\n", size, pfx, nm);
    }
    if (js) fprintf(js, "%s\n      {\"name\":\"%s\",\"kind\":\"%s\",\"super\":\"%s\",\"size\":%d,\"fields\":[", jsfirst?"":",", nm, is_class?"class":"struct", sn, size);
    // ---- fields ----
    int fi = 0;
    g_fguard = 1;
    if (!sigsetjmp(g_fjmp, 1)) {
        int guard = 0;
        for (void* p = *(void**)((uint8_t*)o + USTRUCT_CHILDPROPS);
             addr_readable((uintptr_t)p) && guard++ < 16384;
             p = *(void**)((uint8_t*)p + FFIELD_NEXT_OFF)) {
            char pn[80]; fname_to_str(*(int32_t*)((uint8_t*)p + FFIELD_NAME_OFF), pn, sizeof pn);
            char tp[112]; prop_typestr(p, tp, sizeof tp, 0);
            int32_t off = *(int32_t*)((uint8_t*)p + FPROP_OFFSET_OFF);
            int32_t esz = prop_elemsize(p), dim = prop_arraydim(p); if (dim < 1) dim = 1;
            if (hpp) {
                if (dim > 1) fprintf(hpp, "    %-48s %s[%d];  // +0x%x (0x%x)\n", tp, pn, dim, off, esz*dim);
                else         fprintf(hpp, "    %-48s %s;  // +0x%x (0x%x)\n", tp, pn, off, esz);
            }
            if (js) { char et[128]; json_esc(tp,et,sizeof et);
                fprintf(js, "%s{\"name\":\"%s\",\"type\":\"%s\",\"offset\":%d,\"size\":%d}", fi?",":"", pn, et, off, esz*dim); }
            fi++;
        }
    }
    g_fguard = 0;
    if (js) fprintf(js, "],\"functions\":[");
    // ---- functions (classes only carry UFunctions in Children) ----
    int fnc = 0;
    g_fguard = 1;
    if (!sigsetjmp(g_fjmp, 1)) {
        int guard = 0;
        for (void* c = *(void**)((uint8_t*)o + USTRUCT_CHILDREN);
             addr_readable((uintptr_t)c) && guard++ < 16384;
             c = *(void**)((uint8_t*)c + UFIELD_NEXT_OFF)) {
            char fnm[80]; obj_name(c, fnm, sizeof fnm);
            uint32_t flags = addr_readable((uintptr_t)c+UFUNC_FLAGS_OFF) ? *(uint32_t*)((uint8_t*)c+UFUNC_FLAGS_OFF) : 0;
            uintptr_t func = addr_readable((uintptr_t)c+UFUNC_FUNC_OFF) ? *(uintptr_t*)((uint8_t*)c+UFUNC_FUNC_OFF) : 0;
            uintptr_t funcrel = (func && in_lib(func)) ? func - g_base : 0;
            // walk params (ChildProperties of the UFunction) -> in / out / return
            char ret[112] = "void"; char params[512] = ""; size_t pw = 0;
            if (js) fprintf(js, "%s{\"name\":\"%s\",\"flags\":\"0x%x\",\"native\":\"lib+0x%lx\",\"params\":[",
                            fnc?",":"", fnm, flags, (unsigned long)funcrel);
            int pj = 0;   // params walk stays under the SAME fault-guard as the functions loop (no nested setjmp)
            int g2 = 0;
            for (void* pp = *(void**)((uint8_t*)c + USTRUCT_CHILDPROPS);
                 addr_readable((uintptr_t)pp) && g2++ < 256;
                 pp = *(void**)((uint8_t*)pp + FFIELD_NEXT_OFF)) {
                char ppn[80]; fname_to_str(*(int32_t*)((uint8_t*)pp + FFIELD_NAME_OFF), ppn, sizeof ppn);
                char pt[112]; prop_typestr(pp, pt, sizeof pt, 0);
                uint64_t pf = prop_flags(pp);
                const char* dir = (pf & CPF_ReturnParm) ? "ret" : (pf & CPF_OutParm) ? "out" : "in";
                if (pf & CPF_ReturnParm) { strncpy(ret, pt, sizeof ret-1); ret[sizeof ret-1]=0; }
                else if (pw < sizeof params - 96)
                    pw += snprintf(params+pw, sizeof params-pw, "%s%s%s %s",
                                   pw?", ":"", (pf&CPF_OutParm)?"/*out*/":"", pt, ppn);
                if (js) { char et[128]; json_esc(pt,et,sizeof et);
                    fprintf(js, "%s{\"name\":\"%s\",\"type\":\"%s\",\"dir\":\"%s\"}", pj?",":"", ppn, et, dir); pj++; }
            }
            if (hpp) fprintf(hpp, "    %-20s %s(%s);  // Function flags=0x%x  [lib+0x%lx]\n",
                             ret, fnm, params, flags, (unsigned long)funcrel);
            if (js) fprintf(js, "]}");
            fnc++;
        }
    }
    g_fguard = 0;
    if (hpp) fprintf(hpp, "};\n\n");
    if (js) fprintf(js, "]}");
}

// full-path name: "Class /Script/Pkg.Outer.Object"
static void obj_fullname(void* o, char* out, size_t cap) {
    char cls[48]; obj_name(obj_class(o), cls, sizeof cls);
    char chain[256] = ""; size_t w = 0; void* parts[16]; int np = 0; void* cur = o, *nx;
    for (int i = 0; i < 16 && cur; i++) { parts[np++] = cur; obj_outer(cur, &nx); cur = nx; }
    for (int i = np - 1; i >= 0; i--) { char n[80]; obj_name(parts[i], n, sizeof n);
        w += snprintf(chain+w, sizeof chain-w, "%s%s", i==np-1?"":".", n); if (w >= sizeof chain-2) break; }
    snprintf(out, cap, "%s %s", cls, chain);
}

static void dump_sdk() {
    mkdir(SDK_DIR, 0777);
    char path[256];
    // ---- Offsets.hpp ----
    snprintf(path, sizeof path, "%s/Offsets.hpp", SDK_DIR);
    FILE* off = fopen(path, "w");
    if (off) {
        fprintf(off, "// Auto-generated by pavchams SDK dumper (in-process). Module-relative offsets.\n");
        fprintf(off, "#pragma once\n#include <cstdint>\nnamespace Offsets {\n");
        fprintf(off, "    constexpr uintptr_t GObjects     = 0x%lx;\n", g_gobjects ? (uintptr_t)g_gobjects - g_base : 0);
        fprintf(off, "    constexpr uintptr_t GNames       = 0x%lx;\n", g_name_blocks ? (uintptr_t)g_name_blocks - g_base : 0);
        fprintf(off, "    constexpr uintptr_t ProcessEvent = 0x%lx;\n", (unsigned long)g_pe_rel);
        fprintf(off, "    constexpr int FNameShift = %d;\n", g_name_shift);
        fprintf(off, "    // member offsets (UE5.1)\n");
        fprintf(off, "    constexpr int UObject_Class=0x%x, UObject_Name=0x%x, UObject_Outer=0x%x;\n", UOBJ_CLASS_OFF, UOBJ_NAME_OFF, UOBJ_OUTER_OFF);
        fprintf(off, "    constexpr int UStruct_Super=0x%x, UStruct_Children=0x%x, UStruct_ChildProps=0x%x, UStruct_PropSize=0x%x;\n", USTRUCT_SUPER_OFF, USTRUCT_CHILDREN, USTRUCT_CHILDPROPS, USTRUCT_PROPSIZE);
        fprintf(off, "    constexpr int FField_Next=0x%x, FField_Name=0x%x;\n", FFIELD_NEXT_OFF, FFIELD_NAME_OFF);
        fprintf(off, "    constexpr int FProp_ArrayDim=0x%x, FProp_ElemSize=0x%x, FProp_Flags=0x%x, FProp_Offset=0x%x, FProp_Sub=0x%x;\n", FPROP_ARRAYDIM_OFF, FPROP_ELEMSIZE_OFF, FPROP_FLAGS_OFF, FPROP_OFFSET_OFF, FPROP_SUB_OFF);
        fprintf(off, "    constexpr int UFunction_Flags=0x%x, UFunction_ParmsSize=0x%x, UFunction_Func=0x%x;\n", UFUNC_FLAGS_OFF, UFUNC_PARMSIZE_OFF, UFUNC_FUNC_OFF);
        fprintf(off, "    constexpr int UEnum_Names=0x%x;\n}\n", UENUM_NAMES_OFF);
        fclose(off);
    }
    // ---- Objects.txt ----
    int32_t n = objects_num();
    snprintf(path, sizeof path, "%s/Objects.txt", SDK_DIR);
    FILE* obj = fopen(path, "w");
    if (obj) {
        for (int32_t i = 0; i < n; i++) {
            void* o = object_at(i);
            if (!o || !addr_readable((uintptr_t)o)) continue;
            g_fguard = 1; if (sigsetjmp(g_fjmp,1)) { g_fguard = 0; continue; }
            char fn[320]; obj_fullname(o, fn, sizeof fn);
            uintptr_t rel = in_lib(*(uintptr_t*)o) ? 0 : 0;
            fprintf(obj, "[%06d] %s\n", i, fn);
            g_fguard = 0;
            if ((i & 0x3ff) == 0) fflush(obj);
        }
        fclose(obj);
    }
    // ---- SDK.hpp + Metadata.json + legacy sdk_dump.txt (grouped by package) ----
    snprintf(path, sizeof path, "%s/SDK.hpp", SDK_DIR);         FILE* hpp = fopen(path, "w");
    snprintf(path, sizeof path, "%s/Metadata.json", SDK_DIR);   FILE* js  = fopen(path, "w");
    snprintf(path, sizeof path, "%s/sdk_dump.txt", g_files);     FILE* leg = fopen(path, "w");
    if (hpp) fprintf(hpp, "// Auto-generated in-process SDK. Types are best-effort reflected layouts.\n#pragma once\n#include <cstdint>\n\n");
    if (js)  fprintf(js, "{\n  \"game\":\"PavlovShack\",\"engine\":\"UE5.1\",\"packages\":[");
    // discover unique packages
    static void* pkgs[4096]; static char pkgnm[4096][48]; int npkg = 0;
    for (int32_t i = 0; i < n && npkg < 4096; i++) {
        void* o = object_at(i); if (!o || !addr_readable((uintptr_t)o)) continue;
        g_fguard = 1; if (sigsetjmp(g_fjmp,1)) { g_fguard = 0; continue; }
        void* pk = package_of(o); g_fguard = 0;
        if (!pk) continue; bool seen = false;
        for (int k = 0; k < npkg; k++) if (pkgs[k] == pk) { seen = true; break; }
        if (!seen) { pkgs[npkg] = pk; pkg_short(pk, pkgnm[npkg], sizeof pkgnm[npkg]); npkg++; }
    }
    int total_structs = 0, total_enums = 0;
    for (int pi = 0; pi < npkg; pi++) {
        if (hpp) fprintf(hpp, "\n// ======================= package: %s =======================\n", pkgnm[pi]);
        if (js)  fprintf(js, "%s\n    {\"name\":\"%s\",\"enums\":[", pi?",":"", pkgnm[pi]);
        bool jse_first = true;
        // enums first
        for (int32_t i = 0; i < n; i++) {
            void* o = object_at(i); if (!o || !addr_readable((uintptr_t)o)) continue;
            g_fguard = 1; if (sigsetjmp(g_fjmp,1)) { g_fguard = 0; continue; }
            void* cls = obj_class(o); char cn[48]; obj_name(cls, cn, sizeof cn);
            bool isEnum = !strcmp(cn, "Enum") || !strcmp(cn, "UserDefinedEnum");
            void* pk = isEnum ? package_of(o) : nullptr; g_fguard = 0;
            if (!isEnum || pk != pkgs[pi]) continue;
            if (dump_enum(hpp, js, o, jse_first)) { jse_first = false; total_enums++; }
        }
        if (js) fprintf(js, "],\"structs\":[");
        bool jss_first = true;
        // structs + classes
        for (int32_t i = 0; i < n; i++) {
            void* o = object_at(i); if (!o || !addr_readable((uintptr_t)o)) continue;
            g_fguard = 1; if (sigsetjmp(g_fjmp,1)) { g_fguard = 0; continue; }
            void* cls = obj_class(o); char cn[48]; obj_name(cls, cn, sizeof cn);
            bool isClass  = !strcmp(cn,"Class") || !strcmp(cn,"BlueprintGeneratedClass");
            bool isStruct = !strcmp(cn,"ScriptStruct");
            char nm[80]; obj_name(o, nm, sizeof nm);
            bool skip = !strncmp(nm, "Default__", 9);
            void* pk = (isClass||isStruct) && !skip ? package_of(o) : nullptr; g_fguard = 0;
            if ((!isClass && !isStruct) || skip || pk != pkgs[pi]) continue;
            dump_struct(hpp, js, o, isClass, jss_first); jss_first = false; total_structs++;
            if (leg) {   // legacy flat line
                void* sup = struct_super(o); char sn[80]=""; if (addr_readable((uintptr_t)sup)) obj_name(sup, sn, sizeof sn);
                fprintf(leg, "%s%s : %s  [%s]\n", isClass?"class ":"struct ", nm, sn, pkgnm[pi]);
            }
            if ((total_structs & 0x1f) == 0 && hpp) fflush(hpp);
        }
        if (js) fprintf(js, "]}");
    }
    if (js)  fprintf(js, "\n  ]\n}\n");
    if (hpp) fclose(hpp);
    if (js)  fclose(js);
    if (leg) fclose(leg);
    LOG("SDK dump complete: %d packages, %d structs/classes, %d enums -> %s/", npkg, total_structs, total_enums, SDK_DIR);
}
// BACKGROUND thread: heavy scan -> validated enemy pawn list. Off the game thread (no VR hitch);
// game thread re-validates each before touching it (no stale-pointer corruption).
static void scan_bots() {
    if (!g_ready) return;
    void* me = local_pawn();
    g_myteam = (me && o_TeamId >= 0 && addr_readable((uintptr_t)me)) ? *(int32_t*)((uint8_t*)me + o_TeamId) : -1;
    g_fguard = 1; if (sigsetjmp(g_fjmp, 1)) { g_fguard = 0; return; }
    int32_t n = objects_num(); int c = 0;
    for (int32_t i = 0; i < n && c < 128; i++) {
        void* o = object_at(i);
        if (!o || !addr_readable((uintptr_t)o) || o == me || !in_lib(*(uintptr_t*)o)) continue;
        if (!cls_is_body(obj_class(o))) continue;
        char nm[40]; obj_name(o, nm, sizeof nm);
        if (!strncmp(nm, "Default__", 9)) continue;
        if (pawn_dead(o)) continue;                        // skip corpses (frozen ragdoll chams)
        g_bots[c++] = o;
    }
    g_nbots = c;
    g_fguard = 0;
}
// GAME THREAD: cheap — re-validate each cached pawn FRESH (guards stale/reused pointers) + apply.
static void chams_pass() {
    if (!g_ready) return;
    g_cfg = cfg_value();
    // "Refresh mods" (menu): drop every per-feature cache so guns/chams/movement re-resolve from scratch.
    if (g_mei.act_refresh) { g_mei.act_refresh = false;
        g_gunGen++; g_heldGun = nullptr; g_ammoGun = nullptr;                       // guns re-resolve
        g_mc = nullptr; g_mcPawn = nullptr; g_walkComp = nullptr;                   // movement re-resolves
        g_mSprint = g_mAds = g_mWalk = g_mCrouch = -1; g_mSprintOrig = g_mAdsOrig = -1.f; g_walkOrig = g_crouchOrig = 0.f;
        g_fguard = 1; if (!sigsetjmp(g_fjmp, 1)) cham_restore_all(); g_fguard = 0;  // clear chams overrides
        g_xray0 = g_xray1 = g_xray_mat = nullptr;                                   // reload materials fresh
        g_myPS = nullptr; g_hc = nullptr; g_aim_target = nullptr;
        g_localPC = nullptr;
        LOG("REFRESH: all mod caches dropped");
    }
    // FIX PAWN: re-resolve pawn-dependent features (gun / movement / controller) without touching chams.
    // Press after a death or lobby switch if the kill list / gun mods / speed stop responding.
    if (g_mei.act_fixpawn) { g_mei.act_fixpawn = false;
        g_gunGen++; g_heldGun = nullptr; g_ammoGun = nullptr;                       // gun mods re-resolve
        g_mc = nullptr; g_mcPawn = nullptr; g_walkComp = nullptr;                   // movement re-resolves
        g_mSprint = g_mAds = g_mWalk = g_mCrouch = -1; g_mSprintOrig = g_mAdsOrig = -1.f; g_walkOrig = g_crouchOrig = 0.f;
        g_myPS = nullptr; g_hc = nullptr;                                           // name/devtag re-resolve
        g_localPC = nullptr;                                                        // lobby-switch controller re-find
        LOG("FIXPAWN: pawn-dependent caches dropped");
    }
    bool on = g_mei.master_enabled && g_mei.chams_enabled;   // chams gate (menu-driven)
    bool hide = false;                                       // hide-test path retired
    // NAME SPOOF: menu (g_mei.name_text) is primary; name.txt kept as an adb fallback. Applied
    // CLIENT-SIDE via in-place write (devtag block). No ServerChangeName RPC (server forces it back).
    {
        char want[64] = {0};
        if (g_mei.name_enabled && g_mei.name_text[0]) {
            strncpy(want, g_mei.name_text, sizeof want - 1);
        } else {
            FILE* nf = fopen("/sdcard/Android/data/com.vankrupt.pavlov/files/name.txt", "r");
            if (nf) { if (fgets(want, sizeof want, nf)) { int L = (int)strlen(want);
                while (L > 0 && (want[L-1]=='\n'||want[L-1]=='\r')) want[--L] = 0; } fclose(nf); }
        }
        g_wantName[0] = 0;
        if (want[0]) strncpy(g_wantName, want, sizeof g_wantName - 1);
    }
    // CHAMS REFRESH: every few seconds drop the cached xray materials so a GC that collected them
    // (heavy death/respawn/map change) is recovered — force_load_xray reloads them fresh next line.
    { static long last_ref = 0; struct timespec rt; clock_gettime(CLOCK_MONOTONIC, &rt);
      long rms = rt.tv_sec * 1000 + rt.tv_nsec / 1000000;
      if (rms - last_ref > 4000) { last_ref = rms; g_xray0 = g_xray1 = g_xray_mat = nullptr; } }
    g_fguard = 1; if (!sigsetjmp(g_fjmp, 1)) force_load_xray(); g_fguard = 0;
    void* me = local_pawn();
    g_localMe = me;   // publish for the 90Hz continuous-aim path (so it never calls find_world itself)
    // POST-DEATH RECOVERY: when the pawn changes (respawn/map), every per-pawn cache is stale. Drop them
    // so movement/name/aim re-resolve for the NEW pawn instead of silently no-op'ing on dead pointers.
    { static void* last_me = nullptr;
      if (me && me != last_me) {
          g_mc = nullptr; g_mcPawn = nullptr; g_walkComp = nullptr;   // movement re-resolves
          g_myPS = nullptr; g_hc = nullptr; g_aim_target = nullptr;   // devtag/name/aim re-resolve
          last_me = me;
      } }
    // dev tag (self-view only): force bDev=1 on our own PlayerState (guarded; resolves offset once).
    if (me && addr_readable((uintptr_t)me)) {
        g_fguard = 1; if (!sigsetjmp(g_fjmp, 1)) {
            static int32_t o_ps = -2; static void* devCls = nullptr;
            if (o_ps == -2) o_ps = prop_offset(obj_class(me), "PlayerState");
            if (o_ps >= 0) { void* ps = *(void**)((uint8_t*)me + o_ps);
                if (ps && addr_readable((uintptr_t)ps) && in_lib(*(uintptr_t*)ps)) {
                    void* pc = obj_class(ps);
                    if (pc != devCls) { devCls = pc; g_devOff = prop_offset(pc, "bDev");   // re-resolve on class change only
                        LOG("devtag: PlayerState=%p class-changed bDev@%d", ps, g_devOff); }
                    g_myPS = ps; if (g_devOff >= 0) *(uint8_t*)((uint8_t*)ps + g_devOff) = (g_mei.master_enabled && g_mei.dev_tag) ? 1 : 0;
                    // VOTE UNLOCK: force bCanVote so the vote button shows even in modes that disable it
                    static int32_t o_vote = -2; static void* voteCls = nullptr;
                    if (pc != voteCls) { voteCls = pc; o_vote = prop_offset(pc, "bCanVote"); }
                    if (o_vote >= 0 && g_mei.master_enabled && g_mei.force_vote) *(uint8_t*)((uint8_t*)ps + o_vote) = 1;
                    // our own TTT role (client always knows its own) -> menu TTT tab
                    if (o_PS_Role >= 0) { int32_t rid = *(int32_t*)((uint8_t*)ps + o_PS_Role);
                        if (rid > 0) fname_to_str(rid, g_mei.my_role, sizeof g_mei.my_role); }
                    // NAME SPOOF: in-place write ONLY if our string fits the game's existing FString
                    // buffer (never change the Data pointer -> game still owns/frees it -> no crash).
                    static int32_t o_pn = -2; if (o_pn == -2) o_pn = prop_offset(pc, "PlayerNamePrivate");
                    if (o_pn >= 0 && g_wantName[0]) {
                        char16_t* d = *(char16_t**)((uint8_t*)ps + o_pn);
                        int32_t mx = *(int32_t*)((uint8_t*)ps + o_pn + 12);
                        int wlen = 0; while (g_wantName[wlen] && wlen < 62) wlen++;
                        if (d && addr_readable((uintptr_t)d) && mx >= wlen + 1) {   // fits existing buffer
                            for (int i = 0; i < wlen; i++) d[i] = (unsigned char)g_wantName[i];
                            d[wlen] = 0; *(int32_t*)((uint8_t*)ps + o_pn + 8) = wlen + 1;   // ArrayNum
                        }
                    }
                    if (o_pn >= 0) {   // log the current name only when it actually CHANGES (no per-second spam)
                        char16_t* d = *(char16_t**)((uint8_t*)ps + o_pn);
                        int32_t cnt = *(int32_t*)((uint8_t*)ps + o_pn + 8);
                        char nm[64] = ""; if (d && addr_readable((uintptr_t)d) && cnt > 0 && cnt < 63)
                            for (int i = 0; i < cnt && d[i]; i++) nm[i] = (char)d[i];
                        static char nlast[64] = {0};
                        if (strcmp(nm, nlast)) { strncpy(nlast, nm, sizeof nlast - 1); LOG("NAME: '%s' (@%d cnt=%d)", nm, o_pn, cnt); }
                    }
                    // MYIDENT: log OUR local identity as the server has it — name + platform id + platform enum.
                    { static int32_t o_plat = -2, o_platEnum = -2;
                      if (o_plat == -2) o_plat = prop_offset(pc, "PlatformId");
                      if (o_platEnum == -2) o_platEnum = prop_offset(pc, "PlayerPlatform");
                      char pid[64] = "";
                      if (o_plat >= 0) { char16_t* pd = *(char16_t**)((uint8_t*)ps + o_plat);
                          int32_t pc2 = *(int32_t*)((uint8_t*)ps + o_plat + 8);
                          if (pd && addr_readable((uintptr_t)pd) && pc2 > 0 && pc2 < 63)
                              for (int i = 0; i < pc2 && pd[i]; i++) pid[i] = (char)pd[i]; }
                      int penum = (o_platEnum >= 0) ? *(uint8_t*)((uint8_t*)ps + o_platEnum) : -1;
                      char myname[64] = "";
                      if (o_pn >= 0) { char16_t* nd = *(char16_t**)((uint8_t*)ps + o_pn);
                          int32_t nc = *(int32_t*)((uint8_t*)ps + o_pn + 8);
                          if (nd && addr_readable((uintptr_t)nd) && nc > 0 && nc < 63)
                              for (int i = 0; i < nc && nd[i]; i++) myname[i] = (char)nd[i]; }
                      static char last[192] = {0}; char line[192];
                      snprintf(line, sizeof line, "name='%s' platformId='%s' platformEnum=%d (nameOff=%d platOff=%d)", myname, pid, penum, o_pn, o_plat);
                      if (strcmp(line, last)) { strncpy(last, line, sizeof last - 1); LOG("MYIDENT %s", line); } }
                }
            }
        } g_fguard = 0;
    }
    // (in-headset both-grips toggle removed: two-handing a rifle triggered it and cycled to OFF)
    // weapon/movement/god pass. Run while any feature is ON (apply), PLUS a one-shot pass the first
    // time they all go OFF (restore). NEVER run it continuously with everything off — that re-triggers
    // the movecomp object-scan on every respawn and tanks FPS after dying.
    bool anyFeat = g_mei.master_enabled && (g_mei.no_recoil || g_mei.perfect_accuracy || g_mei.rapid_fire ||
                   g_mei.force_auto || g_mei.no_reload || g_mei.move_enabled || g_mei.godmode);
    static bool ranLast = false;
    if ((anyFeat || ranLast) && me) { g_fguard = 1; if (!sigsetjmp(g_fjmp, 1)) do_norecoil(me); g_fguard = 0; }
    ranLast = anyFeat;   // when all go off, we run ONE more restore pass next tick, then stop
    // movement runs on the pawn regardless of holding a weapon (own guard so a gun-less pawn still gets
    // speed). One extra pass after it goes OFF restores originals, then stops. (godmode removed.)
    static bool ranMove = false;
    bool wantMove = g_mei.master_enabled && (g_mei.move_enabled || g_mei.noclip);
    if ((wantMove || ranMove) && me) { g_fguard = 1; if (!sigsetjmp(g_fjmp, 1)) do_movement(me); g_fguard = 0; }
    ranMove = wantMove;
    g_fguard = 1; if (!sigsetjmp(g_fjmp, 1)) do_antiflash(); g_fguard = 0;   // anti-flash/smoke
    g_fguard = 1; if (!sigsetjmp(g_fjmp, 1)) do_infammo();  g_fguard = 0;   // infinite ammo (all-magazine sweep)
    // ESP restore: the moment chams turns OFF, put every overridden mesh back to its original material
    // (else the x-ray stays until respawn). One-shot: cham_restore_all clears the cache.
    if (!on) { if (g_nCham > 0) cham_restore_all(); return; }
    for (int i = 0; i < g_nbots; i++) {
        void* o = g_bots[i];
        if (!o || !addr_readable((uintptr_t)o) || !in_lib(*(uintptr_t*)o)) continue;
        if (!cls_is_body(obj_class(o))) continue;         // reused/stale-pointer guard
        if (pawn_dead(o)) continue;                       // dead -> stop forcing material on the corpse
        void* avatar = (o_Avatar >= 0) ? rd_obj(o, o_Avatar) : nullptr;   // read fresh
        if (!avatar || !addr_readable((uintptr_t)avatar) || !in_lib(*(uintptr_t*)avatar)) continue;
        int32_t team = (o_TeamId >= 0) ? *(int32_t*)((uint8_t*)o + o_TeamId) : 0;
        void* mat = nullptr;
        switch (g_mei.chams_style) {
            case 1: mat = g_xray1; break;                                        // Single-A (one color for all)
            case 2: mat = g_xray0; break;                                        // Single-B
            case 3: { struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);    // Flash (pulse both colors)
                      long ms = ts.tv_sec*1000 + ts.tv_nsec/1000000; mat = ((ms/220)&1) ? g_xray1 : g_xray0; } break;
            case 4: if (o != g_aim_target) continue; mat = (team==0)?g_xray0:g_xray1; break;  // Target-only
            case 5: mat = g_xray1; break;   // Custom color (tinted below via a dynamic instance)
            default: mat = (team == 0) ? g_xray1 : g_xray0; break;               // Team colors (swapped)
        }
        // CUSTOM COLOR: make (once) a dynamic instance of the xray material for this mesh and set its
        // "Color" param to the user's RGB. The MID is reused per mesh; SetMaterial below applies it.
        if (g_mei.chams_style == 5 && fn_CreateMID && fn_SetVecParam && is_material(g_xray1)) {
            g_fguard = 1; if (!sigsetjmp(g_fjmp, 1)) {
                void* mid = mid_for(avatar);
                if (!mid) { struct { int32_t idx; int32_t _p; void* src; int32_t nId, nNum; void* ret; }
                        cp{ 0, 0, g_xray1, 0, 0, nullptr };
                    g_ProcessEvent(avatar, fn_CreateMID, &cp); mid = cp.ret; if (mid) mid_store(avatar, mid); }
                if (mid && addr_readable((uintptr_t)mid) && in_lib(*(uintptr_t*)mid)) {
                    if (g_colorNameId < 0) g_colorNameId = fname_find("Color");
                    const float* col = (team == 0) ? g_mei.chams_col : g_mei.chams_col2;   // per-team custom color
                    struct { int32_t nId, nNum; float r, g, b, a; }
                        sp{ g_colorNameId, 0, col[0], col[1], col[2], 1.f };
                    g_ProcessEvent(mid, fn_SetVecParam, &sp); mat = mid; }
            } g_fguard = 0;
        }
        if (!is_material(mat)) mat = g_xray_mat;
        if (g_mei.chams_highlight && g_mei.chams_style != 4 && o == g_aim_target) {
            void* hl = (team == 0) ? g_xray0 : g_xray1; if (is_material(hl)) mat = hl; }
        if (!is_material(mat)) continue;
        g_fguard = 1;
        if (!sigsetjmp(g_fjmp, 1)) {
            if (hide && fn_SetVis) { struct { uint8_t v, p; } vp{ 0, 1 }; g_ProcessEvent(avatar, fn_SetVis, &vp); }
            else { cham_snapshot(avatar);                 // capture original ONCE before overriding
                   call_setmaterial(avatar, 0, mat); }    // single call; overlay is a no-op on mobile
        }
        g_fguard = 0;
    }
}

// one-shot: find every UFunction named *XRay* anywhere + its owning class (Outer)
static void dump_xray_api() {
    static bool done = false; if (done) return; done = true;
    LOG("--- XRAY UFUNCTIONS (any class) ---");
    int32_t n = objects_num();
    for (int32_t i = 0; i < n; i++) {
        void* o = object_at(i);
        if (!o || !addr_readable((uintptr_t)o)) continue;
        void* c = obj_class(o);
        char cn[48]; obj_name(c, cn, sizeof cn);
        if (strcmp(cn, "Function")) continue;
        char fn[80]; obj_name(o, fn, sizeof fn);
        if (!(strstr(fn,"XRay")||strstr(fn,"Xray"))) continue;
        void* outer = *(void**)((uint8_t*)o + 0x20);         // UObject::Outer
        char on[64] = "?"; if (addr_readable((uintptr_t)outer)) obj_name(outer, on, sizeof on);
        LOG("  UFUNC '%s' owner '%s'", fn, on);
    }
    // full signatures of the promising loaders
    const char* want[3] = { "LoadAsset_Blocking", "AsyncLoadUObject", "LoadClassAsset_Blocking" };
    for (int32_t i = 0; i < n; i++) {
        void* o = object_at(i);
        if (!o || !addr_readable((uintptr_t)o)) continue;
        char cn[48]; obj_name(obj_class(o), cn, sizeof cn);
        if (strcmp(cn, "Function")) continue;
        char fnm[80]; obj_name(o, fnm, sizeof fnm);
        bool m = false; for (int w = 0; w < 3; w++) if (!strcmp(fnm, want[w])) m = true;
        if (!m) continue;
        LOG("SIG %s:", fnm);
        for (void* p = *(void**)((uint8_t*)o + USTRUCT_CHILDPROPS); addr_readable((uintptr_t)p);
             p = *(void**)((uint8_t*)p + FFIELD_NEXT_OFF)) {
            char pn[64]; field_name(p, pn, sizeof pn);
            void* pc = *(void**)((uint8_t*)p + 0x8);          // FField::ClassPrivate (FFieldClass)
            char pcn[48] = "?"; if (addr_readable((uintptr_t)pc)) {
                int32_t nameidx = *(int32_t*)((uint8_t*)pc);  // FFieldClass name FName id at +0
                fname_to_str(nameidx, pcn, sizeof pcn); }
            LOG("   %s : %s @ %d", pn, pcn, *(int32_t*)((uint8_t*)p + FPROP_OFFSET_OFF));
        }
    }
    LOG("--- LOADER UFUNCTIONS ---");
    for (int32_t i = 0; i < n; i++) {
        void* o = object_at(i);
        if (!o || !addr_readable((uintptr_t)o)) continue;
        char cn[48]; obj_name(obj_class(o), cn, sizeof cn);
        if (strcmp(cn, "Function")) continue;
        char fn[80]; obj_name(o, fn, sizeof fn);
        if (!strstr(fn, "Load")) continue;
        if (!(strstr(fn,"Object")||strstr(fn,"Asset")||strstr(fn,"Material")||strstr(fn,"Class")||
              strstr(fn,"Synchronous")||strstr(fn,"Soft"))) continue;
        void* outer = *(void**)((uint8_t*)o + 0x20);
        char on[64] = "?"; if (addr_readable((uintptr_t)outer)) obj_name(outer, on, sizeof on);
        LOG("  LOADER '%s' owner '%s'", fn, on);
    }
    LOG("--- XRAY API on PavlovPawn ---");
    for (void* s = c_PavlovPawn; addr_readable((uintptr_t)s); s = struct_super(s)) {
        char sn[64]; obj_name(s, sn, sizeof sn);
        for (void* c = *(void**)((uint8_t*)s + USTRUCT_CHILDREN); addr_readable((uintptr_t)c);
             c = *(void**)((uint8_t*)c + UFIELD_NEXT_OFF)) {
            char fn[80]; obj_name(c, fn, sizeof fn);
            if (strstr(fn,"ray")||strstr(fn,"Ray")||strstr(fn,"XRay")||strstr(fn,"Vision")||
                strstr(fn,"Wall")||strstr(fn,"See")||strstr(fn,"Reveal")||strstr(fn,"Spectat")||
                strstr(fn,"Highlight")||strstr(fn,"Outline"))
                LOG("  FUNC %s::%s", sn, fn);
        }
        for (void* f = *(void**)((uint8_t*)s + USTRUCT_CHILDPROPS); addr_readable((uintptr_t)f);
             f = *(void**)((uint8_t*)f + FFIELD_NEXT_OFF)) {
            char pn[80]; field_name(f, pn, sizeof pn);
            if (strstr(pn,"ray")||strstr(pn,"Ray")||strstr(pn,"XRay")||strstr(pn,"Vision")||
                strstr(pn,"Wall")||strstr(pn,"Reveal")||strstr(pn,"Highlight"))
                LOG("  PROP %s::%s @ %d", sn, pn, *(int32_t*)((uint8_t*)f + FPROP_OFFSET_OFF));
        }
        if (!struct_super(s)) break;
    }
}
// scan a live player pawn's fields: log every offset holding a pointer to a live UObject + its class
static void dump_diag() {
    static bool done = false;
    if (done) return;                                   // latch: dump the first pawn we ever see
    void* pawn = nullptr;
    int32_t n = objects_num();
    for (int32_t i = 0; i < n && !pawn; i++) {
        void* o = object_at(i);
        if (!o || !addr_readable((uintptr_t)o)) continue;
        char on[48]; obj_name(o, on, sizeof on);
        if (!strncmp(on, "Default__", 9)) continue;
        if (is_a(o, c_PavlovPawn)) pawn = o;            // any PavlovPawn subclass instance
    }
    if (!pawn) return;
    done = true;
    char cn[64]; obj_name(obj_class(pawn), cn, sizeof cn);
    LOG("DIAG: fields of player pawn %p (class '%s'):", pawn, cn);
    int lines = 0;
    for (int off = 0x28; off <= 0x1300 && lines < 48; off += 8) {
        void* p = *(void**)((uint8_t*)pawn + off);
        if (!addr_readable((uintptr_t)p)) continue;
        void* pc = *(void**)((uint8_t*)p + UOBJ_CLASS_OFF);
        if (!addr_readable((uintptr_t)pc)) continue;
        void* pcc = *(void**)((uint8_t*)pc + UOBJ_CLASS_OFF);   // class-of-class must be a UClass too
        if (!addr_readable((uintptr_t)pcc)) continue;
        char pcn[64]; obj_name(pc, pcn, sizeof pcn);
        if (!printable(pcn)) continue;
        LOG("  +%d -> class '%s'", off, pcn);
        lines++;
    }
}

// homing knife: for ~1.5s after a throw, steer the thrown knife's velocity toward the nearest enemy.
static void knife_home() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    long ms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (ms - g_throwMs > 1500 || !addr_readable((uintptr_t)g_thrownKnife) ||
        !in_lib(*(uintptr_t*)g_thrownKnife)) { g_thrownKnife = nullptr; return; }
    g_fguard = 1; if (sigsetjmp(g_fjmp, 1)) { g_fguard = 0; return; }
    FVec kp = aim_getloc(g_thrownKnife);
    void* me = local_pawn();
    int32_t myteam = (me && o_TeamId >= 0 && addr_readable((uintptr_t)me)) ? *(int32_t*)((uint8_t*)me + o_TeamId) : -1;
    void* best = nullptr; double bd = 1e18; FVec bpos{};
    for (int i = 0; i < g_nbots; i++) { void* o = g_bots[i];
        if (!o || !addr_readable((uintptr_t)o) || o == me || !in_lib(*(uintptr_t*)o)) continue;
        if (!cls_is_body(obj_class(o)) || pawn_dead(o)) continue;
        if (!g_ffaMode && myteam >= 0 && o_TeamId >= 0 && *(int32_t*)((uint8_t*)o + o_TeamId) == myteam) continue;
        FVec p = aim_getloc(o); p.z += 40.0;
        double dx = p.x - kp.x, dy = p.y - kp.y, dz = p.z - kp.z, d = dx*dx + dy*dy + dz*dz;
        if (d < bd) { bd = d; best = o; bpos = p; }
    }
    if (best) {
        double dx = bpos.x - kp.x, dy = bpos.y - kp.y, dz = bpos.z - kp.z;
        double len = sqrt(dx*dx + dy*dy + dz*dz);
        if (len > 1.0) { double s = 2500.0 / len; FVec vel{ dx*s, dy*s, dz*s };
            aim_setrot(g_thrownKnife, look_at(kp, bpos));
            void* root = (o_RootComp >= 0) ? *(void**)((uint8_t*)g_thrownKnife + o_RootComp) : nullptr;
            if (root && addr_readable((uintptr_t)root)) set_phys_vel(root, vel);
        }
    }
    g_fguard = 0;
}
// ===========================================================================
//  mei menu input feed  (reflection owns engine access; math lives in mei_input.cpp)
// ===========================================================================
// Head pose from APlayerController::GetPlayerViewPoint; the aim ray from the held gun's transform
// (both already proven in this codebase). Both expressed in the SAME head-local frame downstream,
// so the cursor stays consistent with the view-space OpenXR quad — no world<->tracking calibration.
static void* fn_ViewPoint = nullptr;
static void rot_basis(double pitch, double yaw, float fwd[3], float right[3], float up[3]) {
    const double D2R = 0.017453292519943295;
    double cy = cos(yaw*D2R), sy = sin(yaw*D2R), cp = cos(pitch*D2R), sp = sin(pitch*D2R);
    fwd[0]=(float)(cp*cy); fwd[1]=(float)(cp*sy); fwd[2]=(float)sp;
    right[0]=(float)(-sy);  right[1]=(float)(cy);  right[2]=0.f;                 // UE Y (roll=0)
    // up = fwd x right  (=+Z at identity)
    up[0]=(float)(fwd[1]*right[2]-fwd[2]*right[1]);
    up[1]=(float)(fwd[2]*right[0]-fwd[0]*right[2]);
    up[2]=(float)(fwd[0]*right[1]-fwd[1]*right[0]);
}
static void mei_feed_input() {
    if (!g_ready) return;
    // ProcessEvent fires ~20k times/frame — do NOT syscall every call. Cheaply skip most, then a
    // real ~60 Hz clock gate on the survivors. (The old per-PE clock_gettime was the lag source.)
    static unsigned tick = 0; if ((++tick & 15) != 0) return;
    static long last = 0; struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    long ms = ts.tv_sec*1000 + ts.tv_nsec/1000000;
    if (ms - last < 16) return; last = ms;                   // ~60 Hz cap on the heavy reflected calls

    void* pc = local_controller();
    if (!pc || !addr_readable((uintptr_t)pc)) return;
    if (!fn_ViewPoint) fn_ViewPoint = find_func(obj_class(pc), "GetPlayerViewPoint");
    if (!fn_ViewPoint) return;
    // GetPlayerViewPoint(FVector& Location, FRotator& Rotation): Location@0 (3 doubles), Rotation@24
    // (pitch,yaw,roll doubles). UE5.1 = double precision.
    uint8_t p[64]; memset(p, 0, sizeof p);
    g_fguard = 1; if (sigsetjmp(g_fjmp, 1)) { g_fguard = 0; return; }
    g_ProcessEvent(pc, fn_ViewPoint, p); g_fguard = 0;
    double* L = (double*)(p + 0); double* R = (double*)(p + 24);

    float hpos[3] = { (float)L[0], (float)L[1], (float)L[2] };
    float hf[3], hr[3], hu[3];
    rot_basis(R[0], R[1], hf, hr, hu);

    // aim ray from the held gun (fall back to head-forward if no gun in hand)
    void* me = local_pawn(); void* gun = nullptr;
    if (me) { g_fguard = 1; if (!sigsetjmp(g_fjmp, 1)) {
        gun = get_item(me, c_Gun);
        if (!gun || !addr_readable((uintptr_t)gun)) gun = get_item(me, c_VRGun);
    } g_fguard = 0; }
    float cpos[3], cfwd[3];
    double gun_pitch = R[0];
    if (gun && addr_readable((uintptr_t)gun) && in_lib(*(uintptr_t*)gun)) {
        FVec gl{}; FRot gr{};
        g_fguard = 1; if (!sigsetjmp(g_fjmp, 1)) { gl = aim_getloc(gun); gr = aim_getrot(gun); } g_fguard = 0;
        cpos[0]=(float)gl.x; cpos[1]=(float)gl.y; cpos[2]=(float)gl.z;
        float rr[3], uu[3]; rot_basis(gr.pitch, gr.yaw, cfwd, rr, uu);
        gun_pitch = gr.pitch;
    } else {
        cpos[0]=hpos[0]; cpos[1]=hpos[1]; cpos[2]=hpos[2];
        cfwd[0]=hf[0]; cfwd[1]=hf[1]; cfwd[2]=hf[2];
    }

    mei_input_feed(hpos, hr, hu, hf, cpos, cfwd, false);   // click comes from the real trigger (mei_xr)

    // OPEN: primary is the LEFT STICK PRESS (L3), handled in mei_xr via our injected action set.
    // The up-point gesture stays ONLY as a fallback until those real buttons report live — so the
    // menu is still openable on first boot if bindings need tuning.
    if (!mei_xr_actions_live()) {
        static long up_since = 0; static bool armed = true;
        bool pointing_up = gun_pitch > 55.0;
        if (pointing_up) {
            if (up_since == 0) up_since = ms;
            if (armed && ms - up_since > 600) {
                g_mei.menu_open = !g_mei.menu_open; armed = false;
                LOG("mei: menu %s (up-gesture fallback)", g_mei.menu_open ? "OPEN" : "closed");
            }
        } else { up_since = 0; armed = true; }
    }
}

// resolve an enemy pawn's head (skull-socket) world position — for kill-aura reports.
static FVec get_head(void* o) {
    FVec head{};
    void* avatar = (o_Avatar >= 0) ? rd_obj(o, o_Avatar) : nullptr;
    if (!avatar || !addr_readable((uintptr_t)avatar) || !in_lib(*(uintptr_t*)avatar)) return head;
    uint64_t skull = 0;
    if (o_AvatarSkin >= 0) { void* skin = rd_obj(o, o_AvatarSkin);
        if (skin && addr_readable((uintptr_t)skin)) {
            if (o_SkullSocket < 0) o_SkullSocket = prop_offset(obj_class(skin), "SkullSocket");
            if (o_SkullSocket >= 0) skull = *(uint64_t*)((uint8_t*)skin + o_SkullSocket); } }
    if (skull && fn_SockLoc) head = aim_sockloc(avatar, skull);
    return head;
}

// TRIGGER-KILL: report a headshot bullet-hit on `target` to the server (client-authoritative hit-reg).
// FClientBulletHit (UE5.1 doubles, from the PC SDK): Target@0x00 Hit@0x08 bHeadshot@0x20 bPenetrated@0x21
// BulletClass@0x28 GunClass@0x30 Origin@0x38 BoneName(FName)@0x50 Timestamp@0x58 (size 0x60).
static void report_hit(void* target, FVec head, void* heldGun) {
    if (!fn_ReportHit || !target || !addr_readable((uintptr_t)target)) return;
    void* pc = local_controller();
    if (!pc || !addr_readable((uintptr_t)pc) || !in_lib(*(uintptr_t*)pc)) return;
    uint8_t p[0x80]; memset(p, 0, sizeof p);       // >= bh_Size, resolved-by-name offsets below
    *(void**)(p + bh_Target) = target;             // Target
    *(FVec*)(p + bh_Hit)     = head;               // Hit
    p[bh_Head] = 1;                                // bHeadshot
    p[bh_Pen]  = 1;                                // bPenetrated
    void* gunCls = c_KillGun;                      // strong default; fall back to the gun that fired
    if (!gunCls && heldGun && addr_readable((uintptr_t)heldGun)) gunCls = obj_class(heldGun);
    if (!gunCls) { static int wc = 0; if (wc++ < 4) LOG("report_hit: NO gun class -> skipped (server drops null-class reports)"); return; }  // never spam null-class reports
    if (c_KillBullet) *(void**)(p + bh_Bullet) = c_KillBullet;   // BulletClass
    *(void**)(p + bh_Gun) = gunCls;                              // GunClass
    *(FVec*)(p + bh_Origin) = head;                // Origin
    if (o_HeadBone >= 0) *(uint64_t*)(p + bh_Bone) = *(uint64_t*)((uint8_t*)target + o_HeadBone);  // BoneName
    static int logn = 0; if (logn++ < 8) LOG("report_hit: target=%p pc=%p gun=%p bullet=%p resolved=%d", target, pc, gunCls, c_KillBullet, bh_resolved);
    g_ProcessEvent(pc, fn_ReportHit, p);
}

// read an FString (char16 data@0, num@8) at `off` on `obj` into an ascii buffer
static void read_fstring(void* obj, int32_t off, char* out, int cap) {
    out[0] = 0; if (off < 0 || !obj) return;
    char16_t* d = *(char16_t**)((uint8_t*)obj + off);
    int32_t cnt = *(int32_t*)((uint8_t*)obj + off + 8);
    if (!d || !addr_readable((uintptr_t)d) || cnt <= 0) return;
    int n = 0; for (int i = 0; i < cnt && n < cap-1 && d[i]; i++) { char c = (char)d[i]; if (c >= 32 && c < 127) out[n++] = c; }
    out[n] = 0;
}
// TTT ServerBuy(FName): resolve the item's FName id and call the RPC on the local controller.
static void do_buy(const char* itemName) {
    if (!fn_ServerBuy || !itemName || !itemName[0]) return;
    void* pc = local_controller();
    if (!pc || !addr_readable((uintptr_t)pc) || !in_lib(*(uintptr_t*)pc)) return;
    int32_t id = fname_find(itemName);        // find existing FName id for the equipment
    if (id < 0) { LOG("buy: FName '%s' not found in pool", itemName); return; }
    struct { int32_t id; int32_t num; } p{ id, 0 };   // FName param (comparison id + number)
    g_ProcessEvent(pc, fn_ServerBuy, &p);
    LOG("buy: ServerBuy('%s' id=%d)", itemName, id);
}
// FREE SPAWN: ServerGive(FName Equipment, FName Skin) on OUR OWNED pawn (via the controller's Pawn —
// local_pawn() can grab a non-owned pawn, and sending a Server RPC on a pawn we don't net-own crashes
// the serializer). Same equipment IDs as buy, no credits.
static void do_give(const char* itemName) {
    if (!fn_ServerGive || !itemName || !itemName[0]) return;
    void* pc = local_controller();
    if (!pc || !addr_readable((uintptr_t)pc) || !in_lib(*(uintptr_t*)pc)) { LOG("give: no controller"); return; }
    static int32_t o_pawn = -2; if (o_pawn == -2) o_pawn = prop_offset(obj_class(pc), "Pawn");
    void* pawn = (o_pawn >= 0) ? *(void**)((uint8_t*)pc + o_pawn) : nullptr;
    if (!pawn || !addr_readable((uintptr_t)pawn) || !in_lib(*(uintptr_t*)pawn)) { LOG("give: no owned pawn (o_pawn=%d)", o_pawn); return; }
    int32_t id = fname_find(itemName);
    if (id < 0) { LOG("give: FName '%s' not found in pool", itemName); return; }
    struct { int32_t eqId, eqNum, skId, skNum; } p{ id, 0, 0, 0 };   // FName Equipment@0, FName Skin@8 (None)
    g_ProcessEvent(pawn, fn_ServerGive, &p);
    LOG("give: ServerGive('%s' id=%d) on owned pawn %p", itemName, id, pawn);
}
// SERVER-VISIBLE RENAME: call the game's ServerChangeName(FString) RPC on our controller. Official
// forces anonymous names back to "null"; community/dedicated servers often honor it -> dodges a
// name-based ban. Uses the game's own RPC (not a raw replicated write, so no netcode SIGSEGV).
static void do_change_name(const char* name) {
    if (!fn_ChangeName || !name || !name[0]) { LOG("changename: no fn/name"); return; }
    void* pc = local_controller();
    if (!pc || !addr_readable((uintptr_t)pc) || !in_lib(*(uintptr_t*)pc)) { LOG("changename: no controller"); return; }
    static char16_t w[64]; int n = 0; for (; name[n] && n < 62; n++) w[n] = (unsigned char)name[n]; w[n] = 0;
    uint8_t buf[16]; memset(buf, 0, sizeof buf);
    *(void**)(buf + 0) = w; *(int32_t*)(buf + 8) = n + 1; *(int32_t*)(buf + 12) = n + 1;   // FString S
    g_ProcessEvent(pc, fn_ChangeName, buf);
    LOG("ServerChangeName('%s') sent", name);
}
// ESP: project every enemy with the game's own camera into screen [0,1] + read name/health/team/role.
static void esp_gather() {
    if (!g_ready) { g_esp_n = 0; return; }
    void* pc = local_controller();
    if (!pc || !addr_readable((uintptr_t)pc)) { g_esp_n = 0; return; }
    if (!fn_ViewPoint) fn_ViewPoint = find_func(obj_class(pc), "GetPlayerViewPoint");
    if (!fn_ViewPoint) { g_esp_n = 0; return; }
    uint8_t vp[64]; memset(vp, 0, sizeof vp);
    g_fguard = 1; if (sigsetjmp(g_fjmp, 1)) { g_fguard = 0; g_esp_n = 0; return; }
    g_ProcessEvent(pc, fn_ViewPoint, vp); g_fguard = 0;
    double* L = (double*)(vp + 0); double* R = (double*)(vp + 24);   // camera loc + rot
    const double D2R = 0.017453292519943295;
    double cyaw = R[1]*D2R, cpit = R[0]*D2R;
    double cy = cos(cyaw), sy = sin(cyaw), cpp = cos(cpit), sp = sin(cpit);
    FVec fwd{ cpp*cy, cpp*sy, sp }, right{ -sy, cy, 0 };
    FVec up{ fwd.y*right.z - fwd.z*right.y, fwd.z*right.x - fwd.x*right.z, fwd.x*right.y - fwd.y*right.x };
    double fovH = g_mei.esp_fov; if (fovH < 40) fovH = 40; if (fovH > 140) fovH = 140;
    g_esp_fov_used = (float)fovH;
    double tanH = tan(fovH*0.5*D2R), tanV = tanH / (double)MEI_ESP_ASPECT;
    void* me = local_pawn();
    int32_t myteam = (me && o_TeamId >= 0 && addr_readable((uintptr_t)me)) ? *(int32_t*)((uint8_t*)me + o_TeamId) : -1;
    int cnt = 0;
    for (int i = 0; i < g_nbots && cnt < MEI_ESP_MAX; i++) {
        void* o = g_bots[i];
        g_fguard = 1; if (sigsetjmp(g_fjmp, 1)) { g_fguard = 0; continue; }
        if (!o || !addr_readable((uintptr_t)o) || o == me || !in_lib(*(uintptr_t*)o) ||
            !cls_is_body(obj_class(o)) || pawn_dead(o)) { g_fguard = 0; continue; }
        FVec feet = aim_getloc(o); FVec head = get_head(o);
        if (head.x == 0 && head.y == 0 && head.z == 0) head = FVec{ feet.x, feet.y, feet.z + 180.0 };
        // project a world point -> screen [0,1]; returns false if behind camera
        auto proj = [&](FVec w, float* su, float* sv) -> bool {
            FVec rel{ w.x - L[0], w.y - L[1], w.z - L[2] };
            double x = rel.x*right.x + rel.y*right.y + rel.z*right.z;
            double y = rel.x*up.x + rel.y*up.y + rel.z*up.z;
            double z = rel.x*fwd.x + rel.y*fwd.y + rel.z*fwd.z;
            if (z <= 1.0) return false;
            *su = (float)(0.5 + (x/z)/(2.0*tanH));
            *sv = (float)(0.5 - (y/z)/(2.0*tanV));
            return true;
        };
        float fu, fv, hu, hv;
        if (!proj(feet, &fu, &fv) || !proj(head, &hu, &hv)) { g_fguard = 0; continue; }
        double dcm = sqrt((feet.x-L[0])*(feet.x-L[0]) + (feet.y-L[1])*(feet.y-L[1]) + (feet.z-L[2])*(feet.z-L[2]));
        float distM = (float)(dcm / 100.0);   // UE units are cm
        if (distM > g_mei.esp_max_dist) { g_fguard = 0; continue; }
        EspEntry& e = g_esp[cnt];
        e.u = fu; e.v = fv; e.uh = hu; e.vh = hv; e.dist = distM;
        e.team = (o_TeamId >= 0) ? *(int32_t*)((uint8_t*)o + o_TeamId) : 0;
        (void)myteam;
        // name via PlayerState
        e.name[0] = 0; e.role[0] = 0; e.credits = -1; e.health = -1.f;
        void* ps = (o_PawnPS >= 0) ? *(void**)((uint8_t*)o + o_PawnPS) : nullptr;
        if (ps && addr_readable((uintptr_t)ps) && in_lib(*(uintptr_t*)ps)) {
            if (o_PS_Name >= 0) read_fstring(ps, o_PS_Name, e.name, sizeof e.name);
            if (o_PS_Role >= 0) { int32_t rid = *(int32_t*)((uint8_t*)ps + o_PS_Role);
                if (rid > 0) fname_to_str(rid, e.role, sizeof e.role); }
        }
        // health via HealthComponent
        void* hc = (o_PawnHC >= 0) ? *(void**)((uint8_t*)o + o_PawnHC) : nullptr;
        if (hc && addr_readable((uintptr_t)hc) && in_lib(*(uintptr_t*)hc) && o_HC_H >= 0 && o_HC_MH >= 0) {
            float h = *(float*)((uint8_t*)hc + o_HC_H), mx = *(float*)((uint8_t*)hc + o_HC_MH);
            if (mx > 0.f) { e.health = h/mx; if (e.health < 0) e.health = 0; if (e.health > 1) e.health = 1; }
        }
        e.valid = true; cnt++;
        g_fguard = 0;
    }
    g_esp_n = cnt;
}

// PICK-A-TARGET kill list. players_gather fills the menu list; do_kill_selected reports a headshot on the
// chosen player, re-resolved FRESH by name at press time (the stored pointer can be reused/freed between
// refresh and press — reporting a stale pawn is what crashes the netcode). Only loaded pawns are reported.
static void* g_playerPawns[MEI_PLAYERS_MAX];
// Robust head for kill/list: prefer the skull socket; if that hasn't resolved (common even for nearby
// pawns), fall back to the actor location + head height. Returns false only if we can't place the pawn
// at all. This is what fixes "someone right next to me shows as far" (socket was just null).
static bool kill_head(void* o, FVec* out) {
    FVec h = get_head(o);
    if (!(h.x == 0 && h.y == 0 && h.z == 0)) { *out = h; return true; }
    FVec f = aim_getloc(o);
    if (f.x == 0 && f.y == 0 && f.z == 0) return false;
    *out = FVec{ f.x, f.y, f.z + 150.0 }; return true;
}
static double dist_cm(void* a, void* b) {
    FVec p = aim_getloc(a), q = aim_getloc(b);
    double dx = p.x - q.x, dy = p.y - q.y, dz = p.z - q.z;
    return sqrt(dx*dx + dy*dy + dz*dz);
}
static void players_gather() {
    void* me = local_pawn();
    int n = 0;
    for (int i = 0; i < g_nbots && n < MEI_PLAYERS_MAX; i++) {
        void* o = g_bots[i];
        g_fguard = 1; if (sigsetjmp(g_fjmp, 1)) { g_fguard = 0; continue; }
        if (o && addr_readable((uintptr_t)o) && o != me && in_lib(*(uintptr_t*)o) && cls_is_body(obj_class(o))) {
            char nm[MEI_NAME_MAX] = {0};
            void* ps = (o_PawnPS >= 0) ? *(void**)((uint8_t*)o + o_PawnPS) : nullptr;
            if (ps && addr_readable((uintptr_t)ps) && in_lib(*(uintptr_t*)ps) && o_PS_Name >= 0)
                read_fstring(ps, o_PS_Name, nm, sizeof nm);
            if (!nm[0]) { g_fguard = 0; continue; }   // skip unnamed pawns (the "Player N" duplicates)
            MeiPlayer& p = g_players[n];
            strncpy(p.name, nm, sizeof p.name); p.name[sizeof p.name - 1] = 0;
            p.team  = (o_TeamId >= 0) ? *(int32_t*)((uint8_t*)o + o_TeamId) : 0;
            p.alive = !pawn_dead(o);
            FVec h; double d = (me ? dist_cm(me, o) : 0.0);
            p.loaded = kill_head(o, &h) && (me ? d < 30000.0 : true);   // placeable AND within ~300m = killable
            g_playerPawns[n] = o; n++;
        }
        g_fguard = 0;
    }
    g_players_n = n;
}
// AntiTank/50Cal etc. load per-map (null at boot). Re-resolve lazily so the report carries a real
// GunClass/BulletClass — the server drops a hit report with null classes (that's why kills did nothing).
static void resolve_kill_classes() {
    if (c_KillGun && c_KillBullet) return;
    if (!c_KillGun) { const char* g[] = { "Gun_AntiTank_C","Gun_50cal_C","Gun_HuntingRifle_C","Gun_AR9_C","Gun_AK47_C","Gun_Shotgun_C" };
        for (auto n : g) { c_KillGun = find_class(n); if (c_KillGun) break; } }
    if (!c_KillBullet) { const char* b[] = { "Bullet_50Cal_C","Bullet_762_C","Bullet_556_C","Bullet_9mm_C","Bullet_Base_C" };
        for (auto n : b) { c_KillBullet = find_class(n); if (c_KillBullet) break; } }
    // CUSTOM-MAP fallback: those names don't exist on every map. Grab ANY loaded gun/bullet class from the
    // live object array so the report always carries a real class (a null class = server drops it silently).
    if (!c_KillGun || !c_KillBullet) {
        int32_t n = objects_num();
        for (int32_t i = 0; i < n && (!c_KillGun || !c_KillBullet); i++) {
            void* o = object_at(i);
            if (!o || !addr_readable((uintptr_t)o) || !in_lib(*(uintptr_t*)o)) continue;
            void* c = obj_class(o); if (!addr_readable((uintptr_t)c) || !in_lib(*(uintptr_t*)c)) continue;
            char on[40]; obj_name(o, on, sizeof on); if (!strncmp(on, "Default__", 9)) continue;
            if (!c_KillGun && c_VRGun && is_a(o, c_VRGun)) c_KillGun = c;          // any real gun's class
            if (!c_KillBullet) { char cn[40]; obj_name(c, cn, sizeof cn);          // any Bullet_* class
                if (!strncmp(cn, "Bullet", 6)) c_KillBullet = c; }
        }
    }
    static int lg = 0; if (lg++ < 8) LOG("killclasses (lazy): gun=%p bullet=%p", c_KillGun, c_KillBullet);
}
// The one working kill primitive: find the live pawn whose PlayerState name matches, and report a headshot
// on it. Both target-kill and repeating kill-all go through this, so they behave identically. Caller sets
// g_in_pass. Returns true if a report was sent.
static bool kill_by_name(const char* wantName) {
    if (!wantName || !wantName[0] || !fn_ReportHit) return false;
    void* me = local_pawn();
    void* target = nullptr;
    for (int i = 0; i < g_nbots && !target; i++) {
        void* o = g_bots[i];
        g_fguard = 1; if (sigsetjmp(g_fjmp, 1)) { g_fguard = 0; continue; }
        if (o && addr_readable((uintptr_t)o) && o != me && in_lib(*(uintptr_t*)o) &&
            cls_is_body(obj_class(o)) && !pawn_dead(o)) {
            void* ps = (o_PawnPS >= 0) ? *(void**)((uint8_t*)o + o_PawnPS) : nullptr;
            char nm[MEI_NAME_MAX] = {0};
            if (ps && addr_readable((uintptr_t)ps) && in_lib(*(uintptr_t*)ps) && o_PS_Name >= 0)
                read_fstring(ps, o_PS_Name, nm, sizeof nm);
            if (nm[0] && !strcmp(nm, wantName)) target = o;
        }
        g_fguard = 0;
    }
    if (!target) return false;
    bool sent = false;
    g_fguard = 1; if (!sigsetjmp(g_fjmp, 1)) {
        FVec h; if (kill_head(target, &h)) { report_hit(target, h, nullptr); sent = true; }
    } g_fguard = 0;
    return sent;
}
static void do_kill_selected() {
    if (!g_mei.act_kill_sel) return;
    g_mei.act_kill_sel = false;
    resolve_kill_classes();
    int idx = g_mei.kill_sel;
    if (idx < 0 || idx >= g_players_n || !fn_ReportHit) { LOG("KILLSEL: bad idx %d (n=%d)", idx, g_players_n); return; }
    char wantName[MEI_NAME_MAX]; strncpy(wantName, g_players[idx].name, sizeof wantName); wantName[MEI_NAME_MAX-1] = 0;
    g_in_pass = true;
    bool ok = kill_by_name(wantName);
    g_in_pass = false;
    LOG("KILLSEL: '%s' -> %s", wantName, ok ? "killed" : "not present/placeable");
}
// REPEATING KILL ALL: run the exact target-kill primitive over EVERY name in the list each pass. Same path
// that works for one player, applied to all. report_hit drops null-class; the server drops teammate hits.
static void kill_all_loaded() {
    resolve_kill_classes();
    g_in_pass = true;
    int n = g_players_n; if (n > MEI_PLAYERS_MAX) n = MEI_PLAYERS_MAX;
    for (int i = 0; i < n; i++) kill_by_name(g_players[i].name);
    g_in_pass = false;
}
// ===========================================================================
//  voice chat (Quest fix)
// ===========================================================================
// first non-Default__ live instance of a class (settings/lobby singletons)
static void* find_singleton(void* cls) {
    if (!cls) return nullptr;
    int32_t n = objects_num();
    for (int32_t i = 0; i < n; i++) { void* o = object_at(i);
        if (!o || !addr_readable((uintptr_t)o) || !in_lib(*(uintptr_t*)o) || obj_class(o) != cls) continue;
        char nm[16]; obj_name(o, nm, sizeof nm); if (!strncmp(nm, "Default__", 9)) continue;
        return o; }
    return nullptr;
}
static void* game_instance() {
    void* w = find_world(); if (!w || O_WORLD_GI < 0) return nullptr;
    void* gi = *(void**)((uint8_t*)w + O_WORLD_GI);
    return addr_readable((uintptr_t)gi) ? gi : nullptr;
}
// fault-guarded PE call for the (new, untested) voice UFunctions
static void voice_call(void* self, void* fn, void* params) {
    if (!self || !fn || !addr_readable((uintptr_t)self) || !in_lib(*(uintptr_t*)self)) return;
    g_fguard = 1; if (!sigsetjmp(g_fjmp, 1)) g_ProcessEvent(self, fn, params); g_fguard = 0;
}
// set an FString property in place is unsafe (GC frees ours); instead call the game's SetVoiceDevice
// RPC with a transient FString frame pointing at a static UTF-16 buffer.
static void voice_set_device(const char* url) {
    if (!g_gus || !fn_SetVoiceDevice || !url) return;
    static char16_t w[64]; int n = 0; for (; url[n] && n < 62; n++) w[n] = (unsigned char)url[n]; w[n] = 0;
    uint8_t buf[16]; memset(buf, 0, sizeof buf);
    *(void**)(buf + 0) = w; *(int32_t*)(buf + 8) = n + 1; *(int32_t*)(buf + 12) = n + 1;   // FString{ptr,Num,Max} (Num incl null)
    voice_call(g_gus, fn_SetVoiceDevice, buf);
    LOG("voice: SetVoiceDevice('%s')", url);
}
// GetVoiceLevelPeak(WorldContext)->float : mic input level. >0 while talking = capture is LIVE (TX ok).
static float voice_peak() {
    void* w = find_world();
    if (!w || !fn_GetVoiceLevelPeak || !cdo_PavStatics) return -1.f;
    struct { void* wc; float ret; } p{ w, -1.f };
    g_fguard = 1; if (sigsetjmp(g_fjmp, 1)) { g_fguard = 0; return -1.f; }
    g_ProcessEvent(cdo_PavStatics, fn_GetVoiceLevelPeak, &p); g_fguard = 0;
    return p.ret;
}
// the live VoiceSource capture object: GI.VoiceSource, falling back to GetVoiceSource() and a scan.
static void* voice_source() {
    void* gi = game_instance();
    if (gi && o_GI_VoiceSrc >= 0 && addr_readable((uintptr_t)gi)) {
        void* vs = *(void**)((uint8_t*)gi + o_GI_VoiceSrc);
        if (vs && addr_readable((uintptr_t)vs) && in_lib(*(uintptr_t*)vs)) return vs;
    }
    if (gi && fn_GetVoiceSource) { struct { void* ret; } p{nullptr}; voice_call(gi, fn_GetVoiceSource, &p);
        if (p.ret && addr_readable((uintptr_t)p.ret)) return p.ret; }
    return find_singleton(c_VoiceSource);
}
// bool-returning method on the VoiceSource (ReturnValue bool @ 0); -1 if the call can't be made.
static int voice_bool_call(void* vs, void* fn) {
    if (!vs || !fn || !addr_readable((uintptr_t)vs) || !in_lib(*(uintptr_t*)vs)) return -1;
    struct { uint8_t ret; } p{2};
    g_fguard = 1; if (sigsetjmp(g_fjmp, 1)) { g_fguard = 0; return -1; }
    g_ProcessEvent(vs, fn, &p); g_fguard = 0;
    return p.ret ? 1 : 0;
}
static bool  g_voiceArmed = false;   // capture init done for this enable (reset when voice toggled off)
static float g_lastPeak   = -2.f;
static int   g_lastCap = -2, g_lastBcast = -2;
// Assert the voice pipeline: unmute, ensure a capture device is selected, enable net voice, init capture
// once, and log the mic peak so we can SEE capture go live. Self-throttled ~2 Hz (safe from the handler).
static void voice_pass() {
    static long last = 0; struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    long ms = ts.tv_sec*1000 + ts.tv_nsec/1000000; if (ms - last < 500) return; last = ms;
    if (!g_gus || !addr_readable((uintptr_t)g_gus) || obj_class(g_gus) != c_GUS) g_gus = find_singleton(c_GUS);
    // 1) unmute (live GameUserSettings bool + setter)
    if (g_mei.voice_unmute) {
        if (g_gus && o_MicMuted >= 0 && addr_readable((uintptr_t)g_gus)) *(uint8_t*)((uint8_t*)g_gus + o_MicMuted) = 0;
        if (g_gus && fn_SetMicMuted) { struct { uint8_t m, ret; } p{0,0}; voice_call(g_gus, fn_SetMicMuted, &p); }
    }
    void* gi = game_instance();
    void* vs = voice_source();
    void* pc = local_controller();
    // ONE-TIME setup on arm: set device + init capture + open the mic. Re-reading VoiceDevice returns
    // empty (the setter stores it in the native voice layer, not the GUS field), so DON'T re-set per pass —
    // that re-inits capture every tick and thrashes the broadcast. Latch it instead.
    if (!g_voiceArmed) {
        if (g_gus && fn_SetVoiceDevice) voice_set_device("Default Device");
        void* lobby = find_singleton(c_OnlineLobby);
        if (lobby && fn_SetVoiceChat) { struct { uint8_t on, ret; } p{1,0}; voice_call(lobby, fn_SetVoiceChat, &p); }
        if (pc && fn_ClientEnNetVoice) { struct { uint8_t on, ret; } p{1,0}; voice_call(pc, fn_ClientEnNetVoice, &p); }
        if (gi && fn_CheckEnVoice) { uint8_t buf[16]; memset(buf, 0, sizeof buf); voice_call(gi, fn_CheckEnVoice, buf); }
        if (vs && fn_VS_SetCapture) { struct { uint8_t on, ret; } p{1,0}; voice_call(vs, fn_VS_SetCapture, &p); }
        g_voiceArmed = true;
        LOG("voice: armed — device set, capture opened (VoiceSource=%p)", vs);
    }
    // BROADCAST: ToggleSpeaking() is a param-less TOGGLE — only call it to flip broadcast ON when it's
    // currently off (never while already broadcasting, or it'd toggle us mute). This is the open-mic latch.
    int cap = vs ? voice_bool_call(vs, fn_VS_IsCapturing) : -1;
    int bc  = vs ? voice_bool_call(vs, fn_VS_IsBroadcasting) : -1;
    if (bc == 0 && pc && fn_ToggleSpeaking) {
        uint8_t buf[8]; memset(buf, 0, sizeof buf); voice_call(pc, fn_ToggleSpeaking, buf);
        LOG("voice: ToggleSpeaking() -> start broadcast");
    }
    { void* me = local_pawn();
      if (me && o_Pawn_VoiceActive >= 0 && addr_readable((uintptr_t)me) && in_lib(*(uintptr_t*)me))
          *(uint8_t*)((uint8_t*)me + o_Pawn_VoiceActive) = 1; }
    if (cap != g_lastCap || bc != g_lastBcast) { g_lastCap = cap; g_lastBcast = bc;
        LOG("voice: IsCapturing=%d IsBroadcasting=%d (vs=%p)", cap, bc, vs); }
    float pk = voice_peak();
    if (pk >= 0.f && (g_lastPeak < 0.f || (pk - g_lastPeak > 0.02f) || (g_lastPeak - pk > 0.02f))) {
        g_lastPeak = pk; LOG("voice: mic peak=%.3f", pk);
    } else if (pk < 0.f && g_lastPeak != -1.f) { g_lastPeak = -1.f; LOG("voice: mic peak unavailable"); }
}
// One-shot: dump the live voice state + function signatures so we can see exactly what's null/empty
// and call each fn with the right param frame. Pull with: adb logcat -s pavchams | grep VOICE
static void do_voice_diag() {
    LOG("=== VOICE DIAG ===");
    LOG("gate: cdo_ARS=%p AndroidVoice@%d", cdo_ARS, o_AndroidVoice);
    if (cdo_ARS && o_AndroidVoice >= 0 && addr_readable((uintptr_t)cdo_ARS))
        LOG("  bAndroidVoiceEnabled=%d", *(uint8_t*)((uint8_t*)cdo_ARS + o_AndroidVoice));
    if (!g_gus || !addr_readable((uintptr_t)g_gus) || obj_class(g_gus) != c_GUS) g_gus = find_singleton(c_GUS);
    LOG("liveGUS=%p (class c_GUS=%p)", g_gus, c_GUS);
    if (g_gus && o_MicMuted >= 0 && addr_readable((uintptr_t)g_gus)) LOG("  bMicMuted=%d", *(uint8_t*)((uint8_t*)g_gus + o_MicMuted));
    if (g_gus && o_VoiceDev >= 0 && addr_readable((uintptr_t)g_gus)) { char s[160]; read_fstring(g_gus, o_VoiceDev, s, sizeof s); LOG("  VoiceDevice='%s'", s); }
    void* lobby = find_singleton(c_OnlineLobby); LOG("liveLobby=%p", lobby);
    void* gi = game_instance(); void* pc = local_controller(); LOG("GI=%p PC=%p", gi, pc);
    // signatures — so the pass calls them with the correct frame
    if (fn_CheckEnVoice)     dump_params("CheckAndEnableVoiceCapture", fn_CheckEnVoice);
    if (fn_SetVoiceChat)     dump_params("SetVoiceChat", fn_SetVoiceChat);
    if (fn_IsVoiceChatEn)    dump_params("IsVoiceChatEnabled", fn_IsVoiceChatEn);
    if (fn_ClientEnNetVoice) dump_params("ClientEnableNetworkVoice", fn_ClientEnNetVoice);
    if (fn_GetVoiceLevelPeak)dump_params("GetVoiceLevelPeak", fn_GetVoiceLevelPeak);
    if (fn_SetVoiceDevice)   dump_params("SetVoiceDevice", fn_SetVoiceDevice);
    if (fn_ResetVoiceDev)    dump_params("ResetVoiceCaptureDevice", fn_ResetVoiceDev);
    void* vs = voice_source(); LOG("VoiceSource live=%p (GI.VoiceSource@%d)", vs, o_GI_VoiceSrc);
    if (fn_VS_SetCapture)     dump_params("VoiceSource::SetCapture", fn_VS_SetCapture);
    if (fn_VS_IsCapturing)    dump_params("VoiceSource::IsCapturing", fn_VS_IsCapturing);
    if (fn_VS_IsBroadcasting) dump_params("VoiceSource::IsBroadcasting", fn_VS_IsBroadcasting);
    if (fn_ToggleSpeaking)    dump_params("PlayerController::ToggleSpeaking", fn_ToggleSpeaking);
    if (vs) LOG("VoiceSource state: IsCapturing=%d IsBroadcasting=%d", voice_bool_call(vs, fn_VS_IsCapturing), voice_bool_call(vs, fn_VS_IsBroadcasting));
    LOG("live mic peak = %.3f (>=0 means capture is running; -1 = no capture)", voice_peak());
    LOG("=== END VOICE DIAG ===");
}
// ---- soundboard bridge (audioshim / libOpenSLES) ------------------------------------------------
// audioshim.cpp ships AS libOpenSLES.so, RTLD_GLOBAL, and exports sb_get_control()+sb_scan(). We
// dlsym them from the global namespace and shuttle state between g_mei (menu) and its SbControl.
// This struct MUST match audioshim.cpp's SbControl (version tag = 1) byte-for-byte.
#define SB_MAX_CLIPS 64
#define SB_NAME_MAX  40
struct SbControl {
    int32_t  version;
    int32_t  n_clips;
    char     names[SB_MAX_CLIPS][SB_NAME_MAX];
    volatile int32_t play_req, stop_req, loop, mix_mic;
    volatile float   gain;
    volatile int32_t cur_clip, recorder_live, rec_rate, rec_chans, rec_bits;
};
typedef SbControl* (*fn_sb_get_control)();
typedef int        (*fn_sb_scan)();
static fn_sb_get_control r_sb_get = nullptr;
static fn_sb_scan        r_sb_scan = nullptr;
static bool              g_sb_tried = false;

// audioshim.cpp is compiled into this same .so — these are its exports (declared, not dlsym'd).
extern "C" int   sb_install();     // resolve real SL fns from the system lib + stage clips
extern "C" void* sb_hook_fn();     // address of our slCreateEngine replacement (for the GOT patch)

// voice_opus.cpp — soundboard clip -> Opus voice frames for ServerOnVoice injection.
extern "C" int         vo_scan();
extern "C" int         vo_count();
extern "C" const char* vo_name(int i);
extern "C" int         vo_load(int idx, uint8_t hdr2, float gain);   // heavy: decode+encode off the game thread
extern "C" int         vo_frames();
extern "C" int         vo_frame(int i, uint8_t* out, int cap);
extern "C" int16_t*    vo_pcm();
extern "C" int         vo_pcm_samples();
// local monitor player (audioshim.cpp): hear the clip out your own headset while it transmits.
extern "C" int         sb_local_play(const int16_t* pcm, int samples);
extern "C" void        sb_local_stop();

// clip playback state (encode on a worker thread; stream frames from the game thread at 20ms)
static volatile int  g_clip_want = -1;    // menu asks to load+play this clip index
static volatile bool g_clip_ready = false;// encode finished, frames available
static volatile bool g_clip_play = false; // streaming now
static int  g_clip_i = 0; static long g_clip_ms = 0;
static uint8_t g_clip_hdr2 = 0;           // header byte [2] to stamp (learned from captured frames)
static void* clip_encode_thread(void*) {
    int idx = g_clip_want;
    int nf = (idx >= 0) ? vo_load(idx, g_clip_hdr2, g_mei.sb_gain) : -1;
    LOG("clip encode: idx=%d frames=%d gain=%.2f", idx, nf, g_mei.sb_gain);
    if (nf > 0) {
        if (g_mei.sb_monitor) sb_local_play(vo_pcm(), vo_pcm_samples());   // hear it locally
        g_clip_i = 0; g_clip_ready = true; g_clip_play = true;
    }
    return nullptr;
}

// Fire one voice packet at the server: FPavlovVoicePacket is just { TArray<uint8> Data }, so the
// param block is that TArray (ptr,num,max). ServerOnVoice replicates it to the lobby. Fault-guarded.
static void* g_router = nullptr;
static bool send_voice_frame(uint8_t* d, int len) {
    if (!fn_ServerOnVoice || !c_VoiceRouter || len <= 0) return false;
    if (!g_router || !addr_readable((uintptr_t)g_router) || !in_lib(*(uintptr_t*)g_router))
        g_router = find_singleton(c_VoiceRouter);
    if (!g_router || !addr_readable((uintptr_t)g_router)) return false;
    struct { void* ptr; int32_t num; int32_t max; } arr;   // FPavlovVoicePacket { TArray<uint8> Data }
    arr.ptr = d; arr.num = len; arr.max = len;
    g_fguard = 1; if (sigsetjmp(g_fjmp, 1)) { g_fguard = 0; return false; }
    g_ProcessEvent(g_router, fn_ServerOnVoice, &arr);
    g_fguard = 0; return true;
}

// Soundboard: Opus-encode the selected clip on a worker thread, then stream its frames through
// ServerOnVoice at 20ms from the game thread. The genuine mic is never involved.
static void soundboard_pass() {
    g_mei.sb_present  = (fn_ServerOnVoice != nullptr);   // can we transmit?

    // clip list (scan once, refresh mirror when it changes)
    static bool scanned = false; if (!scanned) { scanned = true; vo_scan(); }
    int nc = vo_count(); if (nc > MEI_SB_MAX_CLIPS) nc = MEI_SB_MAX_CLIPS;
    if (nc != g_mei.sb_n_clips) {
        for (int i = 0; i < nc; i++) { strncpy(g_mei.sb_names[i], vo_name(i), MEI_SB_NAME_MAX-1);
                                       g_mei.sb_names[i][MEI_SB_NAME_MAX-1] = 0; }
        g_mei.sb_n_clips = nc; if (g_mei.sb_sel >= nc) g_mei.sb_sel = nc ? 0 : -1;
    }
    // derive header byte [2] from a recently captured real frame, if we have one (else 0).
    if (g_vcap_n > 0) { int last = (g_vcap_w - 1 + VCAP_MAX) % VCAP_MAX;
                        if (g_vcaplen[last] > 2) g_clip_hdr2 = g_vcap[last][2]; }
    g_mei.sb_rec_live = (g_vcap_n > 0);      // "have real header sample" indicator
    g_mei.sb_rec_rate = g_vcap_n;

    if (g_mei.sb_act_rescan) { g_mei.sb_act_rescan = false; scanned = false; }
    if (g_mei.sb_act_stop)   { g_mei.sb_act_stop = false; g_clip_play = false; sb_local_stop(); }
    if (g_mei.sb_act_play)   { g_mei.sb_act_play = false;
        if (g_mei.sb_sel >= 0 && fn_ServerOnVoice) {
            g_router = nullptr;                 // force a fresh VoiceRouter resolve (survives lobby switches)
            g_clip_play = false; g_clip_ready = false; g_clip_want = g_mei.sb_sel;
            pthread_t th; pthread_create(&th, nullptr, clip_encode_thread, nullptr); pthread_detach(th);
            LOG("clip: encoding + play requested (idx=%d hdr2=0x%02x)", g_mei.sb_sel, g_clip_hdr2);
        }
    }

    if (g_clip_play && g_clip_ready) {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        long ms = ts.tv_sec*1000 + ts.tv_nsec/1000000;
        if (ms - g_clip_ms >= 20) {
            g_clip_ms = ms;
            uint8_t f[1400]; int len = vo_frame(g_clip_i, f, sizeof f);
            if (len > 1) { f[1] = g_vseq++; send_voice_frame(f, len); }
            if (++g_clip_i >= vo_frames()) {              // end of clip
                if (g_mei.sb_loop) g_clip_i = 0; else { g_clip_play = false; LOG("clip: done"); }
            }
        }
    }
    g_mei.sb_cur = g_clip_play ? g_clip_want : -1;
}

static void handler(void* obj, void* func, void* params) {
    // mei menu input feed (throttled inside; guarded against PE re-entry)
    if (!g_in_pass && g_ready) { g_in_pass = true; mei_feed_input(); g_in_pass = false; }
    // soundboard: voice-TX replay (calls ServerOnVoice -> re-enters PE, so guard it)
    if (!g_in_pass && g_ready) { g_in_pass = true; soundboard_pass(); g_in_pass = false; }
    // ESP gather (~30 Hz) — project enemies for the overlay quad; consume the Buy action.
    if (!g_in_pass && g_ready && g_mei.master_enabled) {
        if (g_mei.act_buy) { g_mei.act_buy = false; g_in_pass = true; do_buy(g_mei.buy_name); g_in_pass = false; }
        if (g_mei.act_give) { g_mei.act_give = false; g_in_pass = true; do_give(g_mei.buy_name); g_in_pass = false; }
        if (g_mei.act_change_name) { g_mei.act_change_name = false; g_in_pass = true; do_change_name(g_mei.name_text); g_in_pass = false; }
        if (g_mei.act_voice_diag) { g_mei.act_voice_diag = false; g_in_pass = true; do_voice_diag(); g_in_pass = false; }
        if (g_mei.voice_enabled)  { g_in_pass = true; voice_pass(); g_in_pass = false; }   // self-throttled ~2Hz
        else if (g_voiceArmed) {                                                            // disable edge: stop broadcast + capture
            g_in_pass = true;
            void* vs = voice_source();
            void* pcb = local_controller();
            if (vs && fn_VS_IsBroadcasting && voice_bool_call(vs, fn_VS_IsBroadcasting) == 1 && pcb && fn_ToggleSpeaking) {
                uint8_t b[8]; memset(b, 0, sizeof b); voice_call(pcb, fn_ToggleSpeaking, b); }   // toggle broadcast OFF
            if (vs && fn_VS_SetCapture) { struct { uint8_t on, ret; } p{0,0}; voice_call(vs, fn_VS_SetCapture, &p); }
            g_in_pass = false;
            g_voiceArmed = false; g_lastPeak = -2.f; g_lastCap = -2; g_lastBcast = -2;
        }
        if (g_mei.esp_enabled) {
            static long last_esp = 0; struct timespec ets; clock_gettime(CLOCK_MONOTONIC, &ets);
            long ems = ets.tv_sec*1000 + ets.tv_nsec/1000000;
            if (ems - last_esp >= 33) { last_esp = ems; g_in_pass = true; esp_gather(); g_in_pass = false; }
        } else if (g_esp_n) g_esp_n = 0;
        // Pick-a-target kill list: refresh ~3 Hz while the menu is open + consume the kill one-shot.
        if (g_mei.menu_open) {
            static long last_pl = 0; struct timespec pts; clock_gettime(CLOCK_MONOTONIC, &pts);
            long pms = pts.tv_sec*1000 + pts.tv_nsec/1000000;
            if (pms - last_pl >= 300) { last_pl = pms; g_in_pass = true; players_gather(); g_in_pass = false; }
        }
        if (g_mei.act_kill_sel) do_kill_selected();
        // Repeating KILL ALL: while on, blast every loaded enemy on a throttle (rate = aura_rate, min 100ms).
        if (g_mei.kill_all_loop && g_ready && fn_ReportHit && g_nbots) {
            static long last_ka = 0; struct timespec kt; clock_gettime(CLOCK_MONOTONIC, &kt);
            long kms = kt.tv_sec*1000 + kt.tv_nsec/1000000;
            long iv = (long)g_mei.aura_rate; if (iv < 100) iv = 100;
            if (kms - last_ka >= iv) { last_ka = kms; kill_all_loaded(); }
        }
    }
    // AUTH DIAGNOSTIC: log the disconnect/kick RPC + any string reason param (the "cannot be verified"
    // message the host sends when it rejects a player-hosted join).
    if (g_ready && g_nkick) {
        for (int k = 0; k < g_nkick; k++) if (g_kick[k] == func) {
            char kn[64]; obj_name(func, kn, sizeof kn);
            LOG("DISCONNECT RPC fired: '%s'", kn);
            for (void* p = *(void**)((uint8_t*)func + USTRUCT_CHILDPROPS); addr_readable((uintptr_t)p);
                 p = *(void**)((uint8_t*)p + FFIELD_NEXT_OFF)) {
                char pn[48]; field_name(p, pn, sizeof pn);
                int32_t po = *(int32_t*)((uint8_t*)p + FPROP_OFFSET_OFF);
                void* pc = *(void**)((uint8_t*)p + 0x8); char pcn[40] = "?";
                if (addr_readable((uintptr_t)pc)) { int32_t ni = *(int32_t*)pc; fname_to_str(ni, pcn, sizeof pcn); }
                if (params && strstr(pcn, "Str")) { char rs[160]; read_fstring(params, po, rs, sizeof rs);
                    LOG("  reason %s='%s'", pn, rs); }
                else LOG("  param %s : %s @ %d", pn, pcn, po);
                if (!*(void**)((uint8_t*)p + FFIELD_NEXT_OFF)) break;
            }
            break;
        }
    }
    // AUTH TRACE (log-only): whenever a join-handshake RPC fires, print it + its params so we can see
    // the LIVE sequence on a failing player-hosted join — which gate the host drives, and what our
    // client actually produces (IdToken/Nonce/AttestationToken lengths+preview, AC DataBlob size).
    if (g_ready && g_nauthfn) {
        for (int k = 0; k < g_nauthfn; k++) if (g_authfn[k] == func) {
            LOG("AUTHTRACE >>> %s", g_authnm[k]);
            for (void* p = *(void**)((uint8_t*)func + USTRUCT_CHILDPROPS); addr_readable((uintptr_t)p);
                 p = *(void**)((uint8_t*)p + FFIELD_NEXT_OFF)) {
                char pn[48]; field_name(p, pn, sizeof pn);
                int32_t po = *(int32_t*)((uint8_t*)p + FPROP_OFFSET_OFF);
                void* pc = *(void**)((uint8_t*)p + 0x8); char pcn[48] = "?";
                if (addr_readable((uintptr_t)pc)) { int32_t ni = *(int32_t*)pc; fname_to_str(ni, pcn, sizeof pcn); }
                if (params && strstr(pcn, "Str")) {                       // FString: length + preview
                    char16_t* d = *(char16_t**)((uint8_t*)params + po); int32_t cnt = *(int32_t*)((uint8_t*)params + po + 8);
                    char rs[200] = ""; if (d && addr_readable((uintptr_t)d) && cnt > 0)
                        for (int j = 0; j < cnt && j < 199 && d[j]; j++) { char c=(char)d[j]; rs[j]=(c>=32&&c<127)?c:'.'; }
                    LOG("   %s: FString len=%d '%.80s'", pn, cnt, rs);
                } else if (params && strstr(pcn, "Array")) {              // TArray<uint8> DataBlob: count
                    int32_t cnt = *(int32_t*)((uint8_t*)params + po + 8);
                    LOG("   %s: Array count=%d", pn, cnt);
                } else LOG("   %s : %s @ %d", pn, pcn, po);
                if (!*(void**)((uint8_t*)p + FFIELD_NEXT_OFF)) break;
            }
            break;
        }
    }
    // VOICE CAPTURE: on an incoming ClientOnVoiceBunch, copy each packet's full Data (6-byte header +
    // Opus frame) into the ring. On PLAY we replay these via ServerOnVoice to prove the transmit path.
    // Don't capture our own replayed frames (would feed back).
    if (g_ready && func == fn_ClientOnVoiceBunch && params && !g_vreplay) {
        static int shots = 0;
        g_fguard = 1; if (sigsetjmp(g_fjmp, 1)) { g_fguard = 0; }
        else {
            uint8_t* pk = *(uint8_t**)((uint8_t*)params + 0x10);   // Packets TArray ptr @0x10
            int32_t  pn = *(int32_t*)((uint8_t*)params + 0x18);    // Packets num  @0x18
            if (pk && addr_readable((uintptr_t)pk) && pn > 0 && pn < 64) {
                for (int i = 0; i < pn; i++) {
                    uint8_t* el = pk + (size_t)i * 0x10;           // FPavlovVoicePacket = {TArray Data @0}
                    uint8_t* d  = *(uint8_t**)el;
                    int32_t  dn = *(int32_t*)(el + 8);
                    if (d && addr_readable((uintptr_t)d) && dn > 0 && dn <= VCAP_LEN) {
                        memcpy(g_vcap[g_vcap_w], d, dn); g_vcaplen[g_vcap_w] = dn;
                        g_vcap_w = (g_vcap_w + 1) % VCAP_MAX; if (g_vcap_n < VCAP_MAX) g_vcap_n++;
                        if (shots < 6) { shots++; LOG("VOICEPKT len=%d TOC=0x%02x hdr=%02x %02x %02x %02x %02x %02x",
                            dn, d[6<dn?6:0], d[0], d[1], d[2], d[3], d[4], d[5]); }
                    }
                }
            }
        }
        g_fguard = 0;
    }
    // homing knife: mark the thrown knife on a throw call, then steer it per-PE toward nearest enemy
    if (g_ready && g_mei.master_enabled && g_mei.homing_knife && g_nknife) {
        for (int k = 0; k < g_nknife; k++) if (g_knife[k] == func) { g_thrownKnife = obj;
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); g_throwMs = ts.tv_sec*1000 + ts.tv_nsec/1000000;
            LOG("knife thrown %p -> homing", obj); break; }
        if (g_thrownKnife && !g_in_pass) { g_in_pass = true; knife_home(); g_in_pass = false; }
    }
    // ANTI-VOTEKICK REMOVED: it couldn't stop the hard disconnect AND it blocked legit menu/travel
    // (ClientReturnToMainMenu is used for normal navigation) -> trapped the user at the menu.
    // movement: apply speeds per-PE (cheap write only). Re-finding the movecomp is done at 10Hz in
    // do_norecoil — NEVER scan the object array per-PE here (that was a 20k-object scan every frame = 1fps).
    if (g_mei.move_enabled && g_mc && addr_readable((uintptr_t)g_mc) && in_lib(*(uintptr_t*)g_mc)) apply_speed();
    // godmode: per-PE so no hit slips through (offline; server-auth online)
    // godmode removed (offline-only / server-authoritative anyway)
    // dev tag (self-view): keep bDev=1 per-PE so it holds against server replication
    if (g_mei.master_enabled && g_mei.dev_tag && g_ready && g_myPS && g_devOff >= 0 && addr_readable((uintptr_t)g_myPS) &&
        in_lib(*(uintptr_t*)g_myPS)) *(uint8_t*)((uint8_t*)g_myPS + g_devOff) = 1;
    // fire-path silent aim: the instant a gun fire function is called, snap the aim so the shot
    // traces at the target's head — the motion controller can't override it at this tick.
    if (!g_in_pass && g_ready && g_nfire && g_mei.master_enabled &&
        ((g_mei.aim_enabled && g_mei.aim_mode >= AIM_ONFIRE) || g_mei.trigger_kill || g_mei.wallbang)) {
        bool aimOn = g_mei.aim_enabled && g_mei.aim_mode >= AIM_ONFIRE;
        bool fire = false; for (int k = 0; k < g_nfire; k++) if (g_fire[k] == func) { fire = true; break; }
        if (fire) {
            // capture the firing weapon as our held gun (GetItemOfClass fails online; this never does)
            if (obj && addr_readable((uintptr_t)obj) && in_lib(*(uintptr_t*)obj) && c_VRGun && is_a(obj, c_VRGun))
                g_heldGun = obj;
            static bool dumped = false;
            if (!dumped) { dumped = true;
                char fnm[64]; obj_name(func, fnm, sizeof fnm);
                LOG("=== FIRE RPC DUMP: fired '%s' ===", fnm);
                dump_params(fnm, func);                  // the func that fired
                // walk the gun's class + supers for Server/Fire/Shoot/Trace/Hit RPCs + params
                for (void* s = obj_class(obj); addr_readable((uintptr_t)s); s = struct_super(s)) {
                    char sn[48]; obj_name(s, sn, sizeof sn);
                    for (void* c = *(void**)((uint8_t*)s + USTRUCT_CHILDREN); addr_readable((uintptr_t)c);
                         c = *(void**)((uint8_t*)c + UFIELD_NEXT_OFF)) {
                        char cf[64]; obj_name(c, cf, sizeof cf);
                        if (strstr(cf,"Fire")||strstr(cf,"Shoot")||strstr(cf,"Server")||strstr(cf,"Trace")||
                            strstr(cf,"Hit")||strstr(cf,"Shot")||strstr(cf,"Damage")) {
                            char tag[80]; snprintf(tag, sizeof tag, "%s::%s", sn, cf);
                            dump_params(tag, c);
                        }
                        if (!*(void**)((uint8_t*)c + UFIELD_NEXT_OFF)) break;
                    }
                    if (!struct_super(s)) break;
                }
            }
            g_in_pass = true;
            void* me = local_pawn();
            if (me) do_aim(me, aimOn, g_mei.wallbang);  // rotate the gun toward the head; TP the shot ONLY if wallbang is on
            // trigger-kill: report a headshot on the selected target the instant you fire
            if (g_mei.trigger_kill && g_aim_target && addr_readable((uintptr_t)g_aim_target) &&
                in_lib(*(uintptr_t*)g_aim_target)) {
                g_fguard = 1; if (!sigsetjmp(g_fjmp, 1)) report_hit(g_aim_target, g_aim_targetHead, obj); g_fguard = 0;
            }
            g_in_pass = false;
        }
    }
    // CONTINUOUS AIM (aim_mode == Continuous): run on the LATE-frame ReceiveDrawHUD path (our PostRender
    // equivalent) so our rotation is the last write before the frame replicates — that's what makes it
    // land online. Uses the CACHED pawn (g_localMe), never local_pawn()/find_world, so no 90Hz scan/lag.
    if (!g_in_pass && g_ready && g_mei.master_enabled && g_mei.aim_enabled && g_mei.aim_mode == AIM_CONTINUOUS) {
        bool isHud = false; for (int k = 0; k < g_ndrawhud; k++) if (g_drawhud[k] == func) { isHud = true; break; }
        static long last_aim = 0;
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        long ms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        if (isHud || (ms - last_aim >= 11)) {
            last_aim = ms;
            void* me = g_localMe;
            if (me && addr_readable((uintptr_t)me) && in_lib(*(uintptr_t*)me)) {
                g_in_pass = true; do_aim(me, true, false); g_in_pass = false;   // rotation only (no TP)
            }
        }
    }
    // KILL-AURA: continuously report a headshot on EVERY valid enemy (no need to fire or aim). Throttled
    // to aura_rate. This is the strongest / most detectable feature — reflection-guarded per target.
    if (!g_in_pass && g_ready && g_mei.master_enabled && g_mei.kill_aura && fn_ReportHit && g_nbots) {
        static long last_aura = 0;
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        long ms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        long iv = (long)g_mei.aura_rate; if (iv < 40) iv = 40;
        if (ms - last_aura >= iv) {
            last_aura = ms;
            g_in_pass = true;
            void* me = local_pawn();
            int32_t myteam = (me && o_TeamId >= 0 && addr_readable((uintptr_t)me)) ? *(int32_t*)((uint8_t*)me + o_TeamId) : -1;
            resolve_ffa();
            for (int i = 0; i < g_nbots; i++) {
                void* o = g_bots[i];
                g_fguard = 1; if (sigsetjmp(g_fjmp, 1)) { g_fguard = 0; continue; }
                if (o && addr_readable((uintptr_t)o) && o != me && in_lib(*(uintptr_t*)o) &&
                    cls_is_body(obj_class(o)) && !pawn_dead(o)) {
                    bool skipTeam = (!g_ffaMode && myteam >= 0 && o_TeamId >= 0 &&
                                     *(int32_t*)((uint8_t*)o + o_TeamId) == myteam);
                    if (!skipTeam) {
                        FVec h = get_head(o);
                        if (!(h.x == 0 && h.y == 0 && h.z == 0)) report_hit(o, h, nullptr);
                    }
                }
                g_fguard = 0;
            }
            g_in_pass = false;
        }
    }
    // world-space TextRender menu DISABLED: it doesn't render in this VR pipeline (like SetOverlay)
    // and hammering the FText system per-frame corrupted a UE background worker -> crash.
    // In-headset control will be button+haptic instead. (menu_spawn/menu_tick kept for reference.)
    (void)menu_spawn; (void)menu_tick; (void)menu_resolve;
    if (!g_in_pass && g_ready) {
        static long last_ms = 0;
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        long ms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        if (ms - last_ms >= 100) {   // ~10 Hz
            last_ms = ms;
            g_in_pass = true;
            chams_pass();
            g_in_pass = false;
        }
    }
    g_ProcessEvent(obj, func, params);   // original
}

// ===========================================================================
//  boot
// ===========================================================================
// AUTH DIAGNOSTIC: list loaded libs that reveal the join-verification mechanism (EAC / attestation /
// Meta platform / EOS). Tells us whether player-hosted join rejection is anti-cheat or EOS identity.
static void log_auth_libs() {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[512]; char seen[4096] = {0};
    const char* keys[] = { "easyanti", "anticheat", "eac", "EOSSDK", "libEOSDK", "ovrplatform",
                           "OVRPlugin", "oculus", "horizon", "attest", "integrity", "splash" };
    while (fgets(line, sizeof line, f)) {
        for (auto k : keys) {
            const char* p = strstr(line, k);
            if (p) { // log each matching lib basename once
                char base[128] = {0}; const char* s = strrchr(line, '/'); if (!s) s = line; else s++;
                int i = 0; while (s[i] && s[i] != '\n' && i < 127) { base[i] = s[i]; i++; } base[i] = 0;
                if (!strstr(seen, base)) { strncat(seen, base, sizeof seen - strlen(seen) - 2); strcat(seen, "|");
                    LOG("AUTH-LIB: %s", base); }
                break;
            }
        }
    }
    fclose(f);
}
static void* boot(void*) {
    install_fault();
    log_auth_libs();            // one-time: what verification libs are in the process?
    mei_load();                 // pull saved feature state (mei.cfg) before anything gates on it
    scan_maps();
    if (!g_base) { LOG("libUnreal not mapped yet"); return nullptr; }
    LOG("libUnreal base=%p text=[%p,%p]", (void*)g_base, (void*)g_text_lo, (void*)g_text_hi);

    for (int tries = 0; tries < 900; tries++) {          // wait for engine init (~90s max)
        scan_maps();
        if (tries % 20 == 0)
            LOG("scan: readable=%d libUnreal-rw=%d gnames=%p gobjects=%p",
                g_nread, g_nscan, (void*)g_name_blocks, g_gobjects);
        if (!g_gobjects) find_gobjects();
        if (g_gobjects && !g_name_blocks) find_gnames();
        if (g_gobjects && g_name_blocks && objects_num() > 5000) {
            resolve_names();
            if (g_ready) {
                uintptr_t pe = find_processevent();
                if (!pe) { g_ready = false; struct timespec t{0,100000000}; nanosleep(&t,nullptr); continue; }
                LOG("PE @ lib+0x%lx — hook DISARMED. arms when g_mei.master_enabled (default on).", pe - g_base);
                for (;;) {                               // discovery done; arm the hook on demand
                    int v = 1;
                    FILE* f = fopen("/sdcard/Android/data/com.vankrupt.pavlov/files/chams.txt", "r");
                    if (f) { fscanf(f, "%d", &v); fclose(f); }
                    if (v >= 2 || g_mei.master_enabled) {   // menu master switch (or legacy chams.txt) arms it
                        LOG("arming: hooking PE @ lib+0x%lx", pe - g_base);
                        if (inline_hook(pe)) {
                            LOG("=== mei live ===");
                            for (;;) {
                                int cv = cfg_value();
                                if (cv == 9) {                   // one-shot SDK dump (menu button or chams.txt=9)
                                    static bool dumped = false;
                                    if (!dumped) { LOG("sdk dump: starting..."); dump_sdk(); dumped = true; }
                                    g_mei.act_dump_sdk = false;  // consume the menu action
                                } else if (cv == 8) {            // whitelist dump mode: retry until map BPs load
                                    static long wlast = 0; struct timespec wt; clock_gettime(CLOCK_MONOTONIC, &wt);
                                    if (wt.tv_sec - wlast >= 3) { wlast = wt.tv_sec;
                                        g_fguard = 1; if (!sigsetjmp(g_fjmp, 1)) dump_whitelist(); g_fguard = 0; }
                                    g_mei.act_dump_whitelist = false;
                                } else scan_bots();
                                struct timespec s{0, 60 * 1000 * 1000}; nanosleep(&s, nullptr);
                            }
                        } else LOG("PE hook failed");
                        return nullptr;
                    }
                    struct timespec t{1, 0}; nanosleep(&t, nullptr);
                }
            }
        }
        struct timespec ts{0, 100 * 1000 * 1000}; nanosleep(&ts, nullptr);
    }
    LOG("boot timed out");
    return nullptr;
}

// Dedicated OpenXR-install thread: retry FAST (every 5 ms) from load so we wrap xrGetInstanceProcAddr
// BEFORE the app calls xrCreateInstance/xrCreateSession (otherwise it caches the real pointers and we
// miss them). Runs independently of the chams boot/arm state.
static void* xr_boot(void*) {
    LOG("xr_boot: waiting for OpenXR loader...");
    for (int i = 0; i < 4000; i++) {                     // ~20s of 5ms polls
        if (mei_xr_install()) { LOG("xr_boot: menu hook installed after %d ms", i * 5); return nullptr; }
        struct timespec t{0, 5 * 1000 * 1000}; nanosleep(&t, nullptr);
    }
    LOG("xr_boot: OpenXR loader never appeared — VR menu unavailable (check libopenxr name)");
    return nullptr;
}

// ---- GOT patch: redirect a libUnreal import to our function --------------------------------------
// Walk libUnreal's dynamic .rela.plt, find the JUMP_SLOT reloc for `symbol`, and overwrite its GOT
// slot with `repl`. This is how we hook slCreateEngine without being able to replace the public
// system lib by name. Returns true once patched.
static bool got_hook(const char* symbol, void* repl) {
    if (!g_base) return false;
    auto fix = [](uintptr_t v) -> uintptr_t {          // .dynamic ptrs are link-time vaddrs -> rebase
        return (v >= g_base && v < g_lib_hi) ? v : g_base + v;
    };
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)g_base;
    if (!addr_readable(g_base) || memcmp(eh->e_ident, ELFMAG, SELFMAG)) return false;
    const Elf64_Phdr* ph = (const Elf64_Phdr*)(g_base + eh->e_phoff);
    const Elf64_Dyn* dyn = nullptr;
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_DYNAMIC) { dyn = (const Elf64_Dyn*)fix(ph[i].p_vaddr); break; }
    if (!dyn || !addr_readable((uintptr_t)dyn)) return false;

    const Elf64_Sym*  symtab = nullptr; const char* strtab = nullptr;
    const Elf64_Rela* jmprel = nullptr; size_t pltsz = 0;
    const Elf64_Rela* rela   = nullptr; size_t relasz = 0;
    for (const Elf64_Dyn* d = dyn; addr_readable((uintptr_t)d) && d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_SYMTAB:   symtab = (const Elf64_Sym*)fix(d->d_un.d_ptr); break;
            case DT_STRTAB:   strtab = (const char*)fix(d->d_un.d_ptr);      break;
            case DT_JMPREL:   jmprel = (const Elf64_Rela*)fix(d->d_un.d_ptr); break;
            case DT_PLTRELSZ: pltsz  = d->d_un.d_val;                        break;
            case DT_RELA:     rela   = (const Elf64_Rela*)fix(d->d_un.d_ptr); break;
            case DT_RELASZ:   relasz = d->d_un.d_val;                        break;
        }
    }
    if (!symtab || !strtab) { LOG("got_hook %s: no symtab/strtab", symbol); return false; }
    LOG("got_hook %s: jmprel=%p pltsz=%zu rela=%p relasz=%zu", symbol,
        (void*)jmprel, pltsz, (void*)rela, relasz);

    // scan both .rela.plt (JUMP_SLOT) and .rela.dyn (GLOB_DAT) for the symbol's GOT slot.
    const Elf64_Rela* tables[2] = { jmprel, rela };
    size_t counts[2] = { pltsz / sizeof(Elf64_Rela), relasz / sizeof(Elf64_Rela) };
    for (int t = 0; t < 2; t++) {
        const Elf64_Rela* T = tables[t]; if (!T) continue;
        for (size_t i = 0; i < counts[t]; i++) {
            uint32_t si = ELF64_R_SYM(T[i].r_info);
            if (!si) continue;
            const char* nm = strtab + symtab[si].st_name;
            if (!addr_readable((uintptr_t)nm) || strcmp(nm, symbol)) continue;
            void** slot = (void**)fix(T[i].r_offset);
            if (!addr_readable((uintptr_t)slot)) { LOG("got_hook %s: slot unreadable", symbol); return false; }
            uintptr_t pg = (uintptr_t)slot & ~(uintptr_t)0xFFF;          // page-align, cover RELRO
            mprotect((void*)pg, 0x2000, PROT_READ | PROT_WRITE);
            void* old = *slot; *slot = repl;
            LOG("GOT hook: %s slot=%p old=%p -> %p (table %d)", symbol, (void*)slot, old, repl, t);
            return true;
        }
    }
    LOG("got_hook %s: symbol not found in relocs", symbol);
    return false;
}

// Race the game's first slCreateEngine call (audio init) the same way xr_boot races xrCreateInstance:
// poll fast from load, and the moment libUnreal is mapped, resolve the real SL fns and patch the GOT.
static void* sb_boot(void*) {
    for (int i = 0; i < 8000; i++) {                 // ~40s of 5ms polls
        if (g_base && sb_install() && got_hook("slCreateEngine", sb_hook_fn())) {
            LOG("soundboard: slCreateEngine hooked after %d ms", i * 5);
            return nullptr;
        }
        struct timespec t{0, 5 * 1000 * 1000}; nanosleep(&t, nullptr);
    }
    LOG("soundboard: slCreateEngine never hooked (engine may init before us)");
    return nullptr;
}

extern "C" __attribute__((visibility("default")))
void pavchams_start() {
    static bool once = false; if (once) return; once = true;
    LOG("libpavchams start");
    pthread_t t; pthread_create(&t, nullptr, boot, nullptr); pthread_detach(t);
    pthread_t x; pthread_create(&x, nullptr, xr_boot, nullptr); pthread_detach(x);
    pthread_t s; pthread_create(&s, nullptr, sb_boot, nullptr); pthread_detach(s);
}

__attribute__((constructor))
static void ctor() { pavchams_start(); }
