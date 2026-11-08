// RR64 RT64 probe.
//
// Creates a Vulkan device through RT64's own setup path and presents test
// frames, on an Android phone, with NO ROM and NO game code.
//
// Init chain (mirrors the real game, RecompFrontend's RT64Context):
//   SDL_CreateWindow(SDL_WINDOW_VULKAN)
//   -> RT64::Application::Core (stub RDRAM + stub VI registers, SDL_Window*)
//   -> RT64::Application(core, appConfig{ useConfigurationFile=false,
//                                         detectDataPath=false })
//   -> app->setup(thread_id 0)
//   -> app->updateScreen()  (submits one present of the stub VI framebuffer)
//   -> presentQueue->waitForPresentId(state->presentId)
//   -> resize the SDL window, updateScreen() again (exercises the
//      swapchain-recreate path RT64's present queue owns)
//
// Before RT64 runs, a small raw-Vulkan pre-check (instance + physical
// device + the two feature structs RT64 needs) runs first, so that when
// setup() fails the report can say WHY: no Vulkan at all, or a device that
// lacks descriptor indexing / scalar block layout.
//
// Every result is reported BOTH on screen (ProbeActivity.appendProbeLine,
// large text — Ray debugs via screenshots) AND to logcat (tag RR64Probe).
// Pass = RT64 setup succeeded AND one frame presented. Anything else is a
// FAIL with the reported reason naming the missing feature.
//
// What this probe deliberately does NOT do: N64Recomp codegen, ROM loading,
// RecompFrontend, touch controls, Turnip driver import (noted as a future
// fallback in docs/ANDROID_PROBE.md), or any game code.
//
// NOT compile-checked: no NDK / Android Studio on this machine. See
// android/probe/README.md.
#include <android/log.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include <SDL.h>
#include <SDL_system.h>
#include <SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include "hle/rt64_application.h"

#define PROBE_LOG_TAG "RR64Probe"

// ---------------------------------------------------------------------------
// Reporting: logcat + on-screen (Java ProbeActivity.appendProbeLine).
// ---------------------------------------------------------------------------
static void report(const char *format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    __android_log_print(ANDROID_LOG_INFO, PROBE_LOG_TAG, "%s", line);
    fprintf(stderr, "[RR64Probe] %s\n", line);

    JNIEnv *env = SDL_AndroidGetJNIEnv();
    jobject activity = (env != nullptr) ? SDL_AndroidGetActivity() : nullptr;
    if (env == nullptr || activity == nullptr) {
        return;
    }
    jclass activityClass = env->GetObjectClass(activity);
    if (activityClass == nullptr) {
        env->DeleteLocalRef(activity);
        return;
    }
    jmethodID append = env->GetMethodID(activityClass, "appendProbeLine", "(Ljava/lang/String;)V");
    if (append == nullptr) {
        env->DeleteLocalRef(activityClass);
        env->DeleteLocalRef(activity);
        return;
    }
    jstring text = env->NewStringUTF(line);
    if (text != nullptr) {
        env->CallVoidMethod(activity, append, text);
        env->DeleteLocalRef(text);
    }
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(activity);
}

static const char *boolStr(bool value) {
    return value ? "YES" : "NO";
}

// ---------------------------------------------------------------------------
// Raw-Vulkan pre-check. Surface-free: no swapchain, just instance +
// physical device + feature query. Distinguishes "no Vulkan" from
// "device lacks the features RT64 needs".
// ---------------------------------------------------------------------------
static bool rawVulkanCheck() {
    if (SDL_Vulkan_LoadLibrary(nullptr) != 0) {
        report("RAW VULKAN: SDL_Vulkan_LoadLibrary FAILED: %s", SDL_GetError());
        report("RAW VULKAN: libvulkan.so could not be loaded at all.");
        return false;
    }
    report("RAW VULKAN: libvulkan.so loaded.");

    auto getProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    if (getProc == nullptr) {
        report("RAW VULKAN: SDL_Vulkan_GetVkGetInstanceProcAddr returned null.");
        return false;
    }
    auto vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(getProc(nullptr, "vkCreateInstance"));
    auto vkEnumeratePhysicalDevices =
        reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(getProc(nullptr, "vkEnumeratePhysicalDevices"));
    auto vkDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(getProc(nullptr, "vkDestroyInstance"));
    if (vkCreateInstance == nullptr || vkEnumeratePhysicalDevices == nullptr || vkDestroyInstance == nullptr) {
        report("RAW VULKAN: could not resolve core instance functions.");
        return false;
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "rr64probe";
    appInfo.applicationVersion = 1;
    appInfo.pEngineName = "rr64probe";
    appInfo.engineVersion = 1;
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&createInfo, nullptr, &instance);
    if (result == VK_ERROR_INCOMPATIBLE_DRIVER) {
        report("RAW VULKAN: Vulkan 1.1 instance creation refused (INCOMPATIBLE_DRIVER); retrying at 1.0.");
        appInfo.apiVersion = VK_API_VERSION_1_0;
        result = vkCreateInstance(&createInfo, nullptr, &instance);
    }
    if (result != VK_SUCCESS || instance == VK_NULL_HANDLE) {
        report("RAW VULKAN: vkCreateInstance FAILED: VkResult %d.", static_cast<int>(result));
        return false;
    }
    report("RAW VULKAN: instance created (requested API %s).",
           appInfo.apiVersion == VK_API_VERSION_1_1 ? "1.1" : "1.0");

    auto vkGetPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        getProc(instance, "vkGetPhysicalDeviceProperties"));
    auto vkGetPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        getProc(instance, "vkGetPhysicalDeviceFeatures2"));
    auto vkEnumerateDeviceExtensionProperties = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        getProc(instance, "vkEnumerateDeviceExtensionProperties"));

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        report("RAW VULKAN: vkEnumeratePhysicalDevices found ZERO devices.");
        vkDestroyInstance(instance, nullptr);
        return false;
    }
    report("RAW VULKAN: %u physical device(s).", deviceCount);
    VkPhysicalDevice device = VK_NULL_HANDLE;
    vkEnumeratePhysicalDevices(instance, &deviceCount, &device);
    if (device == VK_NULL_HANDLE) {
        report("RAW VULKAN: could not fetch the first physical device.");
        vkDestroyInstance(instance, nullptr);
        return false;
    }

    if (vkGetPhysicalDeviceProperties != nullptr) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(device, &props);
        report("RAW VULKAN: device: %s", props.deviceName);
        report("RAW VULKAN: device API: %u.%u.%u, driver version: 0x%X.",
               VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
               VK_VERSION_PATCH(props.apiVersion), props.driverVersion);
    }

    bool indexing = false;
    bool scalar = false;
    if (vkGetPhysicalDeviceFeatures2 != nullptr) {
        VkPhysicalDeviceScalarBlockLayoutFeatures scalarFeatures{};
        scalarFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES;
        VkPhysicalDeviceDescriptorIndexingFeatures indexingFeatures{};
        indexingFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
        indexingFeatures.pNext = &scalarFeatures;
        VkPhysicalDeviceFeatures2 features{};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features.pNext = &indexingFeatures;
        vkGetPhysicalDeviceFeatures2(device, &features);
        indexing = indexingFeatures.descriptorBindingPartiallyBound &&
                   indexingFeatures.descriptorBindingVariableDescriptorCount &&
                   indexingFeatures.runtimeDescriptorArray;
        scalar = scalarFeatures.scalarBlockLayout == VK_TRUE;
        report("RAW VULKAN: descriptorBindingPartiallyBound: %s",
               boolStr(indexingFeatures.descriptorBindingPartiallyBound == VK_TRUE));
        report("RAW VULKAN: descriptorBindingVariableDescriptorCount: %s",
               boolStr(indexingFeatures.descriptorBindingVariableDescriptorCount == VK_TRUE));
        report("RAW VULKAN: runtimeDescriptorArray: %s",
               boolStr(indexingFeatures.runtimeDescriptorArray == VK_TRUE));
        report("RAW VULKAN: descriptor indexing (all three): %s", boolStr(indexing));
        report("RAW VULKAN: scalarBlockLayout: %s", boolStr(scalar));
    } else {
        report("RAW VULKAN: vkGetPhysicalDeviceFeatures2 unavailable; cannot query features.");
    }

    if (vkEnumerateDeviceExtensionProperties != nullptr) {
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount, nullptr);
        bool hasIndexingExt = false;
        bool hasScalarExt = false;
        if (extCount > 0) {
            std::unique_ptr<VkExtensionProperties[]> exts(new VkExtensionProperties[extCount]);
            vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount, exts.get());
            for (uint32_t i = 0; i < extCount; i++) {
                if (std::strcmp(exts[i].extensionName, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME) == 0) {
                    hasIndexingExt = true;
                }
                if (std::strcmp(exts[i].extensionName, VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME) == 0) {
                    hasScalarExt = true;
                }
            }
        }
        report("RAW VULKAN: VK_EXT_descriptor_indexing present: %s", boolStr(hasIndexingExt));
        report("RAW VULKAN: VK_EXT_scalar_block_layout present: %s", boolStr(hasScalarExt));
    }

    // NOTE: the instance is deliberately destroyed before RT64 runs, so the
    // probe's throwaway instance cannot interfere with plume's own instance.
    vkDestroyInstance(instance, nullptr);
    report("RAW VULKAN: pre-check done. RT64 needs descriptor indexing AND scalar block layout.");
    return indexing && scalar;
}

// ---------------------------------------------------------------------------
// Stub N64 core: what RT64::Application::Core needs to exist. Mirrors
// RecompFrontend's RT64Context (dummy ROM header, static DMEM/IMEM,
// static VI registers, no-op interrupt check). No ROM is involved: RDRAM
// is a zeroed 4 MiB buffer, so the presented test frame is a black frame
// of a plausible 320x124 16-bit VI framebuffer.
// ---------------------------------------------------------------------------
static uint8_t g_header[0x40]{};
static uint8_t g_rdram[4 * 1024 * 1024]{};
static uint8_t g_dmem[0x1000]{};
static uint8_t g_imem[0x1000]{};

static uint32_t g_miIntr = 0;
static uint32_t g_dpcStart = 0;
static uint32_t g_dpcEnd = 0;
static uint32_t g_dpcCurrent = 0;
static uint32_t g_dpcStatus = 0;
static uint32_t g_dpcClock = 0;
static uint32_t g_dpcBufBusy = 0;
static uint32_t g_dpcPipeBusy = 0;
static uint32_t g_dpcTmem = 0;

// VI: type 2 (16-bit) so fbSiz() is 16-bit and visible() is true;
// hStart > 0 is what makes VI::visible() true. Width 320, origin inside
// RDRAM; x/y scale 0x400 = 1.0, giving a 320x~124 framebuffer.
static uint32_t g_viStatus = 2;
static uint32_t g_viOrigin = 0x1000;
static uint32_t g_viWidth = 320;
static uint32_t g_viIntr = 0;
static uint32_t g_viVCurrentLine = 0;
static uint32_t g_viTiming = 0;
static uint32_t g_viVSync = 0;
static uint32_t g_viHSync = 0;
static uint32_t g_viLeap = 0;
static uint32_t g_viHStart = (100u << 16) | 420u; // hStart=100, hEnd=420
static uint32_t g_viVStart = (10u << 16) | 250u;  // vStart=10, vEnd=250
static uint32_t g_viVBurst = 0;
static uint32_t g_viXScale = 0x400;
static uint32_t g_viYScale = 0x400;

static void dummyCheckInterrupts() {}

static const char *setupResultStr(RT64::Application::SetupResult result) {
    switch (result) {
    case RT64::Application::SetupResult::Success:
        return "Success";
    case RT64::Application::SetupResult::DynamicLibrariesNotFound:
        return "DynamicLibrariesNotFound";
    case RT64::Application::SetupResult::InvalidGraphicsAPI:
        return "InvalidGraphicsAPI";
    case RT64::Application::SetupResult::GraphicsAPINotFound:
        return "GraphicsAPINotFound";
    case RT64::Application::SetupResult::GraphicsDeviceNotFound:
        return "GraphicsDeviceNotFound";
    default:
        return "Unknown";
    }
}

static bool waitForExit() {
    SDL_Event event;
    while (true) {
        while (SDL_PollEvent(&event) != 0) {
            if (event.type == SDL_QUIT) {
                return true;
            }
        }
        SDL_Delay(100);
    }
}

extern "C" int SDL_main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    report("========================================");
    report("RR64 RT64 PROBE 0.1.0");
    report("NO ROM needed. NO game code.");
    report("========================================");

    SDL_version sdlVersion{};
    SDL_GetVersion(&sdlVersion);
    report("SDL version: %d.%d.%d.", sdlVersion.major, sdlVersion.minor, sdlVersion.patch);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        report("SDL_Init FAILED: %s", SDL_GetError());
        report("RESULT: FAIL - SDL video init failed.");
        waitForExit();
        return 1;
    }

    // Raw-Vulkan pre-check first: it names the missing feature when RT64
    // later fails, instead of leaving a bare GraphicsDeviceNotFound.
    const bool rawOk = rawVulkanCheck();
    if (!rawOk) {
        report("NOTE: raw pre-check did not pass; RT64 setup is still attempted below.");
    }

    SDL_Window *window = SDL_CreateWindow("RR64 RT64 Probe", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                          1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (window == nullptr) {
        report("SDL_CreateWindow(SDL_WINDOW_VULKAN) FAILED: %s", SDL_GetError());
        report("RESULT: FAIL - no Vulkan SDL window.");
        SDL_Quit();
        waitForExit();
        return 1;
    }
    int windowW = 0;
    int windowH = 0;
    SDL_GetWindowSize(window, &windowW, &windowH);
    report("SDL window created: %dx%d.", windowW, windowH);

    // Build the stub core exactly like RecompFrontend's RT64Context does.
    // On Android/Linux the window handle IS the SDL_Window* (plume patch
    // 0020 makes the SDL typedef win over ANativeWindow*).
    RT64::Application::Core core{};
    core.window = window;
    core.HEADER = g_header;
    core.RDRAM = g_rdram;
    core.DMEM = g_dmem;
    core.IMEM = g_imem;
    core.checkInterrupts = dummyCheckInterrupts;
    core.MI_INTR_REG = &g_miIntr;
    core.DPC_START_REG = &g_dpcStart;
    core.DPC_END_REG = &g_dpcEnd;
    core.DPC_CURRENT_REG = &g_dpcCurrent;
    core.DPC_STATUS_REG = &g_dpcStatus;
    core.DPC_CLOCK_REG = &g_dpcClock;
    core.DPC_BUFBUSY_REG = &g_dpcBufBusy;
    core.DPC_PIPEBUSY_REG = &g_dpcPipeBusy;
    core.DPC_TMEM_REG = &g_dpcTmem;
    core.VI_STATUS_REG = &g_viStatus;
    core.VI_ORIGIN_REG = &g_viOrigin;
    core.VI_WIDTH_REG = &g_viWidth;
    core.VI_INTR_REG = &g_viIntr;
    core.VI_V_CURRENT_LINE_REG = &g_viVCurrentLine;
    core.VI_TIMING_REG = &g_viTiming;
    core.VI_V_SYNC_REG = &g_viVSync;
    core.VI_H_SYNC_REG = &g_viHSync;
    core.VI_LEAP_REG = &g_viLeap;
    core.VI_H_START_REG = &g_viHStart;
    core.VI_V_START_REG = &g_viVStart;
    core.VI_V_BURST_REG = &g_viVBurst;
    core.VI_X_SCALE_REG = &g_viXScale;
    core.VI_Y_SCALE_REG = &g_viYScale;

    RT64::ApplicationConfiguration appConfig{};
    appConfig.appId = "rr64probe";
    appConfig.useConfigurationFile = false;
    // Deviation from RT64Context (which leaves detectDataPath on): the
    // probe must not go looking for a data dir on the phone.
    appConfig.detectDataPath = false;

    auto app = std::make_unique<RT64::Application>(core, appConfig);
    // Force Vulkan (the game only ever uses Vulkan on Android) and the
    // console presentation mode the game uses.
    app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Vulkan;
    app->userConfig.developerMode = false;
    app->enhancementConfig.presentation.mode = RT64::EnhancementConfiguration::Presentation::Mode::Console;

    report("RT64: calling app->setup() ...");
    RT64::Application::SetupResult setupResult = app->setup(0);
    report("RT64: setup() returned %s.", setupResultStr(setupResult));
    if (setupResult != RT64::Application::SetupResult::Success) {
        report("RESULT: FAIL - RT64 setup failed (%s).", setupResultStr(setupResult));
        if (setupResult == RT64::Application::SetupResult::GraphicsDeviceNotFound) {
            report("GraphicsDeviceNotFound means: plume found no usable device, OR the");
            report("Android capability gate fired (descriptor indexing / scalar block");
            report("layout missing). See the RAW VULKAN lines above for which one.");
        }
        SDL_DestroyWindow(window);
        SDL_Quit();
        waitForExit();
        return 1;
    }

    const plume::RenderDeviceDescription &desc = app->device->getDescription();
    const plume::RenderDeviceCapabilities &caps = app->device->getCapabilities();
    report("RT64: chosen API: %s.",
           app->chosenGraphicsAPI == RT64::UserConfiguration::GraphicsAPI::Vulkan ? "Vulkan" : "other");
    report("RT64: device: %s", desc.name.c_str());
    report("RT64: vendor: 0x%X, driver version: 0x%llX, VRAM: %llu MiB.",
           static_cast<unsigned>(desc.vendor), static_cast<unsigned long long>(desc.driverVersion),
           static_cast<unsigned long long>(desc.dedicatedVideoMemory / (1024 * 1024)));
    report("RT64: descriptorIndexing: %s, scalarBlockLayout: %s.", boolStr(caps.descriptorIndexing),
           boolStr(caps.scalarBlockLayout));
    report("RT64: presentWait: %s, sampleLocations: %s, raytracing: %s.", boolStr(caps.presentWait),
           boolStr(caps.sampleLocations), boolStr(caps.raytracing));
    report("RT64: swapchain: %ux%u.", app->swapChain->getWidth(), app->swapChain->getHeight());

    // Frame 1: submit the stub VI framebuffer and wait until the present
    // actually completes. If this hangs, the present never completed:
    // report a freeze at "frame 1 submitted".
    report("RT64: presenting frame 1 (stub VI framebuffer) ...");
    app->updateScreen();
    report("RT64: frame 1 submitted; waiting for present to complete ...");
    app->presentQueue->waitForPresentId(app->state->presentId);
    report("RT64: FRAME 1 PRESENTED.");

    // Swapchain recreate: resize the window and present again. RT64's
    // present queue recreates the swapchain when the size changes.
    const uint32_t swapWBefore = app->swapChain->getWidth();
    const uint32_t swapHBefore = app->swapChain->getHeight();
    SDL_SetWindowSize(window, 960, 540);
    SDL_PumpEvents();
    SDL_Event event;
    while (SDL_PollEvent(&event) != 0) {
    }
    SDL_Delay(200);
    report("RT64: window resized to 960x540; presenting frame 2 ...");
    app->updateScreen();
    app->presentQueue->waitForPresentId(app->state->presentId);
    report("RT64: FRAME 2 PRESENTED.");
    report("RT64: swapchain was %ux%u, now %ux%u.", swapWBefore, swapHBefore, app->swapChain->getWidth(),
           app->swapChain->getHeight());
    if (app->swapChain->getWidth() != swapWBefore || app->swapChain->getHeight() != swapHBefore) {
        report("RT64: swapchain size CHANGED after resize (recreate observed).");
    } else {
        report("RT64: swapchain size UNCHANGED after resize (recreate NOT observed;");
        report("RT64: the window resize may not have reached the surface yet).");
    }

    report("========================================");
    report("RESULT: PASS - RT64 device created, 2 frames presented.");
    report("Press Back (or close the app) to exit.");
    report("========================================");

    waitForExit();
    app->end();
    app.reset();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
