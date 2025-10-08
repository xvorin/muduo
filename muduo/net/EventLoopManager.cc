/*
 * EventLoopManager.cc
 *
 *  Created on: 2020年8月5日
 *      Author: chewenyao
 */

#include "muduo/net/EventLoopManager.h"
#include "muduo/base/CountDownLatch.h"
#include "muduo/net/EventLoop.h"

#include <cxxabi.h>
#include <execinfo.h>
#include <limits.h>
#include <mutex>
#include <set>
#include <signal.h>
#include <sstream>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#define AFLLOG(lvl, fmt, ...)

#define gLogT(...) AFLLOG(0, __VA_ARGS__)
#define gLogD(...) AFLLOG(1, __VA_ARGS__)
#define gLogI(...) AFLLOG(2, __VA_ARGS__)
#define gLogW(...) AFLLOG(3, __VA_ARGS__)
#define gLogE(...) AFLLOG(4, __VA_ARGS__)
#define gLogF(...) printf(__VA_ARGS__) // AFLLOG(5, __VA_ARGS__)

namespace muduo {
namespace net {

struct EventLoopManagerContext {

    const int PTHREAD_CREATE_RETRY_TIME = 3;

    std::shared_ptr<muduo::net::EventLoop> s_MainLoop;
    std::atomic<bool> s_MainLoopRunning { false };

    MutexLock s_Thread2EventLoopMapLock;
    MutexLock s_Name2EventLoopMapLock;
    MutexLock s_ModuleThreadAttrsLock;

    /** store the pthread_t to event loop map for getCurrentEventLoop() interface */
    std::unordered_map<pthread_t, std::weak_ptr<muduo::net::EventLoop>> s_Thread2EventLoopMap GUARDED_BY(s_Thread2EventLoopMapLock);

    /** store the name to  event loop map for getEventLoop(name) interface */
    std::unordered_map<std::string, std::weak_ptr<muduo::net::EventLoop>> s_Name2EventLoopMap GUARDED_BY(s_Name2EventLoopMapLock);

    /** store the name to thread attribute map for createEventLoop()  */
    std::unordered_map<std::string, EventLoopManager::ThreadAttribute> s_ModuleThreadAttrs GUARDED_BY(s_ModuleThreadAttrsLock);

    std::once_flag s_InitOnce;
};

static EventLoopManagerContext& getContext()
{
    static EventLoopManagerContext evmEtx;
    return evmEtx;
}

EventLoopManager::EventLoopManager(bool main_loop)
{
    auto& ctx = getContext();

    std::call_once(ctx.s_InitOnce, [&]() {
        if (main_loop) {
            ctx.s_MainLoop = std::make_shared<muduo::net::EventLoop>();
            MutexLockGuard guard(ctx.s_Thread2EventLoopMapLock);
            ctx.s_Thread2EventLoopMap[pthread_self()] = ctx.s_MainLoop;
        }
    });
}

EventLoopManager::~EventLoopManager()
{
}

void EventLoopManager::run()
{
    auto& ctx = getContext();

    ctx.s_MainLoopRunning = true;

    if (ctx.s_MainLoop) {
        installTraceBackSignals();
        ctx.s_MainLoop->loop();
    } else {
        while (ctx.s_MainLoopRunning) {
            sleep(1);
        }
    }
}

void EventLoopManager::quit()
{
    auto& ctx = getContext();

    {
        MutexLockGuard guard(ctx.s_Thread2EventLoopMapLock);
        for (auto& ev : ctx.s_Thread2EventLoopMap) {
            auto loop = ev.second.lock();
            if (loop) {
                loop->quit();
            }
        }
    }

    if (ctx.s_MainLoop) {
        ctx.s_MainLoop->quit();
    }
    ctx.s_MainLoopRunning = false;
}

int EventLoopManager::createCustomAttributes(const std::string& name,
    EvmThreadCreateType unique, int stackSize, int policy, int priority, std::vector<bool> affinity)
{
    if (name.empty()) {
        return -1;
    }

    stackSize *= 1024; /* stackSize KBytes */
    stackSize = std::max(static_cast<int>(PTHREAD_STACK_MIN), stackSize);
    stackSize = std::min(stackSize, static_cast<int>(getUpperLimit(RLIMIT_STACK, 8192 * 1024)));

    if (SCHED_FIFO != policy && SCHED_RR != policy && SCHED_OTHER != policy) {
        return -2;
    }

    if (SCHED_FIFO == policy || SCHED_RR == policy) {
        priority = std::max(priority, sched_get_priority_min(policy));
        priority = std::min(priority, sched_get_priority_max(policy));
    }

    ThreadAttribute attr;
    attr.name = name;
    attr.unique = unique;
    attr.stackSize = stackSize;
    attr.policy = policy;
    attr.priority = priority;
    attr.affinity = affinity;

    auto& ctx = getContext();
    {
        MutexLockGuard guard(ctx.s_ModuleThreadAttrsLock);
        ctx.s_ModuleThreadAttrs[name] = attr;
    }

    gLogT("createCustomAttributes for %s(%d,%d,%d,%d)", name.c_str(),
        (int)attr.unique, attr.stackSize, attr.policy, attr.priority);
    return 0;
}

std::shared_ptr<muduo::net::EventLoop> EventLoopManager::getEventLoop(
    const std::string& name)
{
    std::shared_ptr<muduo::net::EventLoop> retval = nullptr;
    ThreadAttribute attr;

    auto& ctx = getContext();
    {
        MutexLockGuard guard(ctx.s_ModuleThreadAttrsLock);
        auto attrIter = ctx.s_ModuleThreadAttrs.find(name);
        if (attrIter == ctx.s_ModuleThreadAttrs.end()) {
            assert(ctx.s_MainLoop);
            return ctx.s_MainLoop;
        }

        attr = attrIter->second;
    }

    if (muduo::net::EvmMulti == attr.unique) {
        retval = createEventLoop(attr);
        if (!retval) {
            gLogW("Uses main loop for %s's request!", name.c_str());
            assert(ctx.s_MainLoop);
            retval = ctx.s_MainLoop;
        }
        gLogT("thread create done for %s!", name.c_str());
        return retval;
    }

    MutexLockGuard guard(ctx.s_Name2EventLoopMapLock);
    auto loopIter = ctx.s_Name2EventLoopMap.find(name);
    if (loopIter != ctx.s_Name2EventLoopMap.end()) {
        retval = loopIter->second.lock();
    }

    if (retval) {
        gLogT("unique thread had been created for %s!", name.c_str());
        return retval;
    }

    retval = createEventLoop(attr);
    if (!retval) {
        gLogW("Uses main loop for %s's unique request!", name.c_str());
        assert(ctx.s_MainLoop);
        retval = ctx.s_MainLoop;
    }
    ctx.s_Name2EventLoopMap[name] = retval;
    gLogT("unique thread create done for %s!", name.c_str());

    return retval;
}

std::shared_ptr<muduo::net::EventLoop> EventLoopManager::getCurrentEventLoop()
{
    auto& ctx = getContext();

    std::shared_ptr<muduo::net::EventLoop> retval;
    pthread_t tid = pthread_self();

    {
        MutexLockGuard guard(ctx.s_Thread2EventLoopMapLock);
        auto iter = ctx.s_Thread2EventLoopMap.find(tid);
        retval = (iter == ctx.s_Thread2EventLoopMap.end()) ? nullptr : iter->second.lock();
    }

    return retval;
}

std::shared_ptr<muduo::net::EventLoop> EventLoopManager::createEventLoop(
    const ThreadAttribute& attr)
{
    std::shared_ptr<muduo::net::EventLoop> retval = nullptr;

    muduo::CountDownLatch cdl(1);
    void* args[] = { &cdl, &retval };

    pthread_t tid;
    struct sched_param param;
    pthread_attr_t pAttr;
    memset(&pAttr, 0x00, sizeof(pAttr));

    pthread_attr_init(&pAttr);
    pthread_attr_setscope(&pAttr, PTHREAD_SCOPE_SYSTEM);
    pthread_attr_setstacksize(&pAttr, attr.stackSize);

    if (SCHED_FIFO == attr.policy || SCHED_RR == attr.policy) {
        pthread_attr_setschedpolicy(&pAttr, attr.policy);
        pthread_attr_getschedparam(&pAttr, &param);

        param.sched_priority = (0 == geteuid()) ? (attr.priority) : 0;
        pthread_attr_setschedparam(&pAttr, &param);
    }

    auto& ctx = getContext();
    int retry = ctx.PTHREAD_CREATE_RETRY_TIME;
    while (1) {
        int retcode = pthread_create(&tid, &pAttr, createEventLoopDetail, args);
        if (0 != retcode && (EAGAIN == errno || EWOULDBLOCK == errno)
            && retry-- > 0) {
            gLogW("pthread_create failure because of EAGAIN, retry(%d/%d)!",
                PTHREAD_CREATE_RETRY_TIME - retry,
                PTHREAD_CREATE_RETRY_TIME);
            usleep(1 * 1000);
            continue;
        }

        if (0 != retcode) {
            gLogE("pthread_create error, reason:%s!", strerror(retcode));
            return nullptr;
        }

        if (!attr.affinity.empty()) {
            cpu_set_t mask;
            CPU_ZERO(&mask);
            for (size_t i = 0; i < attr.affinity.size(); i++) {
                if (attr.affinity[i]) {
                    CPU_SET(i, &mask);
                }
            }

            retcode = pthread_setaffinity_np(tid, sizeof(mask), &mask);
            if (0 != retcode) {
                gLogW("pthread_setaffinity_np error, reason:%s!", strerror(retcode));
            }
        }

        break;
    }

    /* waiting for new EventLoop */
    cdl.wait();
    assert(retval);

    if (0 != pthread_detach(tid)) {
        pthread_attr_destroy(&pAttr);
        gLogW("pthread_detach(tid=%lu) error! reason:%s", tid, strerror(errno));
    }
    gLogT("New thread(tid=%lu) created!", tid);

    return retval;
}

void* EventLoopManager::createEventLoopDetail(void* arg)
{
    pthread_t tid = pthread_self();
    muduo::CountDownLatch* cdl = reinterpret_cast<muduo::CountDownLatch*>((static_cast<char**>(arg))[0]);
    std::shared_ptr<muduo::net::EventLoop>* retloop = reinterpret_cast<std::shared_ptr<muduo::net::EventLoop>*>((static_cast<char**>(arg))[1]);

    assert(cdl && retloop);

    auto loop = std::make_shared<muduo::net::EventLoop>();
    *retloop = loop;

    auto& ctx = getContext();
    {
        MutexLockGuard guard(ctx.s_Thread2EventLoopMapLock);
        ctx.s_Thread2EventLoopMap[tid] = loop;
    }

    /* notify main thread going */
    cdl->countDown();

    /* detect if idle, and quit this thread */
    auto timer = loop->runEvery(5, [&]() {   if(loop.use_count() <= 1) loop->quit(); });

    installTraceBackSignals();

    /* working */
    loop->loop();

    loop->cancel(timer);

    {
        MutexLockGuard guard(ctx.s_Thread2EventLoopMapLock);
        auto iter = ctx.s_Thread2EventLoopMap.find(tid);
        if (iter != ctx.s_Thread2EventLoopMap.end()) {
            ctx.s_Thread2EventLoopMap.erase(iter);
        }
    }

    gLogT("thread %lu canceled!", tid);

    return nullptr;
}

void EventLoopManager::installTraceBackSignals()
{
    // std::set<int> tobeCaptured = { SIGILL, SIGABRT, SIGBUS, SIGFPE, SIGSEGV,
    //     SIGSYS, SIGSTKFLT, SIGXCPU, SIGXFSZ };

    // for (auto s : tobeCaptured) {
    //     signal(s, traceBack);
    // }

    signal(SIGPIPE, SIG_IGN);
}

void EventLoopManager::traceBack(int signo)
{
    static const int len = 256;
    void* buffer[len];
    int nptrs = ::backtrace(buffer, len);
    char** strings = ::backtrace_symbols(buffer, nptrs);
    if (!strings) {
        return;
    }

    std::stringstream ss;
    for (int i = 0; i < nptrs; ++i) {
        const std::string line(strings[i]); // ./test(_ZN6detail12c_do_nothingEfi+0x44) [0x401974]
        const std::string unmangle = demangleName(line);
        ss << (unmangle.empty() ? line : unmangle) << std::endl;
    }
    free(strings);

    gLogF("exit by signal %d \n%s", signo, ss.str().c_str());

    _exit(signo);
}

std::string EventLoopManager::demangleName(const std::string& mangled)
{
    std::string retval;
    int status = 0;

    static const size_t max_size = 2048;
    char temp[max_size];

    char unmangled[max_size];
    size_t buf_size = max_size;

    if (1 != sscanf(mangled.c_str(), "%*[^(]%*[^_]%127[^)+]", temp)) {
        return retval;
    }

    if ((nullptr != abi::__cxa_demangle(temp, unmangled, &buf_size, &status)) && (0 == status)) {
        retval.assign(unmangled, buf_size);
    }

    return retval;
}

size_t EventLoopManager::getUpperLimit(int type, size_t dft)
{
    struct rlimit rlim;
    if (0 != getrlimit(type, &rlim)) {
        return dft;
    }

    return std::min(rlim.rlim_cur, rlim.rlim_max);
}

}
}