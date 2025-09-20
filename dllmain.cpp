

#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <tchar.h>

#include <string>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <vector>

#include "MinHook.h"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

#include "SDK.hpp"

// -------------------- Typedefs / Forward decls --------------------
typedef HRESULT(__stdcall* Present)(IDXGISwapChain*, UINT, UINT);

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static HRESULT __stdcall hkPresent(IDXGISwapChain*, UINT, UINT);
static void InitializeImGui(IDXGISwapChain*);
static void CreateOrRecreateRTV(IDXGISwapChain*);
static void DrawESP();

// -------------------- Globals --------------------
static Present                    g_originalPresent = nullptr;
static WNDPROC                    g_originalWndProc = nullptr;
static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static HWND                       g_hWnd = nullptr;
static bool                       g_imguiInitialized = false;
static bool                       g_showMenu = false;
static bool                       g_enableESP = false;
static bool                       g_skeletonESP = true;
static bool                       g_healthBars = true;
static bool                       g_wardESP = true;
static bool                       g_needRTVRecreate = false;

// -------------------- Debug console --------------------
// We expose a simple console window for printing debug information.  The user
// requested a "cmd debug window" similar to those used in UE hacking.  We
// allocate a console and redirect stdout/stderr to it.  In a production cheat
// you would hook UObject::ProcessEvent to log function calls, but here we
// provide a basic console for custom debug prints.  ProcessEvent in UE is
// called by the virtual machine to execute a UFunction with parameters【270877410327773†L80-L103】, hence
// logging from there would reveal the "brain" of the game, but hooking it is
// beyond this scope.
static bool g_consoleInitialized = false;

static void InitializeDebugConsole() {
    if (g_consoleInitialized) return;
    // Allocate a new console window for stdout/stderr
    if (AllocConsole()) {
        FILE* dummy;
        // Redirect stdout and stderr to the new console
        freopen_s(&dummy, "CONOUT$", "w", stdout);
        freopen_s(&dummy, "CONOUT$", "w", stderr);
        SetConsoleTitleA("Hemingway Trainer Debug");
        g_consoleInitialized = true;
        printf("[Debug] Console initialized\n");
    }
}


// Perf controls
static bool        g_perfMode = true;
static float       g_maxESPDistance = 25000.f; // cm
static int         g_maxActorsPerFrame = 128;
static int         g_skeletonEveryN = 2;       // draw skeletons every N frames
static size_t      g_actorStartIndex = 0;
static ULONGLONG   g_attCacheMs = 250;

// -------------------- Aim Assist (with prediction) --------------------
static bool   g_aimAssistEnabled = false;
static bool   g_aimHoldToAssist = true;          // hold key to assist
static int    g_aimKeyVirtual = VK_LBUTTON;    // LMB
static float  g_aimFovPixels = 180.0f;        // FOV circle radius
static float  g_aimSmoothing = 0.18f;         // 0.06 snappy … 0.40 soft
static float  g_aimIntensity = 1.0f;          // scales mouse pull strength
static bool   g_aimDrawFov = true;
static const char* g_aimBoneHint = "head";

// Prediction settings
static float  g_projectileSpeed = 30000.0f;      // cm/s (300 m/s)
static float  g_maxLeadSeconds = 0.25f;         // cap lead time

// -------------------- Utilities --------------------
template <typename T>
static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

static bool icontains(const std::string& hay, const char* needle) {
    std::string h = hay, n = needle;
    std::transform(h.begin(), h.end(), h.begin(), ::tolower);
    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
    return h.find(n) != std::string::npos;
}

// -------------------- DX11 RTV --------------------
static void CreateOrRecreateRTV(IDXGISwapChain* sc) {
    SafeRelease(g_rtv);
    ID3D11Texture2D* backBuffer = nullptr;
    if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer))) {
        if (g_device) {
            g_device->CreateRenderTargetView(backBuffer, nullptr, &g_rtv);
        }
        SafeRelease(backBuffer);
    }
}

// -------------------- Dummy window for swapchain bootstrap --------------------
struct DummyWindow {
    HWND hwnd = nullptr;
    WNDCLASSEXW wc{};
    DummyWindow() {
        HINSTANCE hInst = GetModuleHandleW(nullptr);
        wc = { sizeof(WNDCLASSEXW), CS_CLASSDC, DefWindowProcW, 0L, 0L, hInst,
               nullptr, nullptr, nullptr, nullptr, L"DummyDX11Wnd", nullptr };
        RegisterClassExW(&wc);
        hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
            0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    }
    ~DummyWindow() {
        if (hwnd) DestroyWindow(hwnd);
        if (wc.lpszClassName) UnregisterClassW(wc.lpszClassName, wc.hInstance);
    }
};

// -------------------- Bootstrap thread: install Present hook --------------------
static DWORD WINAPI BootstrapThread(LPVOID) {
    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED)
        return 0;

    // Initialize debug console early so we can log bootstrap diagnostics.  Note
    // that this console is optional and will only appear once per session.  If
    // hooking ProcessEvent is later desired, logging can be routed here.
    InitializeDebugConsole();

    DummyWindow dummy;

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = dummy.hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* pSwapChain = nullptr;
    ID3D11Device* pDevice = nullptr;
    ID3D11DeviceContext* pCtx = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &sd, &pSwapChain, &pDevice, nullptr, &pCtx);

    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION,
            &sd, &pSwapChain, &pDevice, nullptr, &pCtx);
    }
    if (FAILED(hr) || !pSwapChain) {
        MH_Uninitialize();
        return 0;
    }

    void** vTable = *reinterpret_cast<void***>(pSwapChain);
    void* presentAddr = vTable[8];

    SafeRelease(pCtx);
    SafeRelease(pDevice);
    SafeRelease(pSwapChain);

    if (!presentAddr) { MH_Uninitialize(); return 0; }

    if (MH_CreateHook(presentAddr, &hkPresent, reinterpret_cast<void**>(&g_originalPresent)) != MH_OK) {
        MH_Uninitialize();
        return 0;
    }
    MH_EnableHook(presentAddr);

    printf("[Debug] Present hook installed at %p\n", presentAddr);
    return 0;
}

// -------------------- ImGui init --------------------
static void InitializeImGui(IDXGISwapChain* pSwapChain)
{
    if (g_imguiInitialized) return;

    if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&g_device))) {
        g_device->GetImmediateContext(&g_context);
    }

    DXGI_SWAP_CHAIN_DESC sd{};
    pSwapChain->GetDesc(&sd);
    g_hWnd = sd.OutputWindow;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui_ImplWin32_Init(g_hWnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    g_originalWndProc = (WNDPROC)SetWindowLongPtr(g_hWnd, GWLP_WNDPROC, (LONG_PTR)WndProc);

    CreateOrRecreateRTV(pSwapChain);

    g_imguiInitialized = true;
}

// -------------------- Bone / attitude caches & helpers --------------------
struct AttCacheEntry { SDK::ETeamAttitude att; ULONGLONG ts; };
static std::unordered_map<SDK::APawn*, AttCacheEntry> g_attCache;

struct BoneCache {
    bool ready = false;
    SDK::FName head, neck, spine1, spine2, spine3, pelvis;
    SDK::FName lSh, lUp, lLo, lHand, rSh, rUp, rLo, rHand;
    SDK::FName lTh, lCalf, lFoot, rTh, rCalf, rFoot;
};
static std::unordered_map<SDK::USkeletalMeshComponent*, BoneCache> g_boneCache;

static inline float DistSq3D(const SDK::FVector& a, const SDK::FVector& b) {
    float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z; return dx * dx + dy * dy + dz * dz;
}

// Find bone by substring once (used to populate BoneCache)
static bool FindBoneByHint(SDK::USkeletalMeshComponent* mesh, const char* hint, SDK::FName* outName) {
    if (!mesh || !outName) return false;
    const int32_t num = mesh->GetNumBones();
    for (int32_t i = 0; i < num; ++i) {
        SDK::FName bn = mesh->GetBoneName(i);
        std::string s = bn.ToString();
        if (icontains(s, hint)) { *outName = bn; return true; }
    }
    return false;
}

static BoneCache& GetOrFillBoneCache(SDK::USkeletalMeshComponent* mesh) {
    BoneCache& bc = g_boneCache[mesh];
    if (bc.ready || !mesh) return bc;

    auto tryHint = [&](const char* hint, SDK::FName& out) {
        SDK::FName tmp; if (FindBoneByHint(mesh, hint, &tmp)) { out = tmp; return true; } return false;
        };

    tryHint("head", bc.head);     tryHint("neck", bc.neck);
    tryHint("spine_01", bc.spine1); tryHint("spine_02", bc.spine2); tryHint("spine_03", bc.spine3);
    tryHint("pelvis", bc.pelvis);

    tryHint("clavicle_l", bc.lSh); tryHint("upperarm_l", bc.lUp); tryHint("lowerarm_l", bc.lLo); tryHint("hand_l", bc.lHand);
    tryHint("clavicle_r", bc.rSh); tryHint("upperarm_r", bc.rUp); tryHint("lowerarm_r", bc.rLo); tryHint("hand_r", bc.rHand);

    tryHint("thigh_l", bc.lTh); tryHint("calf_l", bc.lCalf); tryHint("foot_l", bc.lFoot);
    tryHint("thigh_r", bc.rTh); tryHint("calf_r", bc.rCalf); tryHint("foot_r", bc.rFoot);

    bc.ready = true;
    return bc;
}

static bool GetBonePos(SDK::USkeletalMeshComponent* mesh, const SDK::FName& bone, SDK::FVector* out) {
    if (!mesh || !out) return false;
    if (bone.ComparisonIndex == 0 && bone.Number == 0) return false; // empty FName
    *out = mesh->GetSocketLocation(bone);
    return true;
}

// Cached team attitude (throttle ProcessEvent calls)
static SDK::ETeamAttitude GetTeamCached(SDK::APawn* me, SDK::APawn* other)
{
    using namespace SDK;
    if (!me || !other) return ETeamAttitude::Neutral;
    auto now = GetTickCount64();
    auto it = g_attCache.find(other);
    if (it != g_attCache.end() && (now - it->second.ts) < g_attCacheMs) {
        return it->second.att;
    }
    ETeamAttitude att = UHWLibrary_Core::GetTeamAttitudeTowards(me, other);
    g_attCache[other] = { att, now };
    return att;
}

// Fast bbox via 3 projections instead of 8
static bool ComputeBoxFast(SDK::AActor* actor, SDK::APlayerController* pc,
    float& minX, float& minY, float& maxX, float& maxY)
{
    using namespace SDK;
    FVector origin, extent;
    actor->GetActorBounds(true, &origin, &extent, false);

    FVector top = origin + FVector(0, 0, extent.Z);
    FVector bottom = origin - FVector(0, 0, extent.Z);

    FVector2D sTop{}, sBottom{}, sCenter{};
    if (!pc->ProjectWorldLocationToScreen(top, &sTop, true)) return false;
    if (!pc->ProjectWorldLocationToScreen(bottom, &sBottom, true)) return false;
    if (!pc->ProjectWorldLocationToScreen(origin, &sCenter, true)) return false;

    float h = std::fabs((float)sTop.Y - (float)sBottom.Y);
    float w = h * 0.45f;

    minX = (float)sCenter.X - w * 0.5f;
    maxX = (float)sCenter.X + w * 0.5f;
    minY = (float)sTop.Y;
    maxY = (float)sBottom.Y;
    return (maxY > minY);
}

// -------------------- Aim Assist helpers --------------------
static inline ImVec2 GetDisplayCenter() {
    ImVec2 ds = ImGui::GetIO().DisplaySize; return ImVec2(ds.x * 0.5f, ds.y * 0.5f);
}
static inline float Dist2D(float x1, float y1, float x2, float y2) {
    float dx = x2 - x1, dy = y2 - y1; return sqrtf(dx * dx + dy * dy);
}
static void MouseMoveRelative(int dx, int dy) {
    INPUT in{}; in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_MOVE; in.mi.dx = dx; in.mi.dy = dy;
    SendInput(1, &in, sizeof(INPUT));
}

// simple velocity cache per pawn
struct VelState { SDK::FVector prevPos{}; ULONGLONG prevTsMs = 0; SDK::FVector vel{}; };
static std::unordered_map<SDK::APawn*, VelState> g_velCache;

static SDK::FVector UpdateAndGetVel(SDK::APawn* pawn) {
    using namespace SDK;
    if (!pawn) return FVector{ 0,0,0 };
    VelState& st = g_velCache[pawn];
    ULONGLONG now = GetTickCount64();
    FVector cur = pawn->K2_GetActorLocation();
    if (st.prevTsMs != 0) {
        double dt = (now - st.prevTsMs) / 1000.0;
        if (dt > 0.0 && dt < 1.0) {
            st.vel = FVector{
                (float)((cur.X - st.prevPos.X) / dt),
                (float)((cur.Y - st.prevPos.Y) / dt),
                (float)((cur.Z - st.prevPos.Z) / dt)
            };
        }
    }
    st.prevPos = cur; st.prevTsMs = now;
    return st.vel;
}

static bool FindBestTargetPredicted(
    SDK::APlayerController* pc,
    SDK::APawn* localPawn,
    SDK::ULevel* level,
    const char* boneHint,
    float fovPx,
    SDK::FVector* outPredWorld,
    SDK::APawn** outPawn)
{
    using namespace SDK;
    if (!pc || !level) return false;

    ImVec2 center = GetDisplayCenter();
    APawn* bestPawn = nullptr;
    FVector bestPred{};
    float bestDist = FLT_MAX;

    for (int i = 0; i < level->Actors.Num(); ++i) {
        AActor* a = level->Actors[i];
        if (!a || !a->IsA(APawn::StaticClass())) continue;

        APawn* pawn = static_cast<APawn*>(a);
        if (!pawn || (localPawn && pawn == localPawn)) continue;

        // Skip pawns that are not hostile.  We previously attempted to treat bots as hostile,
        // but this detection was unreliable and has been removed.  Only enemies will be
        // considered for aim assistance.
        {
            SDK::ETeamAttitude att = UHWLibrary_Core::GetTeamAttitudeTowards(localPawn, pawn);
            if (att != SDK::ETeamAttitude::Hostile) continue;
        }

        FVector aim = a->K2_GetActorLocation();
        if (a->IsA(ACharacter::StaticClass())) {
            ACharacter* ch = static_cast<ACharacter*>(a);
            if (ch && ch->Mesh) {
                SDK::FName bn;
                if (FindBoneByHint(ch->Mesh, boneHint, &bn)) {
                    aim = ch->Mesh->GetSocketLocation(bn);
                }
            }
        }

        FVector vel = UpdateAndGetVel(pawn); // cm/s
        FVector me = localPawn ? localPawn->K2_GetActorLocation() : FVector{ 0,0,0 };
        float dist = sqrtf((aim.X - me.X) * (aim.X - me.X) + (aim.Y - me.Y) * (aim.Y - me.Y) + (aim.Z - me.Z) * (aim.Z - me.Z));
        float t = (g_projectileSpeed > 1.0f) ? (dist / g_projectileSpeed) : 0.0f;
        if (t > g_maxLeadSeconds) t = g_maxLeadSeconds;

        FVector pred = FVector{ aim.X + vel.X * t, aim.Y + vel.Y * t, aim.Z + vel.Z * t };

        SDK::FVector2D sp{};
        if (!pc->ProjectWorldLocationToScreen(pred, &sp, true)) continue;

        float d = Dist2D(center.x, center.y, (float)sp.X, (float)sp.Y);
        if (d > fovPx) continue;

        if (d < bestDist) { bestDist = d; bestPawn = pawn; bestPred = pred; }
    }

    if (!bestPawn) return false;
    if (outPawn) *outPawn = bestPawn;
    if (outPredWorld) *outPredWorld = bestPred;
    return true;
}

static void AimAssistTick(SDK::APlayerController* pc, SDK::ULevel* level)
{
    using namespace SDK;
    if (!pc || !level) return;
    if (!g_aimAssistEnabled) return;
    if (g_aimHoldToAssist && !(GetAsyncKeyState(g_aimKeyVirtual) & 0x8000)) return;

    APawn* me = pc->K2_GetPawn();
    if (!me) return;

    FVector pred{};
    APawn* target = nullptr;
    if (!FindBestTargetPredicted(pc, me, level, g_aimBoneHint, g_aimFovPixels, &pred, &target)) return;

    FVector2D scr{};
    if (!pc->ProjectWorldLocationToScreen(pred, &scr, true)) return;

    ImVec2 center = GetDisplayCenter();
    float dx = ((float)scr.X - center.x) * g_aimSmoothing * g_aimIntensity;
    float dy = ((float)scr.Y - center.y) * g_aimSmoothing * g_aimIntensity;

    const float dead = 0.5f;
    if (fabsf(dx) < dead && fabsf(dy) < dead) return;

    MouseMoveRelative((int)lroundf(dx), (int)lroundf(dy));
}

// -------------------- ESP main --------------------
static void DrawESP()
{
    using namespace SDK;

    ImDrawList* drawList = ImGui::GetBackgroundDrawList();

    UWorld* world = UWorld::GetWorld();
    if (!world) return;

    UGameInstance* gi = world->OwningGameInstance;
    if (!gi) return;

    auto& localPlayers = gi->LocalPlayers;
    if (!localPlayers.IsValid() || localPlayers.Num() <= 0) return;

    ULocalPlayer* lp = localPlayers[0];
    if (!lp) return;

    APlayerController* pc = lp->PlayerController;
    if (!pc) return;

    ULevel* level = world->PersistentLevel;
    if (!level) return;

    APawn* localPawn = pc->K2_GetPawn();
    SDK::FVector myLoc = localPawn ? localPawn->K2_GetActorLocation() : SDK::FVector{ 0,0,0 };

    const float maxDistSq = g_perfMode ? (g_maxESPDistance * g_maxESPDistance) : FLT_MAX;

    // Helper to generate a readable label for a pawn.  Hemingway's character classes are
    // often named like "BP_GOD_Ymir_C" or include other blueprint prefixes/suffixes.
    // Additionally, bots may not have a PlayerState from which to read a username.  This
    // lambda attempts to derive a short display name by first trying to use the
    // PlayerState's PlayerName (which for AI often contains the god name), and if that
    // fails it falls back to the UObject name and strips common blueprint prefixes and
    // suffixes.  The resulting string is upper‑cased for readability.
    auto getDisplayName = [&](APawn* p) -> std::string {
        std::string result;
        if (!p) return result;
        // Try PlayerState->GetPlayerName() if available.
        AController* ctrl = p->Controller;
        if (ctrl && ctrl->PlayerState) {
            SDK::FString f = ctrl->PlayerState->GetPlayerName();
            result = f.ToString();
        }
        // If the result is empty or looks like an empty stub, fall back to UObject::GetName().
        if (result.empty()) {
            result = p->GetName();
        }
        // Strip suffix after the last underscore if it contains digits or ends with "_C".
        auto stripSuffix = [](const std::string& in) -> std::string {
            // Find last underscore
            size_t last = in.find_last_of('_');
            if (last == std::string::npos)
                return in;
            // If substring after last underscore contains a digit or exactly "C", strip it
            bool numeric = false;
            bool endsWithC = false;
            for (size_t i = last + 1; i < in.size(); ++i) {
                char c = in[i];
                if (c >= '0' && c <= '9') numeric = true;
                if (c == 'C' && i == in.size() - 1) endsWithC = true;
            }
            if (numeric || endsWithC) {
                return in.substr(0, last);
            }
            return in;
            };
        // Remove common blueprint prefixes
        auto stripPrefix = [](const std::string& in) -> std::string {
            const char* prefixes[] = { "BP_GOD_", "BP_", "ABP_", "WBP_", "SK_", "ABP_GOD_" };
            for (const char* pre : prefixes) {
                size_t len = strlen(pre);
                if (in.size() > len && _strnicmp(in.c_str(), pre, len) == 0) {
                    return in.substr(len);
                }
            }
            return in;
            };
        result = stripSuffix(result);
        result = stripPrefix(result);
        // Trim trailing numbers or underscores
        while (!result.empty() && (result.back() == '_' || (result.back() >= '0' && result.back() <= '9'))) {
            result.pop_back();
        }
        // Convert to uppercase for readability
        std::transform(result.begin(), result.end(), result.begin(), ::toupper);
        return result;
        };

    auto colorForAtt = [](ETeamAttitude att)->ImU32 {
        switch (att) {
        case ETeamAttitude::Hostile:  return IM_COL32(255, 0, 0, 255);
        case ETeamAttitude::Friendly: return IM_COL32(0, 255, 0, 255);
        default:                      return IM_COL32(0, 255, 0, 255);
        }
        };

    const int total = level->Actors.Num();
    if (total <= 0) return;

    int processed = 0;
    int start = (int)(g_actorStartIndex % (size_t)total);

    int boxesDrawn = 0, wardsDrawn = 0;

    for (int n = 0; n < total; ++n) {
        if (g_perfMode && processed >= g_maxActorsPerFrame) break;
        int i = start + n; if (i >= total) i -= total;

        AActor* actor = level->Actors[i];
        if (!actor) continue;

        // Ward ESP
        if (g_wardESP && actor->IsA(AHWDeployable_Ward::StaticClass())) {
            SDK::FVector loc = actor->K2_GetActorLocation();
            SDK::FVector2D s{};
            if (pc->ProjectWorldLocationToScreen(loc, &s, true)) {
                ImVec2 p((float)s.X, (float)s.Y);
                drawList->AddCircle(p, 10.0f, IM_COL32(255, 0, 255, 255), 24, 2.0f);
                ++wardsDrawn;
            }
            ++processed; continue;
        }

        // Pawn ESP
        if (!g_enableESP) { ++processed; continue; }
        if (!actor->IsA(APawn::StaticClass())) { ++processed; continue; }
        if (localPawn && actor == localPawn) { ++processed; continue; }

        if (g_perfMode) {
            SDK::FVector aLoc = actor->K2_GetActorLocation();
            if (DistSq3D(aLoc, myLoc) > maxDistSq) { ++processed; continue; }
        }

        APawn* pawn = static_cast<APawn*>(actor);
        if (!pawn) { ++processed; continue; }

        // (Optionally re-enable if you only want players)
        // if (!pawn->IsPlayerControlled()) { ++processed; continue; }

        // Determine team attitude.  We previously attempted to treat AI‑controlled gods as
        // hostile by checking PlayerState->IsABot(), but this proved unreliable.  We now
        // simply query the cached team attitude and trust the underlying game logic.  Hostile
        // pawns will be coloured red and friendlies green.
        SDK::ETeamAttitude att = GetTeamCached(localPawn, pawn);

        ImU32 col = colorForAtt(att);

        float minX, minY, maxX, maxY;
        if (!ComputeBoxFast(actor, pc, minX, minY, maxX, maxY)) { ++processed; continue; }

        // Draw a glow over the character's silhouette by constructing a convex hull of
        // projected bone points.  This approximates the god's shape instead of using
        // a simple rectangular box.  We gather relevant bone screen coordinates, sort
        // them by angle around their centroid, and draw a filled polygon with a
        // semi‑transparent team colour.  Note: if fewer than 3 points are available
        // the hull is skipped.
        if (actor->IsA(ACharacter::StaticClass())) {
            ACharacter* ch = static_cast<ACharacter*>(actor);
            if (ch && ch->Mesh) {
                BoneCache& bc = GetOrFillBoneCache(ch->Mesh);
                std::vector<ImVec2> pts;
                pts.reserve(32);
                auto addBone = [&](const SDK::FName& bn) {
                    SDK::FVector pos;
                    if (GetBonePos(ch->Mesh, bn, &pos)) {
                        SDK::FVector2D s{};
                        if (pc->ProjectWorldLocationToScreen(pos, &s, true)) {
                            pts.emplace_back((float)s.X, (float)s.Y);
                        }
                    }
                    };
                // Collect major bones for hull
                addBone(bc.head);
                addBone(bc.neck);
                addBone(bc.pelvis);
                addBone(bc.spine1);
                addBone(bc.spine2);
                addBone(bc.spine3);
                addBone(bc.lSh); addBone(bc.lUp); addBone(bc.lLo); addBone(bc.lHand);
                addBone(bc.rSh); addBone(bc.rUp); addBone(bc.rLo); addBone(bc.rHand);
                addBone(bc.lTh); addBone(bc.lCalf); addBone(bc.lFoot);
                addBone(bc.rTh); addBone(bc.rCalf); addBone(bc.rFoot);
                if (pts.size() >= 3) {
                    // Compute centroid
                    ImVec2 center(0.0f, 0.0f);
                    for (const auto& p : pts) { center.x += p.x; center.y += p.y; }
                    center.x /= (float)pts.size(); center.y /= (float)pts.size();
                    // Sort by polar angle around centroid
                    std::sort(pts.begin(), pts.end(), [&](const ImVec2& a, const ImVec2& b) {
                        float angA = atan2f(a.y - center.y, a.x - center.x);
                        float angB = atan2f(b.y - center.y, b.x - center.x);
                        return angA < angB;
                        });
                    // Build semi‑transparent fill colour based on team colour (RGB from col)
                    unsigned char r = (unsigned char)(col & 0xFF);
                    unsigned char g = (unsigned char)((col >> 8) & 0xFF);
                    unsigned char bCol = (unsigned char)((col >> 16) & 0xFF);
                    ImU32 fillColor = IM_COL32(r, g, bCol, 80);
                    drawList->AddConvexPolyFilled(pts.data(), (int)pts.size(), fillColor);
                }
            }
        }

        // Health bar
        if (g_healthBars && actor->IsA(AHWCharacter_Base::StaticClass())) {
            AHWCharacter_Base* ch = static_cast<AHWCharacter_Base*>(actor);
            float hp = ch->GetHealth();
            float mh = (std::max)(1.0f, ch->GetMaxHealth()); // safe vs. NOMINMAX
            float pct = std::clamp(hp / mh, 0.0f, 1.0f);

            const float barW = 3.0f;
            const float barPad = 2.0f;
            float h = (maxY - minY);
            ImU32 barCol = (col == IM_COL32(255, 0, 0, 255)) ? IM_COL32(255, 0, 0, 220) : IM_COL32(0, 255, 0, 220);

            drawList->AddRectFilled(ImVec2(minX - barPad - barW, minY),
                ImVec2(minX - barPad, maxY),
                IM_COL32(0, 0, 0, 150));
            drawList->AddRectFilled(ImVec2(minX - barPad - barW, maxY - h * pct),
                ImVec2(minX - barPad, maxY),
                barCol);
        }

        // Draw character name and distance with a glow effect.  We compute a simplified
        // display name for the pawn, derive the distance to the local player, and
        // overlay both strings near the top of the bounding box.  A simple glow
        // is achieved by drawing the text multiple times with a black colour offset
        // around the main coloured text.
        {
            // Obtain a short display name for this pawn (upper‑case, no prefixes)
            std::string name = getDisplayName(pawn);
            // Compute distance in metres.  DistSq3D returns squared centimetres.
            float distSq = DistSq3D(actor->K2_GetActorLocation(), myLoc);
            float distM = 0.0f;
            if (distSq > 0.0f) {
                distM = sqrtf(distSq) / 100.0f;
            }
            char distBuf[16];
            _snprintf_s(distBuf, sizeof(distBuf), "%.1fm", distM);
            std::string distStr(distBuf);

            // Starting position for the label: pad a few pixels inside the top left of the box
            const float pad = 3.0f;
            ImVec2 posName(minX + pad, minY + pad);

            // Helper lambda to draw a string with a simple glow (outline) effect
            auto drawTextGlow = [&](const ImVec2& pos, const std::string& txt, ImU32 colour) {
                if (txt.empty()) return;
                ImU32 glow = IM_COL32(0, 0, 0, 200);
                // Offsets for an 8‑way shadow/outside pass
                const int offs[8][2] = { { -1, -1 },{ 0, -1 },{ 1, -1 },{ -1, 0 },{ 1, 0 },{ -1, 1 },{ 0, 1 },{ 1, 1 } };
                for (int ii = 0; ii < 8; ++ii) {
                    drawList->AddText(ImVec2(pos.x + (float)offs[ii][0], pos.y + (float)offs[ii][1]), glow, txt.c_str());
                }
                drawList->AddText(pos, colour, txt.c_str());
                };
            // Draw name in bright yellow
            drawTextGlow(posName, name, IM_COL32(255, 255, 0, 255));
            // Draw distance below the name
            ImVec2 nameSize = ImGui::CalcTextSize(name.c_str());
            ImVec2 posDist(posName.x, posName.y + nameSize.y);
            drawTextGlow(posDist, distStr, IM_COL32(255, 255, 0, 255));
        }

        // Skeleton (cached bones, optional decimation)
        static unsigned frame = 0; frame++;
        if (g_skeletonESP && actor->IsA(ACharacter::StaticClass())) {
            if (!g_perfMode || (frame % (unsigned)g_skeletonEveryN == 0)) {
                ACharacter* ch = static_cast<ACharacter*>(actor);
                if (ch && ch->Mesh) {
                    BoneCache& bc = GetOrFillBoneCache(ch->Mesh);
                    auto proj = [&](const SDK::FVector& w, ImVec2* out)->bool {
                        SDK::FVector2D s{}; return pc->ProjectWorldLocationToScreen(w, &s, true)
                            ? ((*out = ImVec2((float)s.X, (float)s.Y)), true) : false;
                        };
                    auto line = [&](const SDK::FVector& a, const SDK::FVector& b) {
                        ImVec2 A, B; if (proj(a, &A) && proj(b, &B)) drawList->AddLine(A, B, col, 1.0f);
                        };
                    auto get = [&](const SDK::FName& fn, SDK::FVector* out)->bool {
                        return GetBonePos(ch->Mesh, fn, out);
                        };

                    SDK::FVector H, N, S1, S2, S3, P, LSH, LUP, LLO, LH, RSH, RUP, RLO, RH, LTH, LCF, LF, RTH, RCF, RF;

                    if (get(bc.pelvis, &P) && get(bc.spine1, &S1)) line(P, S1);
                    if (get(bc.spine1, &S1) && get(bc.spine2, &S2)) line(S1, S2);
                    if (get(bc.spine2, &S2) && get(bc.spine3, &S3)) line(S2, S3);
                    if (get(bc.spine3, &S3) && get(bc.neck, &N))    line(S3, N);
                    if (get(bc.neck, &N) && get(bc.head, &H))    line(N, H);

                    if (get(bc.neck, &N) && get(bc.lSh, &LSH)) line(N, LSH);
                    if (get(bc.lSh, &LSH) && get(bc.lUp, &LUP)) line(LSH, LUP);
                    if (get(bc.lUp, &LUP) && get(bc.lLo, &LLO)) line(LUP, LLO);
                    if (get(bc.lLo, &LLO) && get(bc.lHand, &LH)) line(LLO, LH);

                    if (get(bc.neck, &N) && get(bc.rSh, &RSH)) line(N, RSH);
                    if (get(bc.rSh, &RSH) && get(bc.rUp, &RUP)) line(RSH, RUP);
                    if (get(bc.rUp, &RUP) && get(bc.rLo, &RLO)) line(RUP, RLO);
                    if (get(bc.rLo, &RLO) && get(bc.rHand, &RH)) line(RLO, RH);

                    if (get(bc.pelvis, &P) && get(bc.lTh, &LTH)) line(P, LTH);
                    if (get(bc.lTh, &LTH) && get(bc.lCalf, &LCF)) line(LTH, LCF);
                    if (get(bc.lCalf, &LCF) && get(bc.lFoot, &LF)) line(LCF, LF);

                    if (get(bc.pelvis, &P) && get(bc.rTh, &RTH)) line(P, RTH);
                    if (get(bc.rTh, &RTH) && get(bc.rCalf, &RCF)) line(RTH, RCF);
                    if (get(bc.rCalf, &RCF) && get(bc.rFoot, &RF)) line(RCF, RF);
                }
            }
        }

        ++processed;
    }

    g_actorStartIndex += processed;

    // Optional tiny stats
    char buf[160];
    _snprintf_s(buf, sizeof(buf), "ESP perf: total=%d processed=%d boxes=%d wards=%d",
        total, processed, boxesDrawn, wardsDrawn);
    ImGui::GetBackgroundDrawList()->AddText(ImVec2(10, 30), IM_COL32(180, 255, 180, 200), buf);
}

// -------------------- WndProc --------------------
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_KEYUP && wParam == VK_F12) {
        g_showMenu = !g_showMenu;
    }
    if (msg == WM_SIZE && wParam != SIZE_MINIMIZED) {
        g_needRTVRecreate = true;
    }
    if (g_showMenu && ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
        return TRUE;
    }
    return CallWindowProc(g_originalWndProc, hWnd, msg, wParam, lParam);
}

// -------------------- hkPresent --------------------
static HRESULT __stdcall hkPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
    if (!g_imguiInitialized) {
        InitializeImGui(pSwapChain);
    }

    // Rebind WndProc if the window changed (e.g., on map load)
    DXGI_SWAP_CHAIN_DESC sd{};
    if (SUCCEEDED(pSwapChain->GetDesc(&sd))) {
        if (g_hWnd && g_hWnd != sd.OutputWindow) {
            if (g_originalWndProc) {
                SetWindowLongPtr(g_hWnd, GWLP_WNDPROC, (LONG_PTR)g_originalWndProc);
            }
            g_hWnd = sd.OutputWindow;
            g_originalWndProc = (WNDPROC)SetWindowLongPtr(g_hWnd, GWLP_WNDPROC, (LONG_PTR)WndProc);
            g_needRTVRecreate = true;
        }
    }

    if (g_needRTVRecreate || !g_rtv) {
        CreateOrRecreateRTV(pSwapChain);
        g_needRTVRecreate = false;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // Debug heartbeat
    {
        static int dbg = 0;
        char hb[64];
        _snprintf_s(hb, sizeof(hb), "ESP DEBUG: frame %d", ++dbg);
        ImGui::GetBackgroundDrawList()->AddText(ImVec2(12, 12), IM_COL32(255, 255, 0, 255), hb);
    }

    // Menu
    if (g_showMenu) {
        ImGui::Begin("Trainer Menu", &g_showMenu,
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);

        ImGui::SeparatorText("ESP");
        ImGui::Checkbox("Enable ESP (players)", &g_enableESP);
        ImGui::Checkbox("Skeleton", &g_skeletonESP);
        ImGui::Checkbox("Health bars", &g_healthBars);
        ImGui::Checkbox("Ward ESP", &g_wardESP);

        ImGui::SeparatorText("Performance");
        ImGui::Checkbox("Perf mode", &g_perfMode);
        ImGui::SliderFloat("Max ESP dist (cm)", &g_maxESPDistance, 5000.f, 60000.f, "%.0f");
        ImGui::SliderInt("Max actors/frame", &g_maxActorsPerFrame, 32, 512);
        ImGui::SliderInt("Skeleton every N frames", &g_skeletonEveryN, 1, 4);

        ImGui::SeparatorText("Aim Assist");
        ImGui::Checkbox("Enable aim assist", &g_aimAssistEnabled);
        ImGui::Checkbox("Hold key (RMB)", &g_aimHoldToAssist);
        ImGui::SliderFloat("FOV (px)", &g_aimFovPixels, 40.f, 600.f, "%.0f");
        ImGui::SliderFloat("Smoothing", &g_aimSmoothing, 0.05f, 0.40f, "%.2f");
        ImGui::SliderFloat("Intensity", &g_aimIntensity, 0.5f, 2.0f, "%.2f");

        ImGui::SeparatorText("Prediction");
        ImGui::SliderFloat("Projectile speed (cm/s)", &g_projectileSpeed, 8000.f, 120000.f, "%.0f");
        ImGui::SliderFloat("Max lead time (s)", &g_maxLeadSeconds, 0.05f, 0.40f, "%.2f");
        ImGui::Checkbox("Draw FOV circle", &g_aimDrawFov);

        ImGui::End();
    }

    // ESP
    if (g_enableESP) {
        DrawESP();
    }

    // Aim-assist tick
    {
        using namespace SDK;
        UWorld* w = UWorld::GetWorld();
        if (w) {
            UGameInstance* gi = w->OwningGameInstance;
            if (gi && gi->LocalPlayers.IsValid() && gi->LocalPlayers.Num() > 0) {
                ULocalPlayer* lp = gi->LocalPlayers[0];
                if (lp && lp->PlayerController && w->PersistentLevel) {
                    AimAssistTick(lp->PlayerController, w->PersistentLevel);
                }
            }
        }
    }

    // (optional) draw FOV circle
    if (g_aimAssistEnabled && g_aimDrawFov) {
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        ImVec2 c = GetDisplayCenter();
        dl->AddCircle(c, g_aimFovPixels, IM_COL32(255, 255, 0, 120), 64, 1.5f);
    }

    ImGui::Render();
    if (!g_rtv) CreateOrRecreateRTV(pSwapChain);
    if (g_context && g_rtv) {
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    }
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    return g_originalPresent(pSwapChain, SyncInterval, Flags);
}

// -------------------- Cleanup & DllMain --------------------
static void CleanupAll() {
    if (g_imguiInitialized) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        if (g_hWnd && g_originalWndProc) {
            SetWindowLongPtr(g_hWnd, GWLP_WNDPROC, (LONG_PTR)g_originalWndProc);
            g_originalWndProc = nullptr;
        }
    }
    SafeRelease(g_rtv);
    SafeRelease(g_context);
    SafeRelease(g_device);
    if (g_originalPresent) {
        MH_DisableHook(reinterpret_cast<LPVOID>(g_originalPresent));
    }
    else {
        MH_DisableHook(MH_ALL_HOOKS);
    }
    MH_Uninitialize();

    // Free debug console on shutdown
    if (g_consoleInitialized) {
        FreeConsole();
        g_consoleInitialized = false;
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        HANDLE h = CreateThread(nullptr, 0, BootstrapThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        CleanupAll();
    }
    return TRUE;
}
