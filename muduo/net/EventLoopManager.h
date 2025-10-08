/*
 * EventLoopManager.h
 *
 *  Created on: 2020年8月5日
 *      Author: chewenyao
 */

#ifndef NET_EVENTLOOPMANAGER_H_
#define NET_EVENTLOOPMANAGER_H_

#include "muduo/base/Mutex.h"
#include "muduo/base/noncopyable.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace muduo {
namespace net {

class EventLoop;

enum EvmThreadCreateType {
    EvmUnique,
    EvmMulti,
};

class EventLoopManager : public noncopyable {
public:
    struct ThreadAttribute {
        std::string name;
        EvmThreadCreateType unique;
        int policy;
        int priority;
        int stackSize;
        std::vector<bool> affinity;
    };

public:
    EventLoopManager(bool main_loop = true);
    virtual ~EventLoopManager();

    void run();
    void quit();

    /**
     * @param name      The keyword to index a EventLoop(attach to a thread)
     * @param unique    true:  Create a new instance with each getEventLoop() call
     *                  false: Use the same instance for each getEventLoop() call
     * @param stackSize Thread stack size. The unit is KBytes. /n
     *                  eg. 64 means 65536Bytes
     * @param policy    SCHED_FIFO/SCHED_RR/SCHED_OTHER
     * @param priority  Priority of real-time scheduling. The usual legal scope is [1, 99]
     * @return 0 : success           /n
     *         -1: name empty        /n
     *         -2: policy invalid    /n
     */
    int createCustomAttributes(const std::string& name, EvmThreadCreateType unique,
        int stackSize = 8 * 1024, int policy = SCHED_OTHER, int priority = 0, std::vector<bool> affinity = {});

    /**
     * @param name the keyword to get needed event loop
     * @return the needed event loop.
     */
    std::shared_ptr<muduo::net::EventLoop> getEventLoop(const std::string& name = "");

    /**
     * @return the event loop attach to current tid
     */
    std::shared_ptr<muduo::net::EventLoop> getCurrentEventLoop();

private:
    std::shared_ptr<muduo::net::EventLoop> createEventLoop(const ThreadAttribute& attr);

    static void* createEventLoopDetail(void* arg);
    static void installTraceBackSignals();
    static void traceBack(int signo);
    static std::string demangleName(const std::string& mangled);

    inline size_t getUpperLimit(int type, size_t dft);
};

} /* namespace net */
} /* namespace muduo */

#endif /* NET_EVENTLOOPMANAGER_H_ */
