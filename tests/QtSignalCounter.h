#pragma once

#include <QObject>
#include <stdexcept>

namespace helper_ctest {
class SignalCounter final {
public:
    template <typename Sender, typename Signal>
    SignalCounter(Sender* sender, Signal signal)
    {
        if (!QObject::connect(sender, signal, &receiver_, [this] { ++count_; }))
            throw std::runtime_error("Failed to connect signal counter");
    }

    [[nodiscard]] int size() const { return count_; }

private:
    int count_ = 0;
    QObject receiver_;
};
} // namespace helper_ctest
