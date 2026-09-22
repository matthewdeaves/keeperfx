#include "pre_inc.h"
#include "kfx/renderer/RenderThreadManager.h"
#include "kfx/renderer/RendererThread.h"
#include "kfx/profiling/KfxProfiling.h"
#include "kfx/platform/IPlatform.h"
#include <chrono>
#include "post_inc.h"

thread_local bool g_on_render_thread = false;

// Blocks the main thread until pred() holds, but keeps servicing work the
// platform queues for the main thread meanwhile, so a render thread waiting
// on the main thread can finish.
template <typename Pred>
static void wait_servicing_main_thread(std::unique_lock<std::mutex>& lock,
                                       std::condition_variable& cv, Pred pred)
{
    while (!cv.wait_for(lock, std::chrono::milliseconds(2), pred))
    {
        lock.unlock();
        GetPlatform()->ServiceMainThreadQueue();
        lock.lock();
    }
}

RenderThreadManager::~RenderThreadManager()
{
    if (m_active)
        Stop();
}

void RenderThreadManager::Start(Fn init_fn, Fn work_fn, Fn cleanup_fn)
{
    if (m_active)
        return;

    m_active      = true;
    m_initialized = false;
    m_quit        = false;
    m_finished    = false;
    m_work_ready  = false;
    m_work_done   = true;

    m_thread = std::thread(
        &RenderThreadManager::ThreadProc, this,
        std::move(init_fn), std::move(work_fn), std::move(cleanup_fn));

    std::unique_lock<std::mutex> lock(m_mutex);
    wait_servicing_main_thread(lock, m_cv, [this]{ return m_initialized; });
}

void RenderThreadManager::WaitForCompletion()
{
    KFX_ZONE_COLOR("RenderThreadManager::WaitForCompletion", KFX_COLOR_SIMULATION);
    std::unique_lock<std::mutex> lock(m_mutex);
    wait_servicing_main_thread(lock, m_cv, [this]{ return m_work_done; });
}

void RenderThreadManager::Signal()
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_work_done  = false;
        m_work_ready = true;
    }
    m_cv.notify_one();
}

void RenderThreadManager::Stop()
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_quit = true;
    }
    m_cv.notify_one();
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        wait_servicing_main_thread(lock, m_cv, [this]{ return m_finished; });
    }
    m_thread.join();
    m_active = false;
}

void RenderThreadManager::ThreadProc(Fn init_fn, Fn work_fn, Fn cleanup_fn)
{
    g_on_render_thread = true;
    RendererThread_RegisterRenderThread();
    init_fn();

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_initialized = true;
    }
    m_cv.notify_one();

    for (;;)
    {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this]{ return m_work_ready || m_quit; });
            if (m_quit) break;
            m_work_ready = false;
        }

        {
            KFX_ZONE_COLOR("RenderThreadManager::work_fn", KFX_COLOR_RENDER_CPU);
            work_fn();
        }

        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_work_done = true;
        }
        m_cv.notify_one();
    }

    cleanup_fn();

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_finished = true;
    }
    m_cv.notify_all();
}
