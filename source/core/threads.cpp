// threads.cpp — реализация на pthread с явным размером стека.
#include "core/threads.hpp"

#include <pthread.h>

#include <algorithm>
#include <new>

namespace tnx::threads {
namespace {

struct Job {
    std::function<void()> fn;
};

void* runJob(void* p) {
    Job* j = static_cast<Job*>(p);
    if (j->fn) j->fn();
    delete j;
    return nullptr;
}

int makeAttr(pthread_attr_t* attr, size_t stack, bool detached) {
    if (pthread_attr_init(attr) != 0) return -1;
    // newlib ждёт int; держим разумные границы, чтобы не сломать аллокатор.
    size_t bytes = std::clamp<size_t>(stack, 128u * 1024u, 32u * 1024u * 1024u);
    if (pthread_attr_setstacksize(attr, (int)bytes) != 0) {
        // Не поддерживается конкретным портом — пусть будет дефолт.
    }
    if (detached) pthread_attr_setdetachstate(attr, PTHREAD_CREATE_DETACHED);
    return 0;
}

} // namespace

void spawn(std::function<void()> fn, size_t stack) {
    if (!fn) return;
    Job* job = new (std::nothrow) Job{std::move(fn)};
    if (!job) return;

    pthread_attr_t attr;
    if (makeAttr(&attr, stack, true) != 0) {
        delete job;
        return;
    }
    pthread_t th;
    int rc = pthread_create(&th, &attr, runJob, job);
    pthread_attr_destroy(&attr);
    if (rc != 0) delete job;
}

// ---------------------------------------------------------------- Thread

Thread::~Thread() {
    if (!task_) return;
    // Не джойнили — отцепляем, чтобы pthread не держал ресурсы.
    pthread_detach((pthread_t)handle_);
}

Thread::Thread(Thread&& other) noexcept : task_(other.task_), handle_(other.handle_) {
    other.task_ = nullptr;
    other.handle_ = 0;
}

Thread& Thread::operator=(Thread&& other) noexcept {
    if (this != &other) {
        if (task_) pthread_detach((pthread_t)handle_);
        task_ = other.task_;
        handle_ = other.handle_;
        other.task_ = nullptr;
        other.handle_ = 0;
    }
    return *this;
}

bool Thread::start(std::function<void()> fn, size_t stack) {
    if (task_) return false;
    if (!fn) return false;
    Job* job = new (std::nothrow) Job{std::move(fn)};
    if (!job) return false;

    pthread_attr_t attr;
    if (makeAttr(&attr, stack, false) != 0) {
        delete job;
        return false;
    }
    pthread_t th;
    int rc = pthread_create(&th, &attr, runJob, job);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        delete job;
        return false;
    }
    task_ = job;
    handle_ = (unsigned long)th;
    return true;
}

void Thread::join() {
    if (!task_) return;
    pthread_join((pthread_t)handle_, nullptr);
    task_ = nullptr;
    handle_ = 0;
}

} // namespace tnx::threads