// LiSRemasteredTweaks: a ReShade add-on for Life is Strange Remastered (UE 4.23, DirectX 11, or
// 12 with the -dx12 launch option) that raises the game's lighting, shadow and detail settings
// beyond its menu, live, from an in-game overlay, and adds HDR output, which the game's menu
// doesn't offer. Ported from TrueColorsTweaks (UE 4.25).
//
// Everything goes through the game's console variables (cvars.hpp: console manager found by
// code scan, vtable layout read from the game's own call sites). Settings are kept in
// ReShade.ini [LiSRemasteredTweaks]. Values are set on the game thread, from the console manager's
// once-per-frame CallAllConsoleVariableSinks (GameTick), so they reach the renderer between
// frames like the console's own; they are set at console priority (the menu can't override
// them), and letting go puts the game's value and priority back. F8 switches all of it off and
// on.
//
// Threads: ReShade's present thread draws the overlay, measures frame times and only changes
// options (g_state). The game thread does all setting and the probe: it only tries g_state, to
// copy what's wanted, and skips a frame if the present thread holds it. It never holds g_state
// while it sets anything: a change callback can flush rendering, which waits for the render
// thread, which may be presenting and waiting for g_state. What the overlay shows of the game
// thread's state comes from a copy it publishes under g_viewMutex (also never held across a
// Set), and the cvar lookup cache has its own lock.
//
// No ray tracing: the project never enabled it, so no ray tracing shaders were cooked (no
// r.RayTracing in DefaultEngine.ini, skin cache shaders off). Screen-space GI and GTAO came after
// 4.23 and aren't in this exe either.
//
// The add-on never patches game code. It writes LiSRemasteredTweaks.log (and -probe.txt) next to
// the game executable.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_6.h>

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "cvars.hpp"
#include "hdr_ps.h"  // built from src/hdr.hlsl by build.ps1
#include "hdr_vs.h"
#include "version.h"

extern "C" __declspec(dllexport) const char* NAME = DT_PRODUCT;
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Lighting, shadow, detail and HDR settings beyond " DT_GAME "'s menu, live from the overlay.";

namespace {

using namespace reshade::api;

constexpr int kToggleHotkey = VK_F8;

const uint8_t* GameBase() { return reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr)); }

uint32_t PeTimestamp(const uint8_t* base) {
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    return reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew)->FileHeader.TimeDateStamp;
}

// ---------------------------------------------------------------- logging

std::mutex g_logMutex;
FILE* g_log = nullptr;
ULONGLONG g_loadTick = 0;
std::wstring g_exeDir;

void Log(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02u:%02u:%02u.%03u [+%5.1f s] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            (GetTickCount64() - g_loadTick) / 1000.0, buf);
    fflush(g_log);
}

// ReShade can load the add-on more than once per process (the game makes test devices at
// start), so the log is started fresh only by the first load in a process (marked in an
// environment variable).
void OpenLog() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    g_exeDir.assign(path, n);
    g_exeDir.resize(g_exeDir.find_last_of(L"\\/") + 1);
    wchar_t pid[32] = {}, mine[32];
    swprintf(mine, 32, L"%lu", GetCurrentProcessId());
    GetEnvironmentVariableW(L"LISREMASTEREDTWEAKS_PID", pid, 32);
    const bool again = !wcscmp(pid, mine);
    SetEnvironmentVariableW(L"LISREMASTEREDTWEAKS_PID", mine);
    g_log = _wfopen((g_exeDir + DT_PRODUCT_W L".log").c_str(), again ? L"a" : L"w");
    Log(DT_PRODUCT " " DT_VERSION " loaded%s", again ? " (again in this process)" : "");
}

// Cvar names and values are ASCII.
std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += static_cast<char>(c);
    return s;
}

// ---------------------------------------------------------------- options
//
// Each option is a combo; choice id 0 leaves the game's value. The chosen id is kept in
// ReShade.ini. The Game choice shows the game's current value (the probe writes them all).

using CvarList = std::vector<std::pair<const wchar_t*, const wchar_t*>>;

struct Choice {
    const char* label;
    int id;
    CvarList cvars;
};

struct Option {
    const char* tab;
    const char* section;
    const char* key;       // ReShade.ini
    const char* label;
    const char* tip;
    const wchar_t* shown;  // cvar whose game value the "Game" choice shows (or null)
    std::vector<Choice> choices;
    int value = 0;
};

constexpr const char* kPostTab = "Post-processing";  // taste: in no preset, not measured
constexpr const char* kHdrTab = "HDR";               // in no preset, not measured

// Graded HDR's sliders (ReShade.ini HdrPeak, HdrPaperWhite), in nits.
constexpr int kPeakMin = 400, kPeakMax = 4000, kPaperMin = 80, kPaperMax = 400;
int g_hdrPeak = 1000;
int g_hdrPaper = 200;

// Every cvar here exists in this exe (checked offline against its strings). The game's values
// and costs are filled in from the first probe and measurements (docs/FINDINGS.md).
std::vector<Option> g_options = {
    // ------------------------------------------------ HDR (any API; in no preset, not measured)
    {kHdrTab, "HDR output", "Hdr", "HDR",
     "The game's menu has no HDR option. The game renders as usual, with Deck Nine's colour\n"
     "grade, and the add-on turns the finished picture into HDR10, stretching only the\n"
     "brightest parts (sun, lamps, reflections) up to the peak you set below. Windows HDR\n"
     "must be on.",
     nullptr,
     {{"Off (SDR)", 0, {}}, {"HDR10", 1, {}}}},

    // ------------------------------------------------ lighting
    {"Lighting", "Reflections", "Ssr", "Reflections on rough surfaces",
     "The game's screen-space reflections already run at Cinematic quality, but only on\n"
     "fairly glossy surfaces (each scene sets the cut-off). This lets surfaces up to 0.8\n"
     "roughness reflect too: wet streets, wood floors, painted walls.",
     L"r.SSR.MaxRoughness",
     {{"Game", 0, {}}, {"Rougher surfaces", 5, {{L"r.SSR.MaxRoughness", L"0.8"}}}}},
    {"Lighting", "Translucency", "TranslucencyLighting", "Lighting on glass and particles",
     "Resolution of the 3D grid that lights translucent things (smoke, particles, some hair\n"
     "and glass). The game uses a 64-cell grid; 128 gives finer, less blotchy lighting.",
     L"r.TranslucencyLightingVolumeDim",
     {{"Game", 0, {}}, {"High (128)", 128, {{L"r.TranslucencyLightingVolumeDim", L"128"}}}}},

    // ------------------------------------------------ shadows
    {"Shadows", "Shadow maps", "ShadowRes", "Shadow resolution",
     "Resolution of the shadow maps; the game uses 2048. Higher is sharper and steadier\n"
     "shadow edges, with less shimmer on moving shadows. 8192 applies to the sun and moon\n"
     "only (lamps stay at 4096).",
     L"r.Shadow.MaxCSMResolution",
     {{"Game", 0, {}},
      {"High (4096)", 4096, {{L"r.Shadow.MaxResolution", L"4096"}, {L"r.Shadow.MaxCSMResolution", L"4096"}}},
      {"Highest (8192 sun)", 8192, {{L"r.Shadow.MaxResolution", L"4096"}, {L"r.Shadow.MaxCSMResolution", L"8192"}}}}},
    {"Shadows", "Shadow maps", "ShadowDistance", "Shadow distance",
     "How far away the sun and lamps still cast shadows.",
     L"r.Shadow.DistanceScale",
     {{"Game", 0, {}},
      {"Far (150%)", 150, {{L"r.Shadow.DistanceScale", L"1.5"}}},
      {"Farthest (200%)", 200, {{L"r.Shadow.DistanceScale", L"2"}}}}},
    {"Shadows", "Shadow maps", "ShadowFilter", "Soft shadow filter",
     "Contact-hardening shadows (PCSS, experimental in 4.23): sharp where an object touches\n"
     "the ground, softer further away, like real shadows.",
     L"r.Shadow.FilterMethod",
     {{"Game", 0, {}}, {"Contact-hardening", 1, {{L"r.Shadow.FilterMethod", L"1"}}}}},
    {"Shadows", "Characters", "CapsuleShadows", "Character soft shadows",
     "The soft indirect shadows characters cast on the ground and walls, at full resolution\n"
     "instead of half.",
     L"r.CapsuleShadowsFullResolution",
     {{"Game", 0, {}}, {"Full resolution", 1, {{L"r.CapsuleShadowsFullResolution", L"1"}}}}},

    // ------------------------------------------------ characters
    {"Characters", "Skin", "Skin", "Skin quality",
     "Subsurface scattering (light through skin): the game leaves Unreal to decide (2), which\n"
     "may compute it on a checkerboard and fill in the rest. Full computes every pixel:\n"
     "smoother skin, no fine dithering on faces.",
     L"r.SSS.Checkerboard",
     {{"Game", 0, {}}, {"Full", 1, {{L"r.SSS.Checkerboard", L"0"}}}}},
    {"Characters", "Detail", "CharacterLod", "Character detail",
     "Keeps characters on their most detailed model at a distance instead of switching to\n"
     "simpler versions.",
     L"r.SkeletalMeshLODBias",
     {{"Game", 0, {}}, {"Highest", 1, {{L"r.SkeletalMeshLODBias", L"-1"}}}}},

    // ------------------------------------------------ detail
    {"Detail", "Textures", "TexturePool", "Texture memory",
     "Video memory for streamed textures. The game keeps about 3 GB whatever the card, so\n"
     "textures can go blurry and reload. Free on a card with enough memory.",
     L"r.Streaming.PoolSize",
     {{"Game", 0, {}},
      {"4 GB", 4096, {{L"r.Streaming.PoolSize", L"4096"}}},
      {"6 GB", 6144, {{L"r.Streaming.PoolSize", L"6144"}}},
      {"8 GB", 8192, {{L"r.Streaming.PoolSize", L"8192"}}}}},
    {"Detail", "Textures", "Anisotropy", "Anisotropic filtering",
     "Sharper floors and roads at shallow angles. The game uses 8x. Textures already loaded\n"
     "keep their filtering until they reload (or the game restarts).",
     L"r.MaxAnisotropy",
     {{"Game", 0, {}}, {"16x", 16, {{L"r.MaxAnisotropy", L"16"}}}}},
    {"Detail", "Distance", "MeshDetail", "Model detail",
     "Distance at which models switch to their simpler versions. Finer keeps the detailed\n"
     "version twice as far.",
     L"r.StaticMeshLODDistanceScale",
     {{"Game", 0, {}}, {"Finer", 1, {{L"r.StaticMeshLODDistanceScale", L"0.5"}}}}},
    {"Detail", "Distance", "ViewDistance", "Draw distance",
     "How far away small objects and details are still drawn. The game already uses 2x at\n"
     "its highest view distance.",
     L"r.ViewDistanceScale",
     {{"Game", 0, {}},
      {"Far (3x)", 3, {{L"r.ViewDistanceScale", L"3"}}},
      {"Farthest (4x)", 4, {{L"r.ViewDistanceScale", L"4"}}}}},
    {"Detail", "Image", "TaaSamples", "Anti-aliasing samples",
     "How many jitter positions the temporal anti-aliasing cycles through. Deck Nine set it to\n"
     "4 (Unreal's default is 8): fewer positions leave stair steps and shimmer on thin edges,\n"
     "hair and foliage. 8 gives smoother, steadier edges.",
     L"r.TemporalAASamples",
     {{"Game", 0, {}}, {"8 (Unreal default)", 8, {{L"r.TemporalAASamples", L"8"}}}}},

    // ------------------------------------------------ post-processing (taste; in no preset)
    {kPostTab, "Camera", "FilmGrain", "Film grain",
     "The film grain noise (and the tonemapper's dithering). Off keeps the vignette and the\n"
     "film contrast; Grain and vignette off removes the vignette too.",
     L"r.Tonemapper.Quality",
     {{"Game", 0, {}},
      {"Off", 1, {{L"r.Tonemapper.Quality", L"2"}}},
      {"Grain and vignette off", 2, {{L"r.Tonemapper.Quality", L"1"}}}}},
    {kPostTab, "Camera", "LensFlares", "Lens flares", "Ghost reflections of bright lights across the screen.",
     L"r.LensFlareQuality", {{"Game", 0, {}}, {"Off", 1, {{L"r.LensFlareQuality", L"0"}}}}},
    {kPostTab, "Camera", "DepthOfField", "Depth of field", "Background blur in close-ups and cutscenes.",
     L"r.DepthOfFieldQuality", {{"Game", 0, {}}, {"Off", 1, {{L"r.DepthOfFieldQuality", L"0"}}}}},
    {kPostTab, "Camera", "Fringe", "Chromatic aberration", "Colour fringes at the screen edges.",
     L"r.SceneColorFringeQuality", {{"Game", 0, {}}, {"Off", 1, {{L"r.SceneColorFringeQuality", L"0"}}}}},
    {kPostTab, "Image", "Sharpen", "Sharpening",
     "The tonemapper's sharpening filter. The game uses 0.5; Strong is crisper but can halo.",
     L"r.Tonemapper.Sharpen",
     {{"Game", 0, {}},
      {"Off", 1, {{L"r.Tonemapper.Sharpen", L"0"}}},
      {"Strong (1.0)", 10, {{L"r.Tonemapper.Sharpen", L"1"}}}}},
};

// Guards the options, the measurement and the requests to the game thread (present thread;
// the game thread only tries it, briefly).
std::recursive_mutex g_state;

std::atomic<bool> g_enabled{true};  // F8

const Choice* Chosen(const Option& o) {
    for (const Choice& c : o.choices)
        if (c.id == o.value) return &c;
    return nullptr;
}

void LoadSettings() {
    for (Option& o : g_options) {
        reshade::get_config_value(nullptr, DT_PRODUCT, o.key, o.value);
        if (!Chosen(o)) o.value = 0;
    }
}

void SaveSettings() {
    for (const Option& o : g_options) reshade::set_config_value(nullptr, DT_PRODUCT, o.key, o.value);
}

// What we want in force now.
std::map<std::wstring, std::wstring> Wanted() {
    std::map<std::wstring, std::wstring> m;
    if (!g_enabled) return m;
    for (const Option& o : g_options)
        if (const Choice* c = Chosen(o))
            for (auto& [key, value] : c->cvars) m[key] = value;
    return m;
}

// ---------------------------------------------------------------- device

std::atomic<int> g_api{0};  // device_api of the presenting device (0 = not seen yet)

bool OnDirectX12() { return g_api.load() == static_cast<int>(device_api::d3d12); }

const char* ApiName() {
    switch (static_cast<device_api>(g_api.load())) {
        case device_api::d3d11: return "DirectX 11";
        case device_api::d3d12: return "DirectX 12";
        case device_api::vulkan: return "Vulkan";
        default: return "?";
    }
}

// ---------------------------------------------------------------- live settings

enum class Live { Idle, Searching, Ready, Failed };
std::atomic<Live> g_live{Live::Idle};
void* g_manager = nullptr;
std::string g_liveError;

// Game thread only:
std::map<std::wstring, std::wstring> g_originals;  // the game's value of each cvar we hold
std::map<std::wstring, uint32_t> g_origPriority;   // ...and the priority it was set at
std::map<std::wstring, std::wstring> g_applied;    // what we keep in force
std::set<std::wstring> g_rejected;                 // values the game didn't take (logged once)

// Requests from the present thread (under g_state):
std::atomic<bool> g_dirty{true};  // options changed: apply at the next game tick
std::vector<std::pair<std::wstring, std::wstring>> g_pendingConsole;  // Advanced tab's "Set once"
std::string g_pendingProbe;                                           // Advanced tab's probe request

std::atomic<int> g_reapplied{0};    // values put back after they changed
std::atomic<bool> g_hooked{false};  // game-thread hook in place
std::atomic<uint64_t> g_ticks{0};   // game ticks seen by the hook
ULONGLONG g_readyTick = 0;

// What the overlay shows of the game thread's state; the game thread publishes it.
struct View {
    std::map<std::wstring, std::wstring> held;  // held cvar -> the game's value
    std::vector<std::string> rejected;
    std::string probeResult;
};
std::mutex g_viewMutex;
View g_view;

void PublishView() {
    View v;
    for (auto& [key, value] : g_applied) {
        auto it = g_originals.find(key);
        v.held[key] = it != g_originals.end() ? it->second : L"?";
    }
    for (const std::wstring& k : g_rejected) v.rejected.push_back(Narrow(k));
    std::lock_guard<std::mutex> lock(g_viewMutex);
    v.probeResult = std::move(g_view.probeResult);
    g_view = std::move(v);
}

View CopyView() {
    std::lock_guard<std::mutex> lock(g_viewMutex);
    return g_view;
}

struct VarInfo {
    void* var = nullptr;
    cvars::Type type = cvars::Type::Unknown;
};
std::mutex g_varsMutex;
std::map<std::wstring, VarInfo> g_vars;  // lookup cache (cvars live for the whole session)

VarInfo Lookup(const std::wstring& name) {
    std::lock_guard<std::mutex> lock(g_varsMutex);
    auto it = g_vars.find(name);
    if (it != g_vars.end()) return it->second;
    VarInfo info;
    if (!g_manager) return info;
    info.var = cvars::FindVariable(g_manager, name.c_str());
    if (info.var) info.type = cvars::GetType(info.var);
    if (info.var) g_vars[name] = info;  // misses aren't cached: plugin cvars can register late
    return info;
}

bool Read(const std::wstring& name, std::wstring& out) {
    VarInfo v = Lookup(name);
    wchar_t buf[64];
    if (!v.var || !cvars::GetText(v.var, v.type, buf, 64)) return false;
    out = buf;
    return true;
}

bool SameValue(cvars::Type t, const std::wstring& a, const std::wstring& b) {
    if (t == cvars::Type::Float) return fabs(_wtof(a.c_str()) - _wtof(b.c_str())) < 1e-4;
    return _wtoi(a.c_str()) == _wtoi(b.c_str());
}

// How many change callbacks a variable has: OnChangedCallback is a multicast delegate at +0x20
// with its invocation count at +0x28 (FConsoleVariableBase in UE 4.2x/5.x; logged only).
int Callbacks(void* var) {
    int32_t n = -1;
    cvars::SafeCopy(&n, static_cast<uint8_t*>(var) + 0x28, 4);
    return n;
}

void __fastcall HookedSinks(void* self);
cvars::SinksFn g_origSinks = nullptr;

void FindManager() {
    const uint8_t* base = GameBase();
    Log("game exe timestamp 0x%08x (%s)", PeTimestamp(base),
        PeTimestamp(base) == DT_KNOWN_PE_TIMESTAMP ? "the tested build" : "NOT the tested build");
    int votes = 0, matches = 0;
    void** singleton = cvars::FindConsoleManagerSingleton(base, &votes, &matches);
    void* manager = nullptr;
    if (singleton) cvars::SafeCopy(&manager, singleton, 8);
    Log("console manager: singleton %p (RVA 0x%llx, %d of %d matches), manager %p", singleton,
        singleton ? static_cast<unsigned long long>(reinterpret_cast<const uint8_t*>(singleton) - base) : 0ull, votes,
        matches, manager);
    if (!manager || !cvars::IsConsoleManager(base, manager)) {
        g_liveError = "console manager not found";
        g_live = Live::Failed;
        return;
    }
    // Known variables check the layout: r.MaxAnisotropy is an int (1-16), r.ScreenPercentage a float.
    void* aniso = cvars::FindVariable(manager, L"r.MaxAnisotropy");
    void* screen = cvars::FindVariable(manager, L"r.ScreenPercentage");
    bool vt = aniso && screen && cvars::HasModuleVtable(base, aniso) && cvars::HasModuleVtable(base, screen);
    bool slots = vt && cvars::FindTypeSlots(aniso, screen);
    int32_t a = -1;
    float s = -1;
    bool reads = slots && cvars::GetInt(aniso, a) && cvars::GetFloat(screen, s);
    Log("layout check: r.MaxAnisotropy %p, r.ScreenPercentage %p, vtables %d, IsVariableInt at +0x%x, values %d / %g",
        aniso, screen, vt, cvars::g_isVariableInt, a, s);
    if (!reads || a < 0 || a > 16 || !(s >= 10 && s <= 400)) {
        g_liveError = "the game's settings layout didn't match (see the log)";
        g_live = Live::Failed;
        return;
    }
    for (auto [name, var] : {std::pair<const char*, void*>{"r.MaxAnisotropy", aniso}, {"r.ScreenPercentage", screen}}) {
        cvars::Type t = cvars::GetType(var);
        void* data = cvars::AsData(var, t);
        Log("  %s: %s, priority 0x%02x, data %s (+0x%llx), %d callbacks", name, cvars::TypeName(t), cvars::Priority(var),
            data ? "yes" : "no",
            data ? static_cast<unsigned long long>(static_cast<uint8_t*>(data) - static_cast<uint8_t*>(var)) : 0ull,
            Callbacks(var));
    }
    {
        std::lock_guard<std::mutex> lock(g_varsMutex);
        g_manager = manager;
    }
    if (!cvars::HookSinks(base, manager, &HookedSinks, &g_origSinks)) {
        g_liveError = "couldn't reach the game thread (see the log)";
        Log("game-thread hook: CallAllConsoleVariableSinks slot not found");
        g_live = Live::Failed;
        return;
    }
    g_hooked = true;
    Log("game-thread hook in place (CallAllConsoleVariableSinks was %p, RVA 0x%llx)",
        reinterpret_cast<void*>(g_origSinks),
        static_cast<unsigned long long>(reinterpret_cast<const uint8_t*>(g_origSinks) - base));
    g_readyTick = GetTickCount64();
    g_live = Live::Ready;
}

// Puts <value> in force at console priority. Game thread only.
bool SetVar(const std::wstring& key, const std::wstring& value) {
    VarInfo v = Lookup(key);
    if (!v.var) {
        if (g_rejected.insert(key).second) Log("%ls not found", key.c_str());
        return false;
    }
    if (!cvars::Set(v.var, value.c_str(), cvars::kSetByConsole)) return false;
    std::wstring now;  // on the game thread the getters read the main value, just set
    if (Read(key, now) && !SameValue(v.type, now, value)) {
        if (g_rejected.insert(key).second)
            Log("%ls = %ls not taken (reads %ls, priority 0x%02x)", key.c_str(), value.c_str(), now.c_str(),
                cvars::Priority(v.var));
        return false;
    }
    g_rejected.erase(key);
    return true;
}

// Gives a value back: the game's value, then the game's priority (so its menu can set it again).
bool RestoreVar(const std::wstring& key) {
    auto it = g_originals.find(key);
    if (it == g_originals.end() || !SetVar(key, it->second)) return false;
    auto pr = g_origPriority.find(key);
    VarInfo v = Lookup(key);
    if (pr != g_origPriority.end() && v.var) cvars::RestorePriority(v.var, pr->second);
    return true;
}

// Brings the game in line with Wanted(): our values in force, ones no longer wanted back to the
// game's. Game thread.
void Apply(std::map<std::wstring, std::wstring> want) {
    int set = 0, restored = 0, failed = 0;
    for (auto& [key, value] : want) {
        if (!g_applied.count(key)) {  // newly held: the game's value is what's there now
            std::wstring old;
            if (Read(key, old)) {
                g_originals[key] = old;
                if (VarInfo v = Lookup(key); v.var) g_origPriority[key] = cvars::GetFlags(v.var) & cvars::kPriorityMask;
            }
        }
        if (SetVar(key, value)) ++set;
        else ++failed;
    }
    for (auto& [key, value] : g_applied) {
        if (want.count(key)) continue;
        if (RestoreVar(key)) ++restored;
    }
    g_applied = std::move(want);
    Log("applied %d, restored %d, failed %d (tweaks %s)", set, restored, failed, g_enabled ? "on" : "off");
    PublishView();
}

// Every second: any value that changed anyway (the game's code can set at console priority too)
// becomes the new game value, and ours goes back in. Game thread.
void Hold() {
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    if (now - last < 1000) return;
    last = now;
    static std::map<std::wstring, int> counts;  // per setting, to keep the log short
    bool any = false;
    for (auto& [key, value] : g_applied) {
        if (g_rejected.count(key)) continue;
        VarInfo v = Lookup(key);
        std::wstring cur;
        if (!Read(key, cur) || SameValue(v.type, cur, value)) continue;
        g_originals[key] = cur;
        if (SetVar(key, value)) ++g_reapplied;
        any = true;
        int n = ++counts[key];
        if (n <= 3 || n == 10 || n % 100 == 0)
            Log("put back %ls = %ls (the game set %ls; %d times so far)", key.c_str(), value.c_str(), cur.c_str(), n);
    }
    if (any) PublishView();
}

// ---------------------------------------------------------------- probe
//
// Writes the game's value of every console variable under the prefixes below to
// LiSRemasteredTweaks-probe.txt: automatically 5 s and 60 s after connecting (title screen, then
// after the game has applied its menu), and from the Advanced tab at any time (in a scene). Names
// come from the game's read-only data (UTF-16 strings); nothing is enumerated. Runs on the game
// thread, so it reads the game thread's values.

const char* const kProbePrefixes[] = {
    "r.Shadow.", "r.TemporalAA", "r.PostProcessAAQuality", "r.ScreenPercentage",
    "r.SecondaryScreenPercentage", "r.DynamicRes.", "r.Streaming.", "r.MaxAnisotropy", "r.VT.", "r.HDR.",
    "r.Tonemapper", "r.SSS.", "r.SubsurfaceScattering", "r.SkinCache.", "r.VolumetricFog", "r.Bloom", "r.LensFlare",
    "r.DepthOfField", "r.DOF.", "r.MotionBlur", "r.SceneColorFringe", "r.SSR.", "r.AmbientOcclusion",
    "r.AOQuality", "r.DistanceField", "r.ContactShadows", "r.CapsuleShadow", "r.ViewDistanceScale",
    "r.StaticMeshLODDistanceScale", "r.SkeletalMeshLODBias", "r.LandscapeLOD", "r.Translucency", "r.SeparateTranslucency",
    "r.LightMaxDrawDistanceScale", "r.LightFunctionQuality", "r.LightShaftQuality", "r.MipMapLODBias", "r.EyeAdaptation",
    "r.DefaultFeature.", "r.RefractionQuality", "r.DetailMode", "r.MaterialQualityLevel", "r.SceneColorFormat",
    "r.ShadowQuality", "r.Upscale.", "r.BasePassOutputsVelocity", "r.EmitterSpawnRateScale", "r.ParticleLightQuality",
    "r.D3D12.", "foliage.", "grass.", "sg.", "D9.", "d9.",
};

std::set<std::wstring> g_probeNames;  // collected once

void CollectProbeNames() {
    std::vector<std::wstring> prefixes;
    for (const char* p : kProbePrefixes) prefixes.emplace_back(p, p + strlen(p));
    for (const cvars::Section& s : cvars::Sections(GameBase(), cvars::SectionKind::ReadOnly)) {
        for (size_t i = 2; i + 8 < s.size; i += 2) {
            if (s.begin[i - 2] || s.begin[i - 1]) continue;  // must start right after a terminator
            const wchar_t* w = reinterpret_cast<const wchar_t*>(s.begin + i);
            wchar_t c0 = w[0];
            if (c0 != L'r' && c0 != L'f' && c0 != L'g' && c0 != L's' && c0 != L'D' && c0 != L'd') continue;
            size_t len = 0;
            while (len < 120 && i + 2 * len + 2 < s.size && w[len] > 32 && w[len] < 127) ++len;
            if (len < 3 || w[len] != 0) continue;
            for (const std::wstring& p : prefixes)
                if (len >= p.size() && !wcsncmp(w, p.c_str(), p.size())) {
                    g_probeNames.emplace(w, len);
                    break;
                }
        }
    }
    Log("probe: %zu candidate names", g_probeNames.size());
}

void WriteProbe(const char* label) {
    if (g_probeNames.empty()) CollectProbeNames();
    FILE* f = _wfopen((g_exeDir + DT_PRODUCT_W L"-probe.txt").c_str(), L"a");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "\n==== %s (%04u-%02u-%02u %02u:%02u:%02u, %.0f s after start, %s, tweaks %s, holding %zu)\n", label,
            t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, (GetTickCount64() - g_loadTick) / 1000.0, ApiName(),
            g_enabled ? "on" : "off", g_applied.size());
    int found = 0, unreadable = 0;
    for (const std::wstring& name : g_probeNames) {
        VarInfo v = Lookup(name);
        if (!v.var) continue;
        std::wstring value;
        if (Read(name, value)) {
            ++found;
            fprintf(f, "%ls (%s, set by 0x%02x) = %ls\n", name.c_str(), cvars::TypeName(v.type), cvars::Priority(v.var),
                    value.c_str());
        } else {
            ++unreadable;
            fprintf(f, "%ls (%s) = ?\n", name.c_str(), cvars::TypeName(v.type));
        }
    }
    fprintf(f, "---- %d values, %d unreadable (strings/commands skipped)\n", found, unreadable);
    fclose(f);
    Log("probe '%s': %d values written", label, found);
    std::lock_guard<std::mutex> lock(g_viewMutex);
    g_view.probeResult = std::to_string(found) + " values written to " DT_PRODUCT "-probe.txt";
}

void ProbeOnTick(const std::string& request) {
    static int done = 0;
    ULONGLONG since = GetTickCount64() - g_readyTick;
    if (done == 0 && since > 5000) WriteProbe("auto: 5 s after connecting"), done = 1;
    else if (done == 1 && since > 60000) WriteProbe("auto: 60 s after connecting"), done = 2;
    if (!request.empty()) WriteProbe(request.c_str());
}

// ---------------------------------------------------------------- game thread

// Once per frame, before the game's sinks run: all setting happens here.
void GameTick() {
    ++g_ticks;
    if (g_live.load() != Live::Ready) return;
    // Copy the requests; g_state is let go before anything is set (see the header).
    bool dirty = false;
    std::map<std::wstring, std::wstring> want;
    std::vector<std::pair<std::wstring, std::wstring>> console;
    std::string probe;
    {
        std::unique_lock<std::recursive_mutex> lock(g_state, std::try_to_lock);
        if (lock.owns_lock()) {
            dirty = g_dirty.exchange(false);
            if (dirty) want = Wanted();
            console.swap(g_pendingConsole);
            probe.swap(g_pendingProbe);
        }
    }
    for (auto& [key, value] : console) {
        bool ok = SetVar(key, value);
        Log("console: %ls = %ls %s", key.c_str(), value.c_str(), ok ? "set" : "NOT taken");
    }
    if (!console.empty()) PublishView();
    static bool first = true;
    if (first) {  // the game's values of everything the options touch, for reference
        first = false;
        std::set<std::wstring> keys = {L"r.ScreenPercentage", L"r.TemporalAA.Upsampling",
                                       L"r.DefaultFeature.AntiAliasing", L"r.PostProcessAAQuality",
                                       L"r.HDR.EnableHDROutput"};
        for (const Option& o : g_options)
            for (const Choice& c : o.choices)
                for (auto& [key, value] : c.cvars) keys.insert(key);
        for (const std::wstring& key : keys) {
            std::wstring v;
            VarInfo info = Lookup(key);
            Log("game value: %ls (%s, priority 0x%02x, %d callbacks) = %ls", key.c_str(), cvars::TypeName(info.type),
                info.var ? cvars::Priority(info.var) : -1, info.var ? Callbacks(info.var) : 0,
                Read(key, v) ? v.c_str() : L"(not found)");
        }
    }
    if (dirty) Apply(std::move(want));
    Hold();
    ProbeOnTick(probe);
}

void __fastcall HookedSinks(void* self) {
    GameTick();
    g_origSinks(self);  // then the game's sinks, which see what GameTick set
}

// ---------------------------------------------------------------- presets
//
// A preset sets the quality options (option key -> choice id); every other quality option goes
// to Game. Options in no preset are left alone. The preset shown is derived from the options,
// "Custom" when none matches. Not measured in this game yet: first guesses from what measured
// cheap in True Colors (same studio, UE 4.25).

struct QualityPreset {
    const char* label;
    const char* cost;  // shown next to the label in the picker ("" = not measured)
    const char* tip;
    std::vector<std::pair<const char*, int>> values;
};

const QualityPreset kPresets[] = {
    {"Game", "", "The game's own settings: everything below at Game.", {}},
    {"Ultra+", "",
     "Everything beyond the menu that should cost little: sharper and longer shadow maps,\n"
     "full-resolution character shadows and skin, the most detailed characters and models,\n"
     "finer lighting on translucent effects, 8 anti-aliasing samples, 8 GB textures and 16x\n"
     "filtering.\n\n"
     "On an RTX 5080 at 4K (DirectX 12, 156 fps cap) it cost no frames: the GPU stayed well\n"
     "below its limit. Use Measure preset costs on your PC.",
     {{"ShadowRes", 4096}, {"ShadowDistance", 150}, {"CapsuleShadows", 1}, {"Skin", 1}, {"CharacterLod", 1},
      {"TranslucencyLighting", 128}, {"TexturePool", 8192}, {"Anisotropy", 16}, {"MeshDetail", 1}, {"TaaSamples", 8}}},
    {"Cinematic", "",
     "Ultra+ plus reflections on rougher surfaces, the sharpest sun shadows, shadows twice\n"
     "as far and 3x draw distance.\n\n"
     "On an RTX 5080 at 4K (DirectX 12, 156 fps cap) it cost no frames: the GPU stayed well\n"
     "below its limit. Use Measure preset costs on your PC.",
     {{"Ssr", 5}, {"ShadowRes", 8192}, {"ShadowDistance", 200}, {"CapsuleShadows", 1}, {"Skin", 1},
      {"CharacterLod", 1}, {"TranslucencyLighting", 128}, {"TexturePool", 8192}, {"Anisotropy", 16},
      {"MeshDetail", 1}, {"ViewDistance", 3}, {"TaaSamples", 8}}},
};
constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

Option* FindOption(const char* key) {
    for (Option& o : g_options)
        if (!strcmp(o.key, key)) return &o;
    return nullptr;
}

std::set<std::string> PresetKeys() {
    std::set<std::string> keys;
    for (const QualityPreset& p : kPresets)
        for (auto& [key, id] : p.values) keys.insert(key);
    return keys;
}

int PresetValue(const QualityPreset& p, const std::string& key) {
    for (auto& [k, id] : p.values)
        if (key == k) return id;
    return 0;
}

int CurrentPreset() {
    std::set<std::string> keys = PresetKeys();
    for (int i = 0; i < kPresetCount; ++i) {
        bool match = true;
        for (const std::string& key : keys) {
            const Option* o = FindOption(key.c_str());
            if (o && o->value != PresetValue(kPresets[i], key)) match = false;
        }
        if (match) return i;
    }
    return -1;
}

void ApplyPreset(int index) {
    for (const std::string& key : PresetKeys())
        if (Option* o = FindOption(key.c_str())) o->value = PresetValue(kPresets[index], key);
    g_dirty = true;
}

// ---------------------------------------------------------------- frame time

int64_t g_qpcFreq = 1;
std::vector<float> g_frameMs;  // recent frames, a ring of kFrameRing
size_t g_framePos = 0;
constexpr size_t kFrameRing = 240;

// The average, not the median: with frame generation the present intervals alternate.
float Mean(const std::vector<float>& v) {
    if (v.empty()) return 0;
    double sum = 0;
    for (float x : v) sum += x;
    return static_cast<float>(sum / v.size());
}

// A measurement runs stages one after another: each sets the quality options (key -> choice id,
// every other measured option at Game), settles, then averages the frame time. "Presets" runs
// Game and each preset; "Each option" runs Game, then every quality option (all tabs but
// Post-processing and HDR) at each of its choices, one at a time, then Game again (to show
// drift).
struct Stage {
    std::string label;
    std::vector<std::pair<std::string, int>> values;
};

struct Measure {
    bool running = false;
    bool perOption = false;
    ULONGLONG settleMs = 6000, sampleMs = 8000;
    std::vector<Stage> stages;
    std::set<std::string> keys;  // the options the stages control
    size_t stage = 0;
    ULONGLONG stageStart = 0;
    bool sampling = false;
    std::vector<float> samples;
    std::vector<std::pair<std::string, int>> saved;
    std::vector<float> resultMs;
    bool done = false;
    std::string note;
    std::string summary;
} g_measure;

void ApplyStage(const Stage& st) {
    for (const std::string& key : g_measure.keys)
        if (Option* o = FindOption(key.c_str())) o->value = 0;
    for (auto& [key, id] : st.values)
        if (Option* o = FindOption(key.c_str())) o->value = id;
    g_dirty = true;
}

bool Measurable(const Option& o) {
    return strcmp(o.tab, kPostTab) && strcmp(o.tab, kHdrTab);
}

void StartMeasure(bool perOption) {
    g_measure = {};
    g_measure.running = true;
    g_measure.perOption = perOption;
    if (perOption) g_measure.settleMs = 5000, g_measure.sampleMs = 6000;
    for (const Option& o : g_options) g_measure.saved.push_back({o.key, o.value});
    g_measure.stages.push_back({"Game", {}});
    if (perOption) {
        for (const Option& o : g_options) {
            if (!Measurable(o)) continue;
            g_measure.keys.insert(o.key);
            for (const Choice& c : o.choices)
                if (c.id) g_measure.stages.push_back({std::string(o.key) + " " + c.label, {{o.key, c.id}}});
        }
        g_measure.stages.push_back({"Game (again)", {}});
    } else {
        g_measure.keys = PresetKeys();
        for (int i = 1; i < kPresetCount; ++i) {
            Stage st{kPresets[i].label, {}};
            for (auto& [key, id] : kPresets[i].values) st.values.push_back({key, id});
            g_measure.stages.push_back(st);
        }
    }
    g_measure.resultMs.assign(g_measure.stages.size(), 0);
    g_measure.stageStart = GetTickCount64();
    ApplyStage(g_measure.stages[0]);
    Log("measure: started (%s, %zu stages, %s)", perOption ? "each option" : "presets", g_measure.stages.size(), ApiName());
}

void FinishMeasure(const char* why) {
    for (auto& [key, value] : g_measure.saved)
        if (Option* o = FindOption(key.c_str())) o->value = value;
    g_dirty = true;
    g_measure.running = false;
    g_measure.note = why;
    Log("measure: %s", why);
}

void SummarizeMeasure() {
    const std::vector<float>& r = g_measure.resultMs;
    const float base = g_measure.perOption ? (r.front() + r.back()) / 2 : r.front();
    char line[160];
    if (!g_measure.perOption) {
        std::string sum = "Game " + std::to_string(static_cast<int>(1000.0f / r[0] + 0.5f)) + " fps";
        std::string log = "measure: result";
        for (size_t i = 1; i < r.size(); ++i) {
            snprintf(line, sizeof(line), ", %s %.0f fps (%+.0f%% frame time)", g_measure.stages[i].label.c_str(),
                     1000.0f / r[i], (r[i] / base - 1) * 100);
            sum += line;
            snprintf(line, sizeof(line), ", %s %.2f ms %+.0f%% frame time", g_measure.stages[i].label.c_str(), r[i],
                     (r[i] / base - 1) * 100);
            log += line;
        }
        g_measure.summary = sum;
        Log("%s (Game %.2f ms)", log.c_str(), r[0]);
        return;
    }
    // Each option, costliest first.
    std::vector<size_t> order;
    for (size_t i = 1; i + 1 < r.size(); ++i) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](size_t x, size_t y) { return r[x] > r[y]; });
    Log("measure: each option vs Game %.2f ms (start %.2f, end %.2f), costliest first:", base, r.front(), r.back());
    std::string sum;
    for (size_t k = 0; k < order.size(); ++k) {
        size_t i = order[k];
        Log("measure:   %+6.1f%%  %+6.2f ms  %s", (r[i] / base - 1) * 100, r[i] - base, g_measure.stages[i].label.c_str());
        if (k < 4) {
            snprintf(line, sizeof(line), "%s%s %+.0f%%", k ? ", " : "Costliest: ", g_measure.stages[i].label.c_str(),
                     (r[i] / base - 1) * 100);
            sum += line;
        }
    }
    g_measure.summary = sum + " (all in " DT_PRODUCT ".log)";
}

void MeasureOnFrame(float ms, bool focused) {
    if (!g_measure.running) return;
    if (!focused) return FinishMeasure("stopped: the game lost focus");
    if (!g_enabled) return FinishMeasure("stopped: tweaks switched off (F8)");
    ULONGLONG now = GetTickCount64();
    if (!g_measure.sampling) {
        if (now - g_measure.stageStart >= g_measure.settleMs) g_measure.sampling = true, g_measure.stageStart = now;
        return;
    }
    g_measure.samples.push_back(ms);
    if (now - g_measure.stageStart < g_measure.sampleMs) return;
    float avg = Mean(g_measure.samples);
    g_measure.resultMs[g_measure.stage] = avg;
    Log("measure: %s %.2f ms average (%.1f fps) over %zu frames", g_measure.stages[g_measure.stage].label.c_str(), avg,
        1000.0f / avg, g_measure.samples.size());
    g_measure.samples.clear();
    g_measure.sampling = false;
    g_measure.stageStart = now;
    if (++g_measure.stage < g_measure.stages.size()) {
        ApplyStage(g_measure.stages[g_measure.stage]);
        return;
    }
    g_measure.done = true;
    SummarizeMeasure();
    FinishMeasure("done");
}

void FrameOnPresent(bool focused) {
    static int64_t last = 0;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    float ms = last ? static_cast<float>((t.QuadPart - last) * 1000.0 / g_qpcFreq) : 0;
    last = t.QuadPart;
    if (!ms || !focused || ms > 500) {
        if (g_measure.running && !focused) MeasureOnFrame(ms, false);
        return;
    }
    if (g_frameMs.size() < kFrameRing) g_frameMs.push_back(ms);
    else g_frameMs[g_framePos] = ms;
    g_framePos = (g_framePos + 1) % kFrameRing;
    MeasureOnFrame(ms, true);
}

// ---------------------------------------------------------------- present

// ---------------------------------------------------------------- HDR
//
// The game stays in SDR (its back buffer is R10G10B10A2). At each present, before ReShade's
// effects and overlay, the add-on copies the finished frame and draws it back through
// src/hdr.hlsl: SDR -> linear at the chosen paper white, highlights stretched to the chosen
// peak, Rec. 2020, PQ. The swap chain is set to HDR10 (DXGI SetColorSpace1), and ReShade is told
// too (set_color_space) so its overlay and screenshots follow.
//
// Unreal's own HDR output (r.HDR.EnableHDROutput) isn't offered: it swaps the game's tone curve
// for ACES and drops its colour grade (docs/FINDINGS.md).

constexpr int kGradedHdr = 1;  // the Hdr option's "on" choice

std::string g_hdrStatus = "off";

effect_runtime* g_runtime = nullptr;  // ReShade's runtime for the game's swap chain
bool g_gradedActive = false;          // the graded pass is running and the swap chain is HDR10
bool g_colorSpaceDirty = true;        // set again after a swap chain (re)creation
float g_displayPeak = 0;              // what the display reports (DXGI), 0 = unknown

// The graded pass's GPU objects. Created on the presenting device, rebuilt when it changes; the
// copy and the back buffer views go whenever the swap chain is resized.
struct GradedPass {
    device* dev = nullptr;
    pipeline_layout layout = {0};
    pipeline pipe = {0};
    format pipeFormat = format::unknown;
    resource copy = {0};
    resource_view copyView = {0};
    resource scene = {0};  // the frame just before its first UI draw
    resource_view sceneView = {0};
    uint32_t width = 0, height = 0;
    format copyFormat = format::unknown;
    std::map<uint64_t, resource_view> targets;  // back buffer -> render target view
    bool failed = false;
} g_pass;

void ReleaseTargets() {
    device* dev = g_pass.dev;
    if (!dev) return;
    for (auto& [bb, rtv] : g_pass.targets) dev->destroy_resource_view(rtv);
    g_pass.targets.clear();
    if (g_pass.copyView.handle) dev->destroy_resource_view(g_pass.copyView);
    if (g_pass.copy.handle) dev->destroy_resource(g_pass.copy);
    if (g_pass.sceneView.handle) dev->destroy_resource_view(g_pass.sceneView);
    if (g_pass.scene.handle) dev->destroy_resource(g_pass.scene);
    g_pass.copyView = {0};
    g_pass.copy = {0};
    g_pass.sceneView = {0};
    g_pass.scene = {0};
    g_pass.width = g_pass.height = 0;
}

void ReleasePass() {
    ReleaseTargets();
    if (g_pass.dev) {
        if (g_pass.pipe.handle) g_pass.dev->destroy_pipeline(g_pass.pipe);
        if (g_pass.layout.handle) g_pass.dev->destroy_pipeline_layout(g_pass.layout);
    }
    g_pass = {};
}

bool CreatePipeline(device* dev, format target) {
    pipeline_layout_param params[2];
    params[0] = constant_range{0, 0, 0, 8, shader_stage::pixel};  // b0: 8 floats
    params[1] = descriptor_range{0, 0, 0, 2, shader_stage::pixel, 1, descriptor_type::shader_resource_view};  // t0, t1
    if (!dev->create_pipeline_layout(2, params, &g_pass.layout)) return false;
    shader_desc vs = {};
    vs.code = kHdrVs;
    vs.code_size = sizeof(kHdrVs);
    shader_desc ps = {};
    ps.code = kHdrPs;
    ps.code_size = sizeof(kHdrPs);
    primitive_topology topology = primitive_topology::triangle_list;
    blend_desc blend = {};
    rasterizer_desc raster = {};
    raster.cull_mode = cull_mode::none;
    depth_stencil_desc depth = {};
    depth.depth_enable = false;
    pipeline_subobject subobjects[] = {
        {pipeline_subobject_type::vertex_shader, 1, &vs},
        {pipeline_subobject_type::pixel_shader, 1, &ps},
        {pipeline_subobject_type::render_target_formats, 1, &target},
        {pipeline_subobject_type::primitive_topology, 1, &topology},
        {pipeline_subobject_type::blend_state, 1, &blend},
        {pipeline_subobject_type::rasterizer_state, 1, &raster},
        {pipeline_subobject_type::depth_stencil_state, 1, &depth},
    };
    if (!dev->create_pipeline(g_pass.layout, 7, subobjects, &g_pass.pipe)) return false;
    g_pass.pipeFormat = target;
    return true;
}

// The copy of the frame and a render target view of <bb>, (re)made as needed.
bool EnsureTargets(device* dev, resource bb, const resource_desc& desc, resource_view* rtv) {
    const uint32_t w = desc.texture.width, h = desc.texture.height;
    const format f = desc.texture.format;
    if (!g_pass.copy.handle || g_pass.width != w || g_pass.height != h || g_pass.copyFormat != f) {
        ReleaseTargets();
        resource_desc cd(w, h, 1, 1, f, 1, memory_heap::default_, resource_usage::copy_dest | resource_usage::shader_resource);
        if (!dev->create_resource(cd, nullptr, resource_usage::shader_resource_pixel, &g_pass.copy)) return false;
        if (!dev->create_resource_view(g_pass.copy, resource_usage::shader_resource, resource_view_desc(f), &g_pass.copyView))
            return false;
        if (!dev->create_resource(cd, nullptr, resource_usage::shader_resource_pixel, &g_pass.scene)) return false;
        if (!dev->create_resource_view(g_pass.scene, resource_usage::shader_resource, resource_view_desc(f), &g_pass.sceneView))
            return false;
        g_pass.width = w, g_pass.height = h, g_pass.copyFormat = f;
    }
    auto it = g_pass.targets.find(bb.handle);
    if (it == g_pass.targets.end()) {
        resource_view v = {0};
        if (!dev->create_resource_view(bb, resource_usage::render_target, resource_view_desc(f), &v)) return false;
        it = g_pass.targets.emplace(bb.handle, v).first;
    }
    *rtv = it->second;
    return true;
}

std::atomic<bool> g_inPass{false};  // our own commands: the draw tracking ignores them
int g_hdrDebugView = 0;             // Advanced tab: 0 off, 1 tint UI, 2 show the scene-only copy

// Draws the frame back into the back buffer as HDR10. Present thread, before ReShade's own work.
// <uiMask>: g_pass.scene holds this frame before its UI.
bool RunGradedPass(command_queue* queue, swapchain* sc, float paperWhite, float peak, bool uiMask) {
    device* dev = sc->get_device();
    const resource bb = sc->get_current_back_buffer();
    const resource_desc desc = dev->get_resource_desc(bb);
    if (g_pass.dev != dev) {
        ReleasePass();
        g_pass.dev = dev;
    }
    if (g_pass.failed) return false;
    if (!g_pass.pipe.handle || g_pass.pipeFormat != desc.texture.format) {
        if (g_pass.pipe.handle) dev->destroy_pipeline(g_pass.pipe), g_pass.pipe = {0};
        if (g_pass.layout.handle) dev->destroy_pipeline_layout(g_pass.layout), g_pass.layout = {0};
        if (!CreatePipeline(dev, desc.texture.format)) {
            Log("graded HDR: couldn't create the pipeline");
            g_pass.failed = true;
            return false;
        }
        Log("graded HDR: pipeline ready (%s, back buffer %ux%u, format %u)", ApiName(), desc.texture.width,
            desc.texture.height, static_cast<uint32_t>(desc.texture.format));
    }
    resource_view rtv = {0};
    if (!EnsureTargets(dev, bb, desc, &rtv)) {
        Log("graded HDR: couldn't create the frame copy or the back buffer view");
        g_pass.failed = true;
        return false;
    }
    g_inPass = true;
    command_list* cmd = queue->get_immediate_command_list();
    {
        const resource res[2] = {bb, g_pass.copy};
        const resource_usage from[2] = {resource_usage::present, resource_usage::shader_resource_pixel};
        const resource_usage to[2] = {resource_usage::copy_source, resource_usage::copy_dest};
        cmd->barrier(2, res, from, to);
    }
    cmd->copy_resource(bb, g_pass.copy);
    {
        const resource res[2] = {bb, g_pass.copy};
        const resource_usage from[2] = {resource_usage::copy_source, resource_usage::copy_dest};
        const resource_usage to[2] = {resource_usage::render_target, resource_usage::shader_resource_pixel};
        cmd->barrier(2, res, from, to);
    }
    cmd->bind_pipeline(pipeline_stage::all_graphics, g_pass.pipe);
    // knee 0.6, gamma 2.2 decode, UI mask, debug view (Advanced tab)
    const float params[8] = {paperWhite, peak, 0.6f, 2.2f, uiMask ? 1.0f : 0.0f, static_cast<float>(g_hdrDebugView), 0, 0};
    cmd->push_constants(shader_stage::pixel, g_pass.layout, 0, 0, 8, params);
    const resource_view views[2] = {g_pass.copyView, g_pass.sceneView};
    descriptor_table_update update = {};
    update.count = 2;
    update.type = descriptor_type::shader_resource_view;
    update.descriptors = views;
    cmd->push_descriptors(shader_stage::pixel, g_pass.layout, 1, update);
    const viewport vp = {0, 0, static_cast<float>(desc.texture.width), static_cast<float>(desc.texture.height), 0, 1};
    cmd->bind_viewports(0, 1, &vp);
    const rect scissor = {0, 0, static_cast<int32_t>(desc.texture.width), static_cast<int32_t>(desc.texture.height)};
    cmd->bind_scissor_rects(0, 1, &scissor);
    cmd->bind_render_targets_and_depth_stencil(1, &rtv);
    cmd->draw(3, 1, 0, 0);
    {
        const resource_usage from = resource_usage::render_target, to = resource_usage::present;
        cmd->barrier(1, &bb, &from, &to);
    }
    queue->flush_immediate_command_list();
    g_inPass = false;
    return true;
}

// ---------------------------------------------------------------- UI split
//
// Unreal draws each frame's picture into the back buffer with one full-screen draw (the
// tonemapper/upscale), then its UI with Slate on top: seen in Life is Strange 2 (UE 4.16, draw
// logs in LiS2Tweaks), to be confirmed here with the Advanced tab's draw log. So the first draw
// into a back buffer each frame is the scene and any later one is UI: right before the second,
// the back buffer is copied into g_pass.scene. At present the HDR pass compares the frame with
// that copy and keeps UI pixels at paper white. A frame without UI draws gets no copy and is
// stretched everywhere, as before.

constexpr int kMaxBackBuffers = 4;
std::atomic<uint64_t> g_backBuffers[kMaxBackBuffers] = {};  // the swap chain's buffers
std::atomic<uint64_t> g_uiBoundBackBuffer{0};               // back buffer bound as target 0, or 0
std::atomic<int> g_uiBackBufferDraws{0};                    // draws into back buffers this frame
std::atomic<uint64_t> g_uiSnapshotOf{0};                    // back buffer copied this frame, or 0
std::atomic<uint64_t> g_uiFramesWithUi{0}, g_uiFrames{0};   // for the Advanced tab

bool IsBackBuffer(uint64_t handle) {
    if (!handle) return false;
    for (auto& b : g_backBuffers)
        if (b.load() == handle) return true;
    return false;
}

void UiNoteBackBuffers(swapchain* sc) {
    const uint32_t n = std::min<uint32_t>(sc->get_back_buffer_count(), kMaxBackBuffers);
    for (uint32_t i = 0; i < kMaxBackBuffers; ++i) g_backBuffers[i] = i < n ? sc->get_back_buffer(i).handle : 0;
}

void UiOnBindRenderTargets(command_list* cmd, uint32_t count, const resource_view* rtvs) {
    if (g_inPass) return;
    uint64_t bb = 0;
    if (g_gradedActive && count && rtvs[0].handle) {
        const resource r = cmd->get_device()->get_resource_from_view(rtvs[0]);
        if (IsBackBuffer(r.handle)) bb = r.handle;
    }
    g_uiBoundBackBuffer = bb;
}

// Before each draw: on the frame's second draw into a back buffer (its first UI draw), copy it.
void UiOnDraw(command_list* cmd) {
    const uint64_t bb = g_uiBoundBackBuffer.load();
    if (g_inPass || !bb || !g_gradedActive || !g_pass.scene.handle || g_pass.dev != cmd->get_device()) return;
    if (++g_uiBackBufferDraws != 2 || g_uiSnapshotOf.load()) return;
    const resource back = {bb};
    {
        const resource res[2] = {back, g_pass.scene};
        const resource_usage from[2] = {resource_usage::render_target, resource_usage::shader_resource_pixel};
        const resource_usage to[2] = {resource_usage::copy_source, resource_usage::copy_dest};
        cmd->barrier(2, res, from, to);
    }
    cmd->copy_resource(back, g_pass.scene);
    {
        const resource res[2] = {back, g_pass.scene};
        const resource_usage from[2] = {resource_usage::copy_source, resource_usage::copy_dest};
        const resource_usage to[2] = {resource_usage::render_target, resource_usage::shader_resource_pixel};
        cmd->barrier(2, res, from, to);
    }
    g_uiSnapshotOf = bb;
    static std::atomic<int> logged{0};
    if (logged++ < 6) Log("HDR UI: copied back buffer %016llx before draw 2 (thread %lu)", static_cast<unsigned long long>(bb), GetCurrentThreadId());
}

// The display's peak brightness as Windows reports it (DXGI_OUTPUT_DESC1), or 0.
float QueryDisplayPeak(swapchain* sc) {
    auto native = reinterpret_cast<IUnknown*>(sc->get_native());
    IDXGISwapChain* chain = nullptr;
    float peak = 0;
    if (native && SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&chain))) && chain) {
        IDXGIOutput* output = nullptr;
        if (SUCCEEDED(chain->GetContainingOutput(&output)) && output) {
            IDXGIOutput6* output6 = nullptr;
            if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&output6))) && output6) {
                DXGI_OUTPUT_DESC1 d = {};
                if (SUCCEEDED(output6->GetDesc1(&d))) peak = d.MaxLuminance;
                output6->Release();
            }
            output->Release();
        }
        chain->Release();
    }
    return peak;
}

bool SetSwapchainColorSpace(swapchain* sc, DXGI_COLOR_SPACE_TYPE cs) {
    auto native = reinterpret_cast<IUnknown*>(sc->get_native());
    IDXGISwapChain3* sc3 = nullptr;
    if (!native || FAILED(native->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) return false;
    UINT support = 0;
    bool ok = SUCCEEDED(sc3->CheckColorSpaceSupport(cs, &support)) &&
              (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) && SUCCEEDED(sc3->SetColorSpace1(cs));
    sc3->Release();
    return ok;
}

// Every frame: the pass while HDR is wanted, and the colour space switched to match.
void HdrOnPresent(command_queue* queue, swapchain* sc) {
    const Option* o = FindOption("Hdr");
    const bool want = o && o->value == kGradedHdr && g_enabled;
    if (want && g_displayPeak == 0) {
        g_displayPeak = QueryDisplayPeak(sc);
        if (g_displayPeak < 0) g_displayPeak = 0;
        Log("display reports %.0f nits peak", g_displayPeak);
        if (g_displayPeak == 0) g_displayPeak = -1;  // asked; don't ask every frame
    }
    bool ran = false;
    if (want) {
        const float paper = static_cast<float>(g_hdrPaper);
        const float peak = static_cast<float>(std::max(g_hdrPeak, g_hdrPaper));
        const uint64_t presented = sc->get_current_back_buffer().handle;
        const bool ui = g_uiSnapshotOf.load() != 0 && g_uiSnapshotOf.load() == presented;
        static int logged = 0;
        if (g_uiSnapshotOf.load() && logged++ < 6)
            Log("HDR UI: present of %016llx (thread %lu): copy of %016llx, %d back buffer draws -> mask %s",
                static_cast<unsigned long long>(presented), GetCurrentThreadId(),
                static_cast<unsigned long long>(g_uiSnapshotOf.load()), g_uiBackBufferDraws.load(), ui ? "on" : "off");
        ran = RunGradedPass(queue, sc, paper, peak, ui);
        ++g_uiFrames;
        if (ui) ++g_uiFramesWithUi;
    }
    // The next frame starts: no back buffer drawn yet, no copy.
    g_uiBackBufferDraws = 0;
    g_uiSnapshotOf = 0;
    g_uiBoundBackBuffer = 0;
    UiNoteBackBuffers(sc);
    if (ran && (!g_gradedActive || g_colorSpaceDirty)) {
        const bool ok = SetSwapchainColorSpace(sc, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        if (g_runtime) g_runtime->set_color_space(color_space::hdr10_pq);
        Log("graded HDR on: swap chain HDR10 %s (paper white %d, peak %d nits)", ok ? "set" : "REFUSED", g_hdrPaper,
            g_hdrPeak);
        g_hdrStatus = ok ? "HDR10" : "the display refused HDR10 (is Windows HDR on?)";
        g_gradedActive = true;
        g_colorSpaceDirty = false;
    } else if (!ran && g_gradedActive) {
        const bool ok = SetSwapchainColorSpace(sc, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        if (g_runtime) g_runtime->set_color_space(color_space::srgb);
        Log("graded HDR off: swap chain back to sRGB %s", ok ? "done" : "FAILED");
        g_gradedActive = false;
        g_hdrStatus = "off";
    }
    if (want && !ran) g_hdrStatus = "HDR failed (see the log)";
}

color_space g_colorSpace = color_space::unknown;
bool g_keyWasDown = false;

void OnInitRuntime(effect_runtime* runtime) {
    g_runtime = runtime;
    g_colorSpaceDirty = true;
}

void OnDestroyRuntime(effect_runtime* runtime) {
    if (g_runtime == runtime) g_runtime = nullptr;
}

// A resize or re-creation: the back buffers and their views go away; the colour space is set again.
void OnDestroySwapchain(swapchain* sc, bool) {
    if (g_pass.dev == sc->get_device()) ReleaseTargets();
    g_colorSpaceDirty = true;
}

void OnDestroyDevice(device* dev) {
    if (g_pass.dev == dev) ReleasePass();
}

void LiveOnPresent() {
    if (g_live.load() != Live::Idle) return;
    g_live = Live::Searching;
    std::thread(FindManager).detach();
}

// ---------------------------------------------------------------- draw log (diagnostics)
//
// Logs one frame's work on the back buffer: render target binds, clears, copies and draws while
// the back buffer is bound (vertex/index counts, pixel shader handle), to find where the game's
// picture ends and its UI begins (see the UI split above). Started from the Advanced tab.

std::mutex g_drawLogMutex;
resource g_backBuffer = {0};          // the back buffer of the frame being drawn
std::map<uint64_t, bool> g_rtvIsBackBuffer;
std::atomic<bool> g_drawLogArmed{false};  // log the next frame
bool g_drawLogging = false;               // logging this frame
bool g_backBufferBound = false;
uint64_t g_pixelShader = 0;
int g_drawLogLines = 0, g_drawsElsewhere = 0, g_drawSeq = 0;
FILE* g_drawLog = nullptr;

void DrawLog(const char* fmt, ...) {
    if (!g_drawLog || g_drawLogLines >= 400) return;
    ++g_drawLogLines;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_drawLog, fmt, ap);
    va_end(ap);
    fputc('\n', g_drawLog);
}

bool IsBackBufferView(device* dev, resource_view rtv) {
    if (!rtv.handle || !g_backBuffer.handle) return false;
    auto it = g_rtvIsBackBuffer.find(rtv.handle);
    if (it != g_rtvIsBackBuffer.end()) return it->second;
    const bool bb = IsBackBuffer(dev->get_resource_from_view(rtv).handle);
    g_rtvIsBackBuffer[rtv.handle] = bb;
    return bb;
}

uint32_t g_bbWidth = 0, g_bbHeight = 0;
uint64_t g_boundTarget = 0;  // resource of render target 0 while it's screen-sized, else 0

void OnBindRenderTargets(command_list* cmd, uint32_t count, const resource_view* rtvs, resource_view) {
    UiOnBindRenderTargets(cmd, count, rtvs);
    if (!g_drawLogging) return;
    std::lock_guard<std::mutex> lock(g_drawLogMutex);
    device* dev = cmd->get_device();
    bool bb = false;
    for (uint32_t i = 0; i < count; ++i) bb |= IsBackBufferView(dev, rtvs[i]);
    uint64_t target = 0;
    resource_desc d = {};
    if (g_drawLogging && count && rtvs[0].handle) {
        const resource r = dev->get_resource_from_view(rtvs[0]);
        if (r.handle) {
            d = dev->get_resource_desc(r);
            if (d.texture.width == g_bbWidth && d.texture.height == g_bbHeight) target = r.handle;
        }
    }
    // Screen-sized targets (or the back buffer) are logged with their draws; everything else is
    // only counted.
    const bool shown = bb || target;
    if (g_drawLogging && (shown || g_boundTarget || g_backBufferBound) && (target != g_boundTarget || bb != g_backBufferBound)) {
        if (shown)
            DrawLog("%5d  render target %016llx%s %ux%u format %u (%u bound; %d draws elsewhere before)", g_drawSeq,
                    static_cast<unsigned long long>(target), bb ? " = BACK BUFFER" : "", d.texture.width,
                    d.texture.height, static_cast<uint32_t>(d.texture.format), count, g_drawsElsewhere);
        else
            DrawLog("%5d  render target: smaller/other", g_drawSeq);
        g_drawsElsewhere = 0;
    }
    g_boundTarget = target;
    g_backBufferBound = bb || target;
}

void OnBindPipeline(command_list*, pipeline_stage stages, pipeline p) {
    if ((stages & pipeline_stage::pixel_shader) == pipeline_stage::pixel_shader) g_pixelShader = p.handle;
}

void LogDraw(const char* kind, uint32_t count, uint32_t instances) {
    std::lock_guard<std::mutex> lock(g_drawLogMutex);
    ++g_drawSeq;
    if (!g_drawLogging) return;
    if (!g_backBufferBound) {
        ++g_drawsElsewhere;
        return;
    }
    DrawLog("%5d    %s %u x%u  ps %016llx", g_drawSeq, kind, count, instances,
            static_cast<unsigned long long>(g_pixelShader));
}

bool OnDraw(command_list* cmd, uint32_t vertices, uint32_t instances, uint32_t, uint32_t) {
    UiOnDraw(cmd);
    if (g_drawLogging) LogDraw("draw", vertices, instances);
    return false;
}

bool OnDrawIndexed(command_list* cmd, uint32_t indices, uint32_t instances, uint32_t, int32_t, uint32_t) {
    UiOnDraw(cmd);
    if (g_drawLogging) LogDraw("draw_indexed", indices, instances);
    return false;
}

bool OnResolve(command_list*, resource src, uint32_t, const subresource_box*, resource dst, uint32_t, uint32_t,
               uint32_t, uint32_t, format) {
    std::lock_guard<std::mutex> lock(g_drawLogMutex);
    if (g_drawLogging)
        DrawLog("%5d  resolve %016llx -> %016llx%s", g_drawSeq, static_cast<unsigned long long>(src.handle),
                static_cast<unsigned long long>(dst.handle), dst.handle == g_backBuffer.handle ? " (BACK BUFFER)" : "");
    return false;
}

bool OnClearRtv(command_list* cmd, resource_view rtv, const float c[4], uint32_t, const rect*) {
    std::lock_guard<std::mutex> lock(g_drawLogMutex);
    if (g_drawLogging && IsBackBufferView(cmd->get_device(), rtv))
        DrawLog("%5d  clear BACK BUFFER to %.2f %.2f %.2f %.2f", g_drawSeq, c[0], c[1], c[2], c[3]);
    return false;
}

bool OnCopyResource(command_list*, resource src, resource dst) {
    std::lock_guard<std::mutex> lock(g_drawLogMutex);
    if (g_drawLogging)
        DrawLog("%5d  copy_resource %016llx -> %016llx%s", g_drawSeq, static_cast<unsigned long long>(src.handle),
                static_cast<unsigned long long>(dst.handle), dst.handle == g_backBuffer.handle ? " (BACK BUFFER)" : "");
    return false;
}

bool OnCopyRegion(command_list*, resource src, uint32_t, const subresource_box*, resource dst, uint32_t,
                  const subresource_box*, filter_mode) {
    std::lock_guard<std::mutex> lock(g_drawLogMutex);
    if (g_drawLogging)
        DrawLog("%5d  copy_texture_region %016llx -> %016llx%s", g_drawSeq, static_cast<unsigned long long>(src.handle),
                static_cast<unsigned long long>(dst.handle), dst.handle == g_backBuffer.handle ? " (BACK BUFFER)" : "");
    return false;
}

// At each present: end the logged frame, start one if asked, note the next frame's back buffer.
void DrawLogOnPresent(swapchain* sc) {
    std::lock_guard<std::mutex> lock(g_drawLogMutex);
    if (g_drawLogging) {
        DrawLog("%5d  present (%d draws elsewhere at the end)", g_drawSeq, g_drawsElsewhere);
        fclose(g_drawLog);
        g_drawLog = nullptr;
        g_drawLogging = false;
        Log("draw log: %d lines written to " DT_PRODUCT "-draws.txt", g_drawLogLines);
    }
    const resource bb = sc->get_current_back_buffer();
    if (bb.handle != g_backBuffer.handle) g_rtvIsBackBuffer.clear();
    g_backBuffer = bb;
    const resource_desc bd = sc->get_device()->get_resource_desc(bb);
    g_bbWidth = bd.texture.width, g_bbHeight = bd.texture.height;
    if (g_drawLogArmed.exchange(false)) {
        g_drawLog = _wfopen((g_exeDir + DT_PRODUCT_W L"-draws.txt").c_str(), L"a");
        if (g_drawLog) {
            SYSTEMTIME t;
            GetLocalTime(&t);
            fprintf(g_drawLog, "\n==== frame %02u:%02u:%02u, HDR %s, back buffer %016llx %ux%u format %u\n", t.wHour,
                    t.wMinute, t.wSecond, g_gradedActive ? "on" : "off", static_cast<unsigned long long>(bb.handle),
                    bd.texture.width, bd.texture.height, static_cast<uint32_t>(bd.texture.format));
            g_drawLogging = true;
            g_drawLogLines = 0;
            g_drawSeq = 0;
            g_drawsElsewhere = 0;
        }
    }
}

void OnPresent(command_queue* queue, swapchain* sc, const rect*, const rect*, uint32_t, const rect*) {
    DrawLogOnPresent(sc);
    const int api = static_cast<int>(sc->get_device()->get_api());
    if (g_api.exchange(api) != api) Log("presenting on %s", ApiName());
    if (sc->get_color_space() != g_colorSpace) {
        g_colorSpace = sc->get_color_space();
        Log("swap chain colour space %s", g_colorSpace == color_space::hdr10_pq ? "HDR10 PQ"
                                          : g_colorSpace == color_space::srgb  ? "sRGB"
                                          : g_colorSpace == color_space::extended_srgb_linear ? "scRGB"
                                                                               : "other");
    }
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    const bool focused = pid == GetCurrentProcessId();
    std::lock_guard<std::recursive_mutex> lock(g_state);
    bool down = focused && (GetAsyncKeyState(kToggleHotkey) & 0x8000);
    if (down && !g_keyWasDown) {
        g_enabled = !g_enabled;
        g_dirty = true;
        Log("tweaks %s (F8)", g_enabled ? "on" : "off");
    }
    g_keyWasDown = down;
    FrameOnPresent(focused);
    LiveOnPresent();
    HdrOnPresent(queue, sc);
}

// ---------------------------------------------------------------- overlay

constexpr ImVec4 kGood(0.45f, 0.85f, 0.45f, 1.0f);
constexpr ImVec4 kWarn(1.0f, 0.8f, 0.3f, 1.0f);
constexpr ImVec4 kBad(1.0f, 0.45f, 0.35f, 1.0f);
constexpr ImVec4 kAccent(0.4f, 0.75f, 1.0f, 1.0f);

float ControlWidth() { return ImGui::GetFontSize() * 14.0f; }

void Help(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

void ChangedMark(bool changed) {
    if (changed)
        ImGui::TextColored(kAccent, "*");
    else
        ImGui::TextUnformatted(" ");
    ImGui::SameLine();
}

// The game's value of <name> as text: the one we saved if we're holding it, else the current
// (the renderer's copy, read from this thread).
std::string GameValue(const View& view, const wchar_t* name) {
    if (!name || g_live.load() != Live::Ready) return {};
    auto it = view.held.find(name);
    if (it != view.held.end()) return Narrow(it->second);
    std::wstring cur;
    return Read(name, cur) ? Narrow(cur) : std::string();
}

std::string ChoiceLabel(const View& view, const Option& o, const Choice& c) {
    if (c.id != 0 || !o.shown) return c.label;  // no game value to show: the choice's own label
    std::string v = GameValue(view, o.shown);
    return v.empty() ? "Game" : "Game (" + v + ")";
}

bool DrawOption(const View& view, Option& o) {
    bool changed = false;
    const Choice* cur = Chosen(o);
    ChangedMark(o.value != 0);
    ImGui::SetNextItemWidth(ControlWidth());
    std::string preview = cur ? ChoiceLabel(view, o, *cur) : "?";
    if (ImGui::BeginCombo(o.label, preview.c_str())) {
        for (const Choice& c : o.choices) {
            std::string label = ChoiceLabel(view, o, c);
            if (ImGui::Selectable(label.c_str(), c.id == o.value) && c.id != o.value) {
                o.value = c.id;
                changed = true;
                Log("option %s: %s", o.key, c.label);
            }
        }
        ImGui::EndCombo();
    }
    Help(o.tip);
    return changed;
}

// HDR's brightness sliders (HDR tab, while HDR10 is chosen). Saved at once;
// the pass reads them every frame.
void DrawHdrSliders() {
    const Option* o = FindOption("Hdr");
    if (!o || o->value != kGradedHdr) return;
    ImGui::SeparatorText("Brightness");
    bool save = false;
    ImGui::SetNextItemWidth(ControlWidth());
    save |= ImGui::SliderInt("Peak brightness", &g_hdrPeak, kPeakMin, kPeakMax, "%d nits", ImGuiSliderFlags_AlwaysClamp);
    Help("How bright the brightest parts of the picture get: the sun, lamps, reflections.\n"
         "Set it to your display's peak brightness (or a little below, for a softer look).\n"
         "Ctrl+click to type a value.");
    if (g_displayPeak > 0) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Use display's")) {
            g_hdrPeak = std::clamp(static_cast<int>(g_displayPeak + 0.5f), kPeakMin, kPeakMax);
            save = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Windows reports %.0f nits for this display.", g_displayPeak);
    }
    ImGui::SetNextItemWidth(ControlWidth());
    save |= ImGui::SliderInt("Paper white", &g_hdrPaper, kPaperMin, kPaperMax, "%d nits", ImGuiSliderFlags_AlwaysClamp);
    Help("How bright everything else is: walls, faces, the menus and subtitles. 200 is a\n"
         "common choice; raise it in a bright room, lower it in a dark one.");
    if (g_hdrPeak < g_hdrPaper) ImGui::TextColored(kWarn, "Peak is below paper white: highlights aren't stretched.");
    if (save) {
        reshade::set_config_value(nullptr, DT_PRODUCT, "HdrPeak", g_hdrPeak);
        reshade::set_config_value(nullptr, DT_PRODUCT, "HdrPaperWhite", g_hdrPaper);
        Log("graded HDR: peak %d, paper white %d nits", g_hdrPeak, g_hdrPaper);
    }
}

bool DrawTab(const View& view, const char* tab) {
    bool changed = false;
    const char* section = nullptr;
    for (Option& o : g_options) {
        if (strcmp(o.tab, tab)) continue;
        if (!section || strcmp(section, o.section)) {
            section = o.section;
            ImGui::SeparatorText(section);
        }
        changed |= DrawOption(view, o);
    }
    if (!strcmp(tab, kHdrTab)) DrawHdrSliders();
    ImGui::Spacing();
    bool any = false;
    for (const Option& o : g_options) any |= !strcmp(o.tab, tab) && o.value != 0;
    ImGui::BeginDisabled(!any);
    if (ImGui::Button("Set this tab to Game")) {
        for (Option& o : g_options)
            if (!strcmp(o.tab, tab)) o.value = 0;
        changed = true;
    }
    ImGui::EndDisabled();
    return changed;
}

char g_cvarName[128] = "r.Shadow.MaxCSMResolution";
char g_cvarValue[64] = "";
std::string g_cvarResult;

void DrawAdvanced(const View& view) {
    const Live live = g_live.load();
    ImGui::SeparatorText("Status");
    ImGui::TextDisabled("Game settings: %s", live == Live::Ready    ? "connected"
                                             : live == Live::Failed ? g_liveError.c_str()
                                                                    : "connecting...");
    ImGui::TextDisabled("Game thread: %s, %llu frames", g_hooked ? "reached" : "not reached",
                        static_cast<unsigned long long>(g_ticks.load()));
    ImGui::TextDisabled("Holding %zu settings; put back %d times after they changed", view.held.size(),
                        g_reapplied.load());
    if (!view.rejected.empty()) {
        std::string list;
        for (const std::string& k : view.rejected) list += (list.empty() ? "" : ", ") + k;
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
        ImGui::TextColored(kWarn, "Not taken: %s", list.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::TextDisabled("Graphics API: %s", ApiName());
    ImGui::TextDisabled("HDR: %s (swap chain %s)", g_hdrStatus.c_str(),
                        g_colorSpace == color_space::hdr10_pq ? "HDR10" : g_colorSpace == color_space::srgb ? "sRGB" : "other");
    if (g_gradedActive) {
        ImGui::TextDisabled("HDR UI kept at paper white in %llu of %llu frames so far",
                            static_cast<unsigned long long>(g_uiFramesWithUi.load()),
                            static_cast<unsigned long long>(g_uiFrames.load()));
        ImGui::SetNextItemWidth(ControlWidth());
        const char* views[] = {"Off", "Tint what counts as UI", "Show the picture before UI"};
        ImGui::Combo("HDR debug view", &g_hdrDebugView, views, 3);
        Help("For diagnosing HDR. Tint: pixels the add-on keeps at paper white as UI turn\n"
             "magenta. Picture before UI: shows the copy taken just before the UI is drawn.");
    }
    ImGui::TextDisabled("Log: " DT_PRODUCT ".log next to the game's exe");
    if (live != Live::Ready) return;

    ImGui::SeparatorText("Probe");
    if (ImGui::Button("Write all settings to file")) g_pendingProbe = "manual";
    Help("Writes the game's value of every lighting, shadow, detail and post-processing\n"
         "setting to " DT_PRODUCT "-probe.txt next to the game's exe.\n"
         "Do it in a scene (not a menu) for the values the game really plays with.");
    if (!view.probeResult.empty()) ImGui::TextDisabled("%s", view.probeResult.c_str());
    if (ImGui::Button("Log one frame's drawing")) g_drawLogArmed = true;
    Help("For diagnosing HDR: writes the next frame's render target changes and draw calls on\n"
         "the screen image to " DT_PRODUCT "-draws.txt next to the game's exe. ReShade's own\n"
         "overlay isn't included.");

    ImGui::SeparatorText("Console variable");
    ImGui::SetNextItemWidth(ControlWidth() * 1.6f);
    ImGui::InputText("Name", g_cvarName, sizeof(g_cvarName));
    std::wstring name(g_cvarName, g_cvarName + strlen(g_cvarName));
    std::wstring value;
    VarInfo v = Lookup(name);
    if (!v.var)
        ImGui::TextDisabled("Not found");
    else
        ImGui::TextDisabled("%s, now %s (set by 0x%02x)%s", cvars::TypeName(v.type),
                            Read(name, value) ? Narrow(value).c_str() : "?", cvars::Priority(v.var),
                            view.held.count(name) ? " (held by an option)" : "");
    ImGui::SetNextItemWidth(ControlWidth() * 0.6f);
    ImGui::InputText("##value", g_cvarValue, sizeof(g_cvarValue));
    ImGui::SameLine();
    ImGui::BeginDisabled(!v.var || !g_cvarValue[0]);
    if (ImGui::Button("Set once")) {
        std::wstring val(g_cvarValue, g_cvarValue + strlen(g_cvarValue));
        g_pendingConsole.push_back({name, val});  // set by the game thread at its next frame
        g_cvarResult = "Sent (see the log; lasts until an option changes it or the game restarts)";
    }
    ImGui::EndDisabled();
    Help("For experiments: sets the value at the game's next frame, at console priority.\n"
         "It isn't saved.");
    if (!g_cvarResult.empty()) ImGui::TextDisabled("%s", g_cvarResult.c_str());
}

void DrawOverlay(effect_runtime*) {
    std::lock_guard<std::recursive_mutex> lock(g_state);  // the game thread skips a frame meanwhile
    const View view = CopyView();
    ImGui::TextDisabled(DT_PRODUCT " " DT_VERSION);
    ImGui::SameLine();
    const Live live = g_live.load();
    if (live == Live::Failed)
        ImGui::TextColored(kBad, "  Can't reach the game's settings (Advanced tab)");
    else if (live != Live::Ready)
        ImGui::TextDisabled("  Connecting to the game's settings...");
    else if (!g_enabled)
        ImGui::TextColored(kWarn, "  Tweaks off (F8)");
    else
        ImGui::TextColored(kGood, "  Tweaks on (F8 compares)");
    ImGui::TextDisabled("* = changed from the game's value. Hover (?) for details. Changes apply at once.");

    bool changed = false;
    const bool measuring = g_measure.running;

    ImGui::Spacing();
    ImGui::BeginDisabled(measuring || live != Live::Ready);
    const int current = CurrentPreset();
    ChangedMark(current != 0);
    ImGui::SetNextItemWidth(ControlWidth());
    if (ImGui::BeginCombo("Quality preset", current >= 0 ? kPresets[current].label : "Custom")) {
        for (int i = 0; i < kPresetCount; ++i) {
            std::string item = kPresets[i].label;
            if (*kPresets[i].cost) item += std::string(" (") + kPresets[i].cost + ")";
            if (ImGui::Selectable(item.c_str(), i == current) && i != current) {
                ApplyPreset(i);
                changed = true;
                Log("preset: %s", kPresets[i].label);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kPresets[i].tip);
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    Help("Sets the quality options together. Measure preset costs checks them on your PC.\n"
         "Changing an option afterwards shows Custom.");
    ImGui::SameLine();
    float ms = Mean(g_frameMs);
    if (ms > 0) ImGui::TextDisabled("   %.1f ms (%.0f fps)", ms, 1000.0f / ms);

    if (measuring) {
        ImGui::TextColored(kAccent, "Measuring %s (%zu of %zu)... keep the camera still.",
                           g_measure.stages[g_measure.stage].label.c_str(), g_measure.stage + 1, g_measure.stages.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop")) FinishMeasure("stopped");
    } else {
        ImGui::BeginDisabled(live != Live::Ready || !g_enabled);
        if (ImGui::SmallButton("Measure preset costs")) StartMeasure(false);
        ImGui::EndDisabled();
        Help("Runs each preset for 14 s (6 s to settle, 8 s measured) and shows how much longer\n"
             "each frame takes than with Game. Stand still in a typical scene and leave the mouse\n"
             "alone. Your options come back afterwards.");
        ImGui::SameLine();
        ImGui::BeginDisabled(live != Live::Ready || !g_enabled);
        if (ImGui::SmallButton("Measure each option")) StartMeasure(true);
        ImGui::EndDisabled();
        Help("Runs every quality option on its own at each of its choices (11 s each, several\n"
             "minutes in all; Post-processing and HDR aren't measured)\n"
             "and writes how much each one costs to " DT_PRODUCT ".log, costliest first.");
        if (g_measure.done) {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
            ImGui::TextDisabled("%s", g_measure.summary.c_str());
            ImGui::PopTextWrapPos();
        } else if (!g_measure.note.empty()) {
            ImGui::TextColored(kWarn, "Measurement %s.", g_measure.note.c_str());
        }
    }
    ImGui::Spacing();

    ImGui::BeginDisabled(measuring);
    if (ImGui::BeginTabBar("tabs")) {
        std::vector<const char*> tabs;  // in the order the options list them
        for (const Option& o : g_options)
            if (std::none_of(tabs.begin(), tabs.end(), [&](const char* t) { return !strcmp(t, o.tab); }))
                tabs.push_back(o.tab);
        for (const char* tab : tabs)
            if (ImGui::BeginTabItem(tab)) {
                changed |= DrawTab(view, tab);
                ImGui::EndTabItem();
            }
        if (ImGui::BeginTabItem("Advanced")) {
            DrawAdvanced(view);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndDisabled();
    if (changed) {
        SaveSettings();
        g_dirty = true;
    }
}

// ---------------------------------------------------------------- own window
//
// ReShade docks every window made with register_overlay into its main panel, and the add-on
// can't undock it from inside (ReShade has already called Begin). So the add-on draws its own
// window from the reshade_overlay event while ReShade's overlay is open: it starts floating, and
// ImGui remembers where the player puts it. Closing it (x) is undone from the add-on's entry in
// ReShade's Add-ons tab. The window id is new, so the old docked layout doesn't apply to it.

std::atomic<bool> g_overlayOpen{false};
bool g_showWindow = true;  // ReShade.ini [LiSRemasteredTweaks] ShowWindow

bool OnOpenOverlay(effect_runtime*, bool open, input_source) {
    g_overlayOpen = open;
    return false;
}

void DrawWindow(effect_runtime* runtime) {
    if (!g_overlayOpen || !g_showWindow) return;
    const float fs = ImGui::GetFontSize();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(fs * 40, fs * 44), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(display.x - fs * 42, fs * 2), ImGuiCond_FirstUseEver);
    bool open = true;
    if (ImGui::Begin(DT_PRODUCT "###" DT_PRODUCT "Window", &open)) DrawOverlay(runtime);
    ImGui::End();
    if (!open) {
        g_showWindow = false;
        reshade::set_config_value(nullptr, DT_PRODUCT, "ShowWindow", g_showWindow);
    }
}

// The add-on's entry in ReShade's Add-ons tab.
void DrawSettings(effect_runtime*) {
    if (ImGui::Checkbox("Show the " DT_PRODUCT " window", &g_showWindow))
        reshade::set_config_value(nullptr, DT_PRODUCT, "ShowWindow", g_showWindow);
    Help("The add-on's own window, shown while the ReShade overlay is open. Drag it by its\n"
         "title bar; it keeps its place and size.");
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            if (!reshade::register_addon(module)) return FALSE;
            g_loadTick = GetTickCount64();
            {
                LARGE_INTEGER f;
                QueryPerformanceFrequency(&f);
                g_qpcFreq = f.QuadPart;
            }
            OpenLog();
            LoadSettings();
            reshade::get_config_value(nullptr, DT_PRODUCT, "ShowWindow", g_showWindow);
            reshade::get_config_value(nullptr, DT_PRODUCT, "HdrPeak", g_hdrPeak);
            reshade::get_config_value(nullptr, DT_PRODUCT, "HdrPaperWhite", g_hdrPaper);
            g_hdrPeak = std::clamp(g_hdrPeak, kPeakMin, kPeakMax);
            g_hdrPaper = std::clamp(g_hdrPaper, kPaperMin, kPaperMax);
            reshade::register_event<reshade::addon_event::present>(&OnPresent);
            reshade::register_event<reshade::addon_event::init_effect_runtime>(&OnInitRuntime);
            reshade::register_event<reshade::addon_event::destroy_effect_runtime>(&OnDestroyRuntime);
            reshade::register_event<reshade::addon_event::destroy_swapchain>(&OnDestroySwapchain);
            reshade::register_event<reshade::addon_event::destroy_device>(&OnDestroyDevice);
            reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(&OnBindRenderTargets);
            reshade::register_event<reshade::addon_event::bind_pipeline>(&OnBindPipeline);
            reshade::register_event<reshade::addon_event::draw>(&OnDraw);
            reshade::register_event<reshade::addon_event::draw_indexed>(&OnDrawIndexed);
            reshade::register_event<reshade::addon_event::clear_render_target_view>(&OnClearRtv);
            reshade::register_event<reshade::addon_event::copy_resource>(&OnCopyResource);
            reshade::register_event<reshade::addon_event::copy_texture_region>(&OnCopyRegion);
            reshade::register_event<reshade::addon_event::resolve_texture_region>(&OnResolve);
            reshade::register_event<reshade::addon_event::reshade_open_overlay>(&OnOpenOverlay);
            reshade::register_event<reshade::addon_event::reshade_overlay>(&DrawWindow);
            reshade::register_overlay(nullptr, &DrawSettings);
            break;
        case DLL_PROCESS_DETACH:
            if (g_hooked) cvars::UnhookSinks(g_manager);
            reshade::unregister_addon(module);
            if (g_log) fclose(g_log);
            break;
    }
    return TRUE;
}
