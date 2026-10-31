#pragma once

#include <Arduino.h>
#include <array>
#include <memory>
#include <string>

#include "device_config.h"

namespace ledlab {

struct Rgb {
    float r = 0;
    float g = 0;
    float b = 0;
};

struct RuntimeError {
    std::string code;
    std::string message;
    uint16_t line = 0;
    uint16_t column = 0;
};

struct RuntimeJob {
    uint32_t id = 0;
    bool pending = false;
    bool complete = false;
    bool success = false;
    std::string name;
    RuntimeError error;
};

class AnimationRuntime {
public:
    AnimationRuntime();
    ~AnimationRuntime();

    bool queueCompile(const std::string& name, const std::string& source,
                      uint32_t& jobId, RuntimeError& error);
    void serviceCompile();
    const RuntimeJob& job() const { return job_; }

    bool renderFrame(float dt, std::array<Rgb, config::LED_COUNT>& output,
                     RuntimeError& error, bool singleStep = false);
    void setTimeScale(float value) { timeScale_ = value; }
    void stop();
    bool running() const { return running_; }
    const std::string& animationName() const;
    uint32_t frameCount() const { return frameCount_; }
    float simulationTime() const { return simulationTime_; }
    uint32_t lastComputeTimeUs() const { return lastComputeTimeUs_; }

private:
    struct Program;
    std::unique_ptr<Program> active_;
    std::string pendingName_;
    std::string pendingSource_;
    RuntimeJob job_;
    uint32_t nextJobId_ = 1;
    uint32_t jobId_ = 0;
    uint32_t frameCount_ = 0;
    uint32_t lastComputeTimeUs_ = 0;
    float simulationTime_ = 0;
    float timeScale_ = 1;
    bool running_ = false;
};

}  // namespace ledlab
