#ifndef EASY_ASYNC_H
#define EASY_ASYNC_H

#define NOCALLBACK [](){}

#include <functional>
#include <memory>
#include <queue>
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#define ASYNC_DEBUG 1

#ifdef ASYNC_DEBUG
    #define ASYNC_LOG(fmt, ...) Serial.printf("[EasyAsync] " fmt "\n", ##__VA_ARGS__)
#else
    #define ASYNC_LOG(fmt, ...)
#endif

struct AsyncConfig {
    uint32_t defaultStackSize = 4096;
    UBaseType_t defaultPriority = 1;
    BaseType_t defaultCore = tskNO_AFFINITY;
    uint16_t maxConcurrentTasks = 10;
    bool executeCallbacksInLoop = true;
};

enum class TaskState {
    Pending,
    Running,
    Completed,
    Failed,
    Cancelled
};

class CallbackQueue {
public:
    static CallbackQueue& instance() {
        static CallbackQueue instance;
        return instance;
    }

    void enqueue(std::function<void()> callback) {
        if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
            queue.push(callback);
            xSemaphoreGive(mutex);
            ASYNC_LOG("Callback enqueued. Queue size: %d", queue.size());
        }
    }

    void process() {
        if (xSemaphoreTake(mutex, 0) == pdTRUE) {
            while (!queue.empty()) {
                auto callback = queue.front();
                queue.pop();
                xSemaphoreGive(mutex);
                
                ASYNC_LOG("Processing callback...");
                callback();
                
                if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
                    return;
                }
            }
            xSemaphoreGive(mutex);
        }
    }

    size_t size() {
        size_t sz = 0;
        if (xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE) {
            sz = queue.size();
            xSemaphoreGive(mutex);
        }
        return sz;
    }

private:
    CallbackQueue() {
        mutex = xSemaphoreCreateMutex();
    }

    ~CallbackQueue() {
        if (mutex) {
            vSemaphoreDelete(mutex);
        }
    }

    std::queue<std::function<void()>> queue;
    SemaphoreHandle_t mutex;
};

struct TaskConfig {
    uint32_t stackSize = 0;
    UBaseType_t priority = 0;
    BaseType_t core = tskNO_AFFINITY;
    const char* name = nullptr;
    uint32_t timeoutMs = 0;
    bool executeInLoop = true;
};

class TaskHandle {
public:
    TaskHandle() : taskHandle(nullptr), state(TaskState::Pending), 
                   cancelled(false), startTime(0), endTime(0) {}

    void setHandle(TaskHandle_t handle) { 
        taskHandle = handle; 
        state = TaskState::Running;
        startTime = millis();
        ASYNC_LOG("Task started at %lu ms", startTime);
    }

    TaskHandle_t getHandle() const { return taskHandle; }
    
    TaskState getState() const { return state; }
    
    void setState(TaskState newState) { 
        state = newState;
        if (newState == TaskState::Completed || 
            newState == TaskState::Failed || 
            newState == TaskState::Cancelled) {
            endTime = millis();
            ASYNC_LOG("Task ended at %lu ms. Duration: %lu ms", endTime, endTime - startTime);
        }
    }

    bool isCancelled() const { return cancelled; }
    
    void cancel() {
        cancelled = true;
        if (taskHandle != nullptr && state == TaskState::Running) {
            setState(TaskState::Cancelled);
            vTaskDelete(taskHandle);
            taskHandle = nullptr;
            ASYNC_LOG("Task cancelled");
        }
    }

    bool isRunning() const { 
        return state == TaskState::Running && !cancelled; 
    }

    uint32_t getExecutionTime() const {
        if (startTime == 0) return 0;
        if (endTime == 0) return millis() - startTime;
        return endTime - startTime;
    }

private:
    TaskHandle_t taskHandle;
    TaskState state;
    bool cancelled;
    uint32_t startTime;
    uint32_t endTime;
};

class Task {
public:
    Task() : handle(std::make_shared<TaskHandle>()), config() {}

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    
    Task(Task&& other) noexcept : handle(std::move(other.handle)), 
                                   config(other.config),
                                   taskFunc(std::move(other.taskFunc)) {}
    
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            handle = std::move(other.handle);
            config = other.config;
            taskFunc = std::move(other.taskFunc);
        }
        return *this;
    }

    template<typename Func, typename Callback>
    Task(Func func, Callback callback, const TaskConfig& cfg) 
        : handle(std::make_shared<TaskHandle>()), config(cfg) {
        using ResultType = decltype(func());
        taskFunc = [func, callback, h = handle, cfg]() {
            executeTask(func, callback, h, cfg, (ResultType*)nullptr);
        };
    }

    bool run() {
        if (!taskFunc) {
            ASYNC_LOG("ERROR: Task function is null");
            return false;
        }

        static uint32_t taskCounter = 0;
        char taskName[16];
        if (config.name == nullptr) {
            snprintf(taskName, sizeof(taskName), "Task_%lu", taskCounter++);
            config.name = taskName;
        }

        extern AsyncConfig globalConfig;
        uint32_t stackSize = config.stackSize > 0 ? config.stackSize : globalConfig.defaultStackSize;
        UBaseType_t priority = config.priority > 0 ? config.priority : globalConfig.defaultPriority;
        BaseType_t core = config.core != tskNO_AFFINITY ? config.core : globalConfig.defaultCore;

        auto wrapper = [](void* param) {
            auto* func = static_cast<std::function<void()>*>(param);
            try {
                (*func)();
            } catch (...) {
                ASYNC_LOG("ERROR: Exception in task");
            }
            delete func;
            vTaskDelete(NULL);
        };

        auto* funcCopy = new std::function<void()>(taskFunc);
        TaskHandle_t taskHandle = nullptr;

        BaseType_t result;
        if (core != tskNO_AFFINITY) {
            result = xTaskCreatePinnedToCore(wrapper, config.name, stackSize, 
                                            funcCopy, priority, &taskHandle, core);
            ASYNC_LOG("Creating task '%s' on core %d (stack: %u, priority: %u)", 
                     config.name, core, stackSize, priority);
        } else {
            result = xTaskCreate(wrapper, config.name, stackSize, 
                               funcCopy, priority, &taskHandle);
            ASYNC_LOG("Creating task '%s' on any core (stack: %u, priority: %u)", 
                     config.name, stackSize, priority);
        }

        if (result != pdPASS) {
            ASYNC_LOG("ERROR: Failed to create task");
            delete funcCopy;
            handle->setState(TaskState::Failed);
            return false;
        }

        handle->setHandle(taskHandle);
        return true;
    }

    void cancel() {
        if (handle) {
            handle->cancel();
        }
    }

    TaskState getState() const {
        return handle ? handle->getState() : TaskState::Failed;
    }

    bool isRunning() const {
        return handle ? handle->isRunning() : false;
    }

    bool isCancelled() const {
        return handle ? handle->isCancelled() : false;
    }

    uint32_t getExecutionTime() const {
        return handle ? handle->getExecutionTime() : 0;
    }

private:
    std::shared_ptr<TaskHandle> handle;
    TaskConfig config;
    std::function<void()> taskFunc;

    template<typename Func, typename Callback>
    static void executeTask(Func func, Callback callback, std::shared_ptr<TaskHandle> h, 
                          const TaskConfig& cfg, void*) {
        ASYNC_LOG("Executing void task...");
        
        if (h->isCancelled()) {
            ASYNC_LOG("Task was cancelled before execution");
            return;
        }

        try {
            func();
            
            if (!h->isCancelled()) {
                h->setState(TaskState::Completed);
                
                auto callbackWrapper = [callback]() {
                    ASYNC_LOG("Executing void callback");
                    callback();
                };

                if (cfg.executeInLoop) {
                    CallbackQueue::instance().enqueue(callbackWrapper);
                } else {
                    callbackWrapper();
                }
            }
        } catch (...) {
            ASYNC_LOG("Task failed with exception");
            h->setState(TaskState::Failed);
        }
    }

    template<typename Func, typename Callback, typename ResultType>
    static void executeTask(Func func, Callback callback, std::shared_ptr<TaskHandle> h, 
                          const TaskConfig& cfg, ResultType*) {
        ASYNC_LOG("Executing task with return type...");
        
        if (h->isCancelled()) {
            ASYNC_LOG("Task was cancelled before execution");
            return;
        }

        try {
            auto result = std::make_shared<ResultType>(func());
            
            if (!h->isCancelled()) {
                h->setState(TaskState::Completed);
                
                auto callbackWrapper = [callback, result]() {
                    ASYNC_LOG("Executing callback with result");
                    callback(*result);
                };

                if (cfg.executeInLoop) {
                    CallbackQueue::instance().enqueue(callbackWrapper);
                } else {
                    callbackWrapper();
                }
            }
        } catch (...) {
            ASYNC_LOG("Task failed with exception");
            h->setState(TaskState::Failed);
        }
    }
};

AsyncConfig globalConfig;

class Async {
public:
    static void setConfig(const AsyncConfig& config) {
        globalConfig = config;
        ASYNC_LOG("Global config updated");
    }

    static void update() {
        if (globalConfig.executeCallbacksInLoop) {
            CallbackQueue::instance().process();
        }
    }

    static size_t pendingCallbacks() {
        return CallbackQueue::instance().size();
    }

    template<typename Func, typename Callback>
    static Task Run(Func func, Callback cb) {
        TaskConfig config;
        return Run(func, cb, config);
    }

    template<typename Func, typename Callback>
    static Task Run(Func func, Callback cb, const TaskConfig& config) {
        Task task(func, cb, config);
        task.run();
        return task;
    }

    template<typename Func, typename Callback>
    static Task Create(Func func, Callback cb, const TaskConfig& config = TaskConfig()) {
        return Task(func, cb, config);
    }

    template<typename Func>
    static Task RunFireAndForget(Func func, const TaskConfig& config = TaskConfig()) {
        auto noopCallback = []() {};
        return Run(func, noopCallback, config);
    }

    template<typename Func, typename Callback>
    static Task RunAfter(uint32_t delayMs, Func func, Callback cb, 
                        const TaskConfig& config = TaskConfig()) {
        auto delayedFunc = [delayMs, func]() {
            vTaskDelay(pdMS_TO_TICKS(delayMs));
            return func();
        };
        return Run(delayedFunc, cb, config);
    }

    template<typename Func, typename Callback>
    static Task RunOnCore(int core, Func func, Callback cb) {
        TaskConfig config;
        config.core = core;
        return Run(func, cb, config);
    }

    template<typename Func, typename Callback>
    static Task RunWithPriority(UBaseType_t priority, Func func, Callback cb) {
        TaskConfig config;
        config.priority = priority;
        return Run(func, cb, config);
    }
};

#endif