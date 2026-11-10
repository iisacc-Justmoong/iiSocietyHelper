#pragma once
#include "iiSocietyHelper/Sync/Status.h"
#include <memory>

// Example host: onWake must enqueue drain on the receiver thread, never invoke
// it inline. Reserved wakes remain usable when unrelated work is saturated.
class ManualStatusExecutor final : public iiSocietyHelper::Sync::StatusExecutor {
public:
    explicit ManualStatusExecutor(std::function<void()> onWake = {});
    ~ManualStatusExecutor() override;
    std::shared_ptr<iiSocietyHelper::Sync::StatusWake> reserve(std::function<void()> callback) override;
    void drain();
    void setAvailable(bool available);
private:
    struct State;
    std::shared_ptr<State> state_;
};
