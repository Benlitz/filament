/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "details/Engine.h"

#include "MaterialParser.h"

#include "details/DFG.h"
#include "details/VertexBuffer.h"
#include "details/Fence.h"
#include "details/Camera.h"
#include "details/IndexBuffer.h"
#include "details/IndirectLight.h"
#include "details/Material.h"
#include "details/Renderer.h"
#include "details/RenderPrimitive.h"
#include "details/Scene.h"
#include "details/Skybox.h"
#include "details/Stream.h"
#include "details/SwapChain.h"
#include "details/Texture.h"
#include "details/View.h"

#include "fg/ResourceAllocator.h"


#include <private/filament/SibGenerator.h>

#include <filament/MaterialEnums.h>

#include <utils/compiler.h>
#include <utils/Log.h>
#include <utils/Panic.h>
#include <utils/Systrace.h>

#include <memory>

#include "generated/resources/materials.h"

using namespace filament::math;
using namespace utils;

namespace filament {

using namespace backend;
using namespace filaflat;

namespace details {

// The global list of engines
static std::unordered_map<Engine const*, std::unique_ptr<FEngine>> sEngines;
static std::mutex sEnginesLock;

FEngine* FEngine::create(Backend backend, Platform* platform, void* sharedGLContext) {
    FEngine* instance = new FEngine(backend, platform, sharedGLContext);

    slog.i << "FEngine (" << sizeof(void*) * 8 << " bits) created at " << instance << " "
            << "(threading is " << (UTILS_HAS_THREADING ? "enabled)" : "disabled)") << io::endl;

    // initialize all fields that need an instance of FEngine
    // (this cannot be done safely in the ctor)

    // Normally we launch a thread and create the context and Driver from there (see FEngine::loop).
    // In the single-threaded case, we do so in the here and now.
    if (!UTILS_HAS_THREADING) {
        // we don't own the external context at that point, set it to null
        instance->mPlatform = nullptr;
        if (platform == nullptr) {
            platform = DefaultPlatform::create(&instance->mBackend);
            instance->mPlatform = platform;
            instance->mOwnPlatform = true;
        }
        instance->mDriver = platform->createDriver(sharedGLContext);
    } else {
        // start the driver thread
        instance->mDriverThread = std::thread(&FEngine::loop, instance);

        // wait for the driver to be ready
        instance->mDriverBarrier.await();

        if (UTILS_UNLIKELY(!instance->mDriver)) {
            // something went horribly wrong during driver initialization
            instance->mDriverThread.join();
            return nullptr;
        }
    }

    // Add this Engine to the list of active Engines
    { // scope for the lock
        std::unique_ptr<FEngine> engine(instance);
        std::lock_guard<std::mutex> guard(sEnginesLock);
        Engine* handle = engine.get();
        sEngines[handle] = std::move(engine);
    }

    // now we can initialize the largest part of the engine
    instance->init();

    if (!UTILS_HAS_THREADING) {
        instance->execute();
    }

    return instance;
}

void FEngine::assertValid(Engine const& engine) {
    bool valid;
    { // scope for the lock
        std::lock_guard<std::mutex> guard(sEnginesLock);
        auto const& engines = sEngines;
        auto const& pos = engines.find(&engine);
        valid = pos != engines.end();
    }
    ASSERT_POSTCONDITION(valid,
            "Using an Engine instance (@ %p) after it's been destroyed", &engine);
}

// these must be static because only a pointer is copied to the render stream
// Note that these coordinates are specified in OpenGL clip space. Other backends can transform
// these in the vertex shader as needed.
static const half4 sFullScreenTriangleVertices[3] = {
        { -1.0_h, -1.0_h, 1.0_h, 1.0_h },
        {  3.0_h, -1.0_h, 1.0_h, 1.0_h },
        { -1.0_h,  3.0_h, 1.0_h, 1.0_h }
};

// these must be static because only a pointer is copied to the render stream
static const uint16_t sFullScreenTriangleIndices[3] = { 0, 1, 2 };

FEngine::FEngine(Backend backend, Platform* platform, void* sharedGLContext) :
        mBackend(backend),
        mPlatform(platform),
        mSharedGLContext(sharedGLContext),
        mPostProcessManager(*this),
        mEntityManager(EntityManager::get()),
        mRenderableManager(*this),
        mTransformManager(),
        mLightManager(*this),
        mCameraManager(*this),
        mCommandBufferQueue(CONFIG_MIN_COMMAND_BUFFERS_SIZE, CONFIG_COMMAND_BUFFERS_SIZE),
        mPerRenderPassAllocator("per-renderpass allocator", CONFIG_PER_RENDER_PASS_ARENA_SIZE),
        mEngineEpoch(std::chrono::steady_clock::now()),
        mDriverBarrier(1)
{
    SYSTRACE_ENABLE();

    // we're assuming we're on the main thread here.
    // (it may not be the case)
    mJobSystem.adopt();
}

/*
 * init() is called just after the driver thread is initialized. Driver commands are therefore
 * possible.
 */

void FEngine::init() {
    // this must be first.
    mCommandStream = CommandStream(*mDriver, mCommandBufferQueue.getCircularBuffer());
    DriverApi& driverApi = getDriverApi();

    mResourceAllocator = new fg::ResourceAllocator(driverApi);

    mFullScreenTriangleVb = upcast(VertexBuffer::Builder()
            .vertexCount(3)
            .bufferCount(1)
            .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::HALF4, 0)
            .build(*this));

    mFullScreenTriangleVb->setBufferAt(*this, 0,
            { sFullScreenTriangleVertices, sizeof(sFullScreenTriangleVertices) });

    mFullScreenTriangleIb = upcast(IndexBuffer::Builder()
            .indexCount(3)
            .bufferType(IndexBuffer::IndexType::USHORT)
            .build(*this));

    mFullScreenTriangleIb->setBuffer(*this,
            { sFullScreenTriangleIndices, sizeof(sFullScreenTriangleIndices) });

    mFullScreenTriangleRph = driverApi.createRenderPrimitive();
    driverApi.setRenderPrimitiveBuffer(mFullScreenTriangleRph,
            mFullScreenTriangleVb->getHwHandle(), mFullScreenTriangleIb->getHwHandle(),
            mFullScreenTriangleVb->getDeclaredAttributes().getValue());
    driverApi.setRenderPrimitiveRange(mFullScreenTriangleRph, PrimitiveType::TRIANGLES,
            0, 0, 2, (uint32_t)mFullScreenTriangleIb->getIndexCount());

    mDefaultIblTexture = upcast(Texture::Builder()
            .width(1).height(1).levels(1)
            .format(Texture::InternalFormat::RGBA8)
            .sampler(Texture::Sampler::SAMPLER_CUBEMAP)
            .build(*this));
    static uint32_t pixel = 0;
    Texture::PixelBufferDescriptor buffer(
            &pixel, 4, // 4 bytes in 1 RGBA pixel
            Texture::Format::RGBA, Texture::Type::UBYTE);
    Texture::FaceOffsets offsets = {};
    mDefaultIblTexture->setImage(*this, 0, std::move(buffer), offsets);

    // 3 bands = 9 float3
    const float sh[9 * 3] = { 0.0f };
    mDefaultIbl = upcast(IndirectLight::Builder()
            .reflections(mDefaultIblTexture)
            .irradiance(3, reinterpret_cast<const float3*>(sh))
            .intensity(1.0f)
            .build(*this));

    // Always initialize the default material, most materials' depth shaders fallback on it.
    mDefaultMaterial = upcast(
            FMaterial::DefaultMaterialBuilder()
                    .package(MATERIALS_DEFAULTMATERIAL_DATA, MATERIALS_DEFAULTMATERIAL_SIZE)
                    .build(*const_cast<FEngine*>(this)));

    mPostProcessManager.init();
    mLightManager.init(*this);
    mDFG = std::make_unique<DFG>(*this);
}

FEngine::~FEngine() noexcept {
    ASSERT_DESTRUCTOR(mTerminated, "Engine destroyed but not terminated!");
    delete mResourceAllocator;
    delete mDriver;
    if (mOwnPlatform) {
        DefaultPlatform::destroy((DefaultPlatform**)&mPlatform);
    }
}

void FEngine::shutdown() {
#ifndef NDEBUG
    // print out some statistics about this run
    size_t wm = mCommandBufferQueue.getHigWatermark();
    size_t wmpct = wm / (CONFIG_COMMAND_BUFFERS_SIZE / 100);
    slog.d << "CircularBuffer: High watermark "
           << wm / 1024 << " KiB (" << wmpct << "%)" << io::endl;
#endif

    DriverApi& driver = getDriverApi();

    /*
     * Destroy our own state first
     */

    mPostProcessManager.terminate(driver);  // free-up post-process manager resources
    mResourceAllocator->terminate();
    mDFG->terminate();                      // free-up the DFG
    mRenderableManager.terminate();         // free-up all renderables
    mLightManager.terminate();              // free-up all lights
    mCameraManager.terminate();             // free-up all cameras

    driver.destroyRenderPrimitive(mFullScreenTriangleRph);
    destroy(mFullScreenTriangleIb);
    destroy(mFullScreenTriangleVb);

    destroy(mDefaultIblTexture);
    destroy(mDefaultIbl);

    destroy(mDefaultMaterial);

    /*
     * clean-up after the user -- we call terminate on each "leaked" object and clear each list.
     *
     * This should free up everything.
     */

    // try to destroy objects in the inverse dependency
    cleanupResourceList(mRenderers);
    cleanupResourceList(mViews);
    cleanupResourceList(mScenes);
    cleanupResourceList(mSkyboxes);

    // this must be done after Skyboxes and before materials
    destroy(mSkyboxMaterial);

    cleanupResourceList(mIndexBuffers);
    cleanupResourceList(mVertexBuffers);
    cleanupResourceList(mTextures);
    cleanupResourceList(mRenderTargets);
    cleanupResourceList(mMaterials);
    for (auto& item : mMaterialInstances) {
        cleanupResourceList(item.second);
    }
    cleanupResourceList(mFences);

    // There might be commands added by the terminate() calls
    flushCommandBuffer(mCommandBufferQueue);
    if (!UTILS_HAS_THREADING) {
        execute();
    }

    /*
     * terminate the rendering engine
     */

    mCommandBufferQueue.requestExit();
    if (UTILS_HAS_THREADING) {
        mDriverThread.join();
    }

    // detach this thread from the jobsystem
    mJobSystem.emancipate();

    mTerminated = true;
}

void FEngine::prepare() {
    SYSTRACE_CALL();
    // prepare() is called once per Renderer frame. Ideally we would upload the content of
    // UBOs that are visible only. It's not such a big issue because the actual upload() is
    // skipped is the UBO hasn't changed. Still we could have a lot of these.
    FEngine::DriverApi& driver = getDriverApi();
    for (auto& materialInstanceList : mMaterialInstances) {
        for (auto& item : materialInstanceList.second) {
            item->commit(driver);
        }
    }

    // Commit default material instances.
    for (auto& material : mMaterials) {
        material->getDefaultInstance()->commit(driver);
    }
}

void FEngine::gc() {
    // Note: this runs in a Job

    JobSystem& js = mJobSystem;
    auto parent = js.createJob();
    auto em = std::ref(mEntityManager);

    js.run(jobs::createJob(js, parent, &FRenderableManager::gc, &mRenderableManager, em),
            JobSystem::DONT_SIGNAL);
    js.run(jobs::createJob(js, parent, &FLightManager::gc, &mLightManager, em),
            JobSystem::DONT_SIGNAL);
    js.run(jobs::createJob(js, parent, &FTransformManager::gc, &mTransformManager, em),
            JobSystem::DONT_SIGNAL);
    js.run(jobs::createJob(js, parent, &FCameraManager::gc, &mCameraManager, em),
            JobSystem::DONT_SIGNAL);

    js.runAndWait(parent);
}

void FEngine::flush() {
    // flush the command buffer
    flushCommandBuffer(mCommandBufferQueue);
}

void FEngine::flushAndWait() {
    FFence::waitAndDestroy(FEngine::createFence(FFence::Type::SOFT), FFence::Mode::FLUSH);
}

// -----------------------------------------------------------------------------------------------
// Render thread / command queue
// -----------------------------------------------------------------------------------------------

int FEngine::loop() {
    // we don't own the external context at that point, set it to null
    Platform* platform = mPlatform;
    mPlatform = nullptr;

    if (platform == nullptr) {
        platform = DefaultPlatform::create(&mBackend);
        mPlatform = platform;
        mOwnPlatform = true;
        slog.d << "FEngine resolved backend: ";
        switch (mBackend) {
            case backend::Backend::NOOP:
                slog.d << "Noop";
                break;

            case backend::Backend::OPENGL:
                slog.d << "OpenGL";
                break;

            case backend::Backend::VULKAN:
                slog.d << "Vulkan";
                break;

            case backend::Backend::METAL:
                slog.d << "Metal";
                break;

            default:
                slog.d << "Unknown";
                break;
        }
        slog.d << io::endl;
    }

#if FILAMENT_ENABLE_MATDBG
    const char* portString = getenv("FILAMENT_MATDBG_PORT");
    if (portString != nullptr) {
        const int port = atoi(portString);
        debug.server = new matdbg::DebugServer(mBackend, port);

        // Sometimes the server can fail to spin up (e.g. if the above port is already in use).
        // When this occurs, carry onward, developers can look at civetweb.txt for details.
        if (!debug.server->isReady()) {
            delete debug.server;
            debug.server = nullptr;
        } else {
            debug.server->setEditCallback(FMaterial::onEditCallback);
            debug.server->setQueryCallback(FMaterial::onQueryCallback);
        }
    }
#endif

    mDriver = platform->createDriver(mSharedGLContext);
    mDriverBarrier.latch();
    if (UTILS_UNLIKELY(!mDriver)) {
        // if we get here, it's because the driver couldn't be initialized and the problem has
        // been logged.
        return 0;
    }

    JobSystem::setThreadName("FEngine::loop");
    JobSystem::setThreadPriority(JobSystem::Priority::DISPLAY);

    // We use the highest affinity bit, assuming this is a Big core in a  big.little
    // configuration. This is also a core not used by the JobSystem.
    // Either way the main reason to do this is to avoid this thread jumping from core to core
    // and loose its caches in the process.
    uint32_t id = std::thread::hardware_concurrency() - 1;

    while (true) {
        // looks like thread affinity needs to be reset regularly (on Android)
        JobSystem::setThreadAffinityById(id);
        if (!execute()) {
            break;
        }
    }

    // terminate() is a synchronous API
    getDriverApi().terminate();
    return 0;
}

void FEngine::flushCommandBuffer(CommandBufferQueue& commandQueue) {
    getDriver().purge();
    commandQueue.flush();
}

const FMaterial* FEngine::getSkyboxMaterial() const noexcept {
    FMaterial const* material = mSkyboxMaterial;
    if (UTILS_UNLIKELY(material == nullptr)) {
        material = FSkybox::createMaterial(*const_cast<FEngine*>(this));
        mSkyboxMaterial = material;
    }
    return material;
}

// -----------------------------------------------------------------------------------------------
// Resource management
// -----------------------------------------------------------------------------------------------

/*
 * Object created from a Builder
 */

template <typename T>
inline T* FEngine::create(ResourceList<T>& list, typename T::Builder const& builder) noexcept {
    T* p = mHeapAllocator.make<T>(*this, builder);
    list.insert(p);
    return p;
}

FVertexBuffer* FEngine::createVertexBuffer(const VertexBuffer::Builder& builder) noexcept {
    return create(mVertexBuffers, builder);
}

FIndexBuffer* FEngine::createIndexBuffer(const IndexBuffer::Builder& builder) noexcept {
    return create(mIndexBuffers, builder);
}

FTexture* FEngine::createTexture(const Texture::Builder& builder) noexcept {
    return create(mTextures, builder);
}

FIndirectLight* FEngine::createIndirectLight(const IndirectLight::Builder& builder) noexcept {
    return create(mIndirectLights, builder);
}

FMaterial* FEngine::createMaterial(const Material::Builder& builder) noexcept {
    return create(mMaterials, builder);
}

FSkybox* FEngine::createSkybox(const Skybox::Builder& builder) noexcept {
    return create(mSkyboxes, builder);
}

FStream* FEngine::createStream(const Stream::Builder& builder) noexcept {
    return create(mStreams, builder);
}

FRenderTarget* FEngine::createRenderTarget(const RenderTarget::Builder& builder) noexcept {
    return create(mRenderTargets, builder);
}

/*
 * Special cases
 */

FRenderer* FEngine::createRenderer() noexcept {
    FRenderer* p = mHeapAllocator.make<FRenderer>(*this);
    if (p) {
        mRenderers.insert(p);
        p->init();
    }
    return p;
}

FMaterialInstance* FEngine::createMaterialInstance(const FMaterial* material) noexcept {
    FMaterialInstance* p = mHeapAllocator.make<FMaterialInstance>(*this, material);
    if (p) {
        auto pos = mMaterialInstances.emplace(material, "MaterialInstance");
        pos.first->second.insert(p);
    }
    return p;
}

/*
 * Objects created without a Builder
 */

FScene* FEngine::createScene() noexcept {
    FScene* p = mHeapAllocator.make<FScene>(*this);
    if (p) {
        mScenes.insert(p);
    }
    return p;
}

FView* FEngine::createView() noexcept {
    FView* p = mHeapAllocator.make<FView>(*this);
    if (p) {
        mViews.insert(p);
    }
    return p;
}

FFence* FEngine::createFence(FFence::Type type) noexcept {
    FFence* p = mHeapAllocator.make<FFence>(*this, type);
    if (p) {
        mFences.insert(p);
    }
    return p;
}

FSwapChain* FEngine::createSwapChain(void* nativeWindow, uint64_t flags) noexcept {
    FSwapChain* p = mHeapAllocator.make<FSwapChain>(*this, nativeWindow, flags);
    if (p) {
        mSwapChains.insert(p);
    }
    return p;
}

/*
 * Objects created with a component manager
 */


FCamera* FEngine::createCamera(Entity entity) noexcept {
    return mCameraManager.create(entity);
}

FCamera* FEngine::getCameraComponent(Entity entity) noexcept {
    auto ci = mCameraManager.getInstance(entity);
    return ci ? mCameraManager.getCamera(ci) : nullptr;
}

void FEngine::destroyCameraComponent(utils::Entity entity) noexcept {
    mCameraManager.destroy(entity);
}


void FEngine::createRenderable(const RenderableManager::Builder& builder, Entity entity) {
    mRenderableManager.create(builder, entity);
    auto& tcm = mTransformManager;
    // if this entity doesn't have a transform component, add one.
    if (!tcm.hasComponent(entity)) {
        tcm.create(entity, 0, mat4f());
    }
}

void FEngine::createLight(const LightManager::Builder& builder, Entity entity) {
    mLightManager.create(builder, entity);
}

// -----------------------------------------------------------------------------------------------

template<typename T, typename L>
void FEngine::cleanupResourceList(ResourceList<T, L>& list) {
    if (!list.empty()) {
#ifndef NDEBUG
        slog.d << "cleaning up " << list.size()
               << " leaked " << CallStack::typeName<T>().c_str() << io::endl;
#endif
        // Move the list (copy-and-clear). We can only modify/access the list from this
        // thread, because it's not thread-safe.
        auto copy(list.getListAndClear());
        for (T* item : copy) {
            item->terminate(*this);
            mHeapAllocator.destroy(item);
        }
    }
}

// -----------------------------------------------------------------------------------------------

template<typename T, typename L>
void FEngine::terminateAndDestroy(const T* ptr, ResourceList<T, L>& list) {
    if (ptr != nullptr) {
        if (list.remove(ptr)) {
            const_cast<T*>(ptr)->terminate(*this);
            mHeapAllocator.destroy(const_cast<T*>(ptr));
        } else {
            // object not found, do nothing and log an error on DEBUG builds.
#ifndef NDEBUG
            slog.d << "object "
                   << CallStack::typeName<T>().c_str()
                   << " at " << ptr << " doesn't exist!"
                   << io::endl;
#endif
        }
    }
}

// -----------------------------------------------------------------------------------------------

void FEngine::destroy(const FVertexBuffer* p) {
    terminateAndDestroy(p, mVertexBuffers);
}

void FEngine::destroy(const FIndexBuffer* p) {
    terminateAndDestroy(p, mIndexBuffers);
}

inline void FEngine::destroy(const FRenderer* p) {
    terminateAndDestroy(p, mRenderers);
}

inline void FEngine::destroy(const FScene* p) {
    terminateAndDestroy(p, mScenes);
}

inline void FEngine::destroy(const FSkybox* p) {
    terminateAndDestroy(p, mSkyboxes);
}

UTILS_NOINLINE
void FEngine::destroy(const FTexture* p) {
    terminateAndDestroy(p, mTextures);
}

void FEngine::destroy(const FRenderTarget* p) {
    terminateAndDestroy(p, mRenderTargets);
}

inline void FEngine::destroy(const FView* p) {
    terminateAndDestroy(p, mViews);
}

inline void FEngine::destroy(const FIndirectLight* p) {
    terminateAndDestroy(p, mIndirectLights);
}

UTILS_NOINLINE
void FEngine::destroy(const FFence* p) {
    terminateAndDestroy(p, mFences);
}

void FEngine::destroy(const FSwapChain* p) {
    terminateAndDestroy(p, mSwapChains);
}

void FEngine::destroy(const FStream* p) {
    terminateAndDestroy(p, mStreams);
}


void FEngine::destroy(const FMaterial* ptr) {
    if (ptr != nullptr) {
        auto pos = mMaterialInstances.find(ptr);
        if (pos != mMaterialInstances.cend()) {
            // ensure we've destroyed all instances before destroying the material
            if (!ASSERT_PRECONDITION_NON_FATAL(pos->second.empty(),
                    "destroying material \"%s\" but %u instances still alive",
                    ptr->getName().c_str(), (*pos).second.size())) {
                return;
            }
        }
        terminateAndDestroy(ptr, mMaterials);
    }
}

void FEngine::destroy(const FMaterialInstance* ptr) {
    if (ptr != nullptr) {
        auto pos = mMaterialInstances.find(ptr->getMaterial());
        assert(pos != mMaterialInstances.cend());
        if (pos != mMaterialInstances.cend()) {
            terminateAndDestroy(ptr, pos->second);
        }
    }
}

void FEngine::destroy(Entity e) {
    mRenderableManager.destroy(e);
    mLightManager.destroy(e);
    mTransformManager.destroy(e);
    mCameraManager.destroy(e);
}

void* FEngine::streamAlloc(size_t size, size_t alignment) noexcept {
    // we allow this only for small allocations
    if (size > 1024) {
        return nullptr;
    }
    return getDriverApi().allocate(size, alignment);
}

bool FEngine::execute() {

    // wait until we get command buffers to be executed (or thread exit requested)
    auto buffers = mCommandBufferQueue.waitForCommands();
    if (UTILS_UNLIKELY(buffers.empty())) {
        return false;
    }

    // execute all command buffers
    for (auto& item : buffers) {
        if (UTILS_LIKELY(item.begin)) {
            mCommandStream.execute(item.begin);
            mCommandBufferQueue.releaseBuffer(item);
        }
    }

    return true;
}

} // namespace details

// ------------------------------------------------------------------------------------------------
// Trampoline calling into private implementation
// ------------------------------------------------------------------------------------------------

using namespace details;

Engine* Engine::create(Backend backend, Platform* platform, void* sharedGLContext) {
    return FEngine::create(backend, platform, sharedGLContext);
}

void Engine::destroy(Engine* engine) {
    destroy(&engine);
}

void Engine::destroy(Engine** engine) {
    if (engine) {
        std::unique_ptr<FEngine> filamentEngine;

        std::unique_lock<std::mutex> guard(sEnginesLock);
        auto const& pos = sEngines.find(*engine);
        if (pos != sEngines.end()) {
            std::swap(filamentEngine, pos->second);
            sEngines.erase(pos);
        }
        guard.unlock();

        // Make sure to call into shutdown() without the lock held
        if (filamentEngine) {
            filamentEngine->shutdown();
            // clear the user's handle
            *engine = nullptr;
        }
    }
}

// -----------------------------------------------------------------------------------------------
// Resource management
// -----------------------------------------------------------------------------------------------

const Material* Engine::getDefaultMaterial() const noexcept {
    return upcast(this)->getDefaultMaterial();
}

Backend Engine::getBackend() const noexcept {
    return upcast(this)->getBackend();
}

Renderer* Engine::createRenderer() noexcept {
    return upcast(this)->createRenderer();
}

View* Engine::createView() noexcept {
    return upcast(this)->createView();
}

Scene* Engine::createScene() noexcept {
    return upcast(this)->createScene();
}

Camera* Engine::createCamera(Entity entity) noexcept {
    return upcast(this)->createCamera(entity);
}

Camera* Engine::getCameraComponent(utils::Entity entity) noexcept {
    return upcast(this)->getCameraComponent(entity);
}

void Engine::destroyCameraComponent(utils::Entity entity) noexcept {
    upcast(this)->destroyCameraComponent(entity);
}

Fence* Engine::createFence() noexcept {
    return upcast(this)->createFence(FFence::Type::SOFT);
}

SwapChain* Engine::createSwapChain(void* nativeWindow, uint64_t flags) noexcept {
    return upcast(this)->createSwapChain(nativeWindow, flags);
}

void Engine::destroy(const VertexBuffer* p) {
    upcast(this)->destroy(upcast(p));
}

void Engine::destroy(const IndexBuffer* p) {
    upcast(this)->destroy(upcast(p));
}

void Engine::destroy(const IndirectLight* p) {
    upcast(this)->destroy(upcast(p));
}

void Engine::destroy(const Material* p) {
    upcast(this)->destroy(upcast(p));
}

void Engine::destroy(const MaterialInstance* p) {
    upcast(this)->destroy(upcast(p));
}

void Engine::destroy(const Renderer* p) {
    upcast(this)->destroy(upcast(p));
}

void Engine::destroy(const View* p) {
    upcast(this)->destroy(upcast(p));
}

void Engine::destroy(const Scene* p) {
    upcast(this)->destroy(upcast(p));
}

void Engine::destroy(const Skybox* p) {
    upcast(this)->destroy(upcast(p));
}

void Engine::destroy(const Stream* p) {
    upcast(this)->destroy(upcast(p));
}

void Engine::destroy(const Texture* p) {
    upcast(this)->destroy(upcast(p));
}

void Engine::destroy(const RenderTarget* p) {
    upcast(this)->destroy(upcast(p));
}

void Engine::destroy(const Fence* p) {
    upcast(this)->destroy(upcast(p));
}

void Engine::destroy(const SwapChain* p) {
    upcast(this)->destroy(upcast(p));
}

void Engine::destroy(Entity e) {
    upcast(this)->destroy(e);
}

void Engine::flushAndWait() {
    upcast(this)->flushAndWait();
}

RenderableManager& Engine::getRenderableManager() noexcept {
    return upcast(this)->getRenderableManager();
}

LightManager& Engine::getLightManager() noexcept {
    return upcast(this)->getLightManager();
}

TransformManager& Engine::getTransformManager() noexcept {
    return upcast(this)->getTransformManager();
}

void* Engine::streamAlloc(size_t size, size_t alignment) noexcept {
    return upcast(this)->streamAlloc(size, alignment);
}

// The external-facing execute does a flush, and is meant only for single-threaded environments.
// It also discards the boolean return value, which would otherwise indicate a thread exit.
void Engine::execute() {
    ASSERT_PRECONDITION(!UTILS_HAS_THREADING, "Execute is meant for single-threaded platforms.");
    upcast(this)->flush();
    upcast(this)->execute();
}

DebugRegistry& Engine::getDebugRegistry() noexcept {
    return upcast(this)->getDebugRegistry();
}


} // namespace filament
