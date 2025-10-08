// Use of this source code is governed by a BSD-style license
// that can be found in the License file.
//
// Author: Shuo Chen (chenshuo at chenshuo dot com)

#ifndef MUDUO_BASE_BLOCKINGQUEUE_H
#define MUDUO_BASE_BLOCKINGQUEUE_H

#include "muduo/base/Condition.h"
#include "muduo/base/Mutex.h"

#include <assert.h>
#include <deque>

namespace muduo {

template <typename T>
class BlockingQueue : public noncopyable {
public:
    explicit BlockingQueue(size_t s = 0)
        : limit_(s)
        , mutex_()
        , notEmpty_(mutex_)
        , queue_()
    {
    }

    void put(const T& x)
    {
        MutexLockGuard lock(mutex_);
        queue_.push_back(x);
        if (limit_ && queue_.size() > limit_) {
            queue_.pop_front();
        }
        notEmpty_.notify(); // wait morphing saves us
        // http://www.domaigne.com/blog/computing/condvars-signal-with-mutex-locked-or-not/
    }

    void put(T&& x)
    {
        MutexLockGuard lock(mutex_);
        queue_.push_back(std::move(x));
        if (limit_ && queue_.size() > limit_) {
            queue_.pop_front();
        }
        notEmpty_.notify();
    }

    T take()
    {
        MutexLockGuard lock(mutex_);
        // always use a while-loop, due to spurious wakeup
        while (queue_.empty()) {
            notEmpty_.wait();
        }
        assert(!queue_.empty());
        T front(std::move(queue_.front()));
        queue_.pop_front();
        return front;
    }

    bool take(T& front, double seconds)
    {
        MutexLockGuard lock(mutex_);
        // always use a while-loop, due to spurious wakeup
        while (queue_.empty()) {
            if (notEmpty_.waitForSeconds(seconds)) {
                return false;
            }
        }
        assert(!queue_.empty());
        front = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    size_t size() const
    {
        MutexLockGuard lock(mutex_);
        return queue_.size();
    }

private:
    const size_t limit_;
    mutable MutexLock mutex_;
    Condition notEmpty_ GUARDED_BY(mutex_);
    std::deque<T> queue_ GUARDED_BY(mutex_);
};

} // namespace muduo

#endif // MUDUO_BASE_BLOCKINGQUEUE_H
