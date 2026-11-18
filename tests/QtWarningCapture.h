#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QtLogging>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace helper_ctest {
// qInstallMessageHandler is process-wide. This scoped fixture preserves the
// previous handler and serializes access from Qt worker threads.
class ExpectedWarning final {
public:
    explicit ExpectedWarning(QString message) : expected_(std::move(message))
    {
        std::lock_guard lock(mutex_);
        if (active_) throw std::logic_error("A warning capture is already active");
        active_ = this;
        previous_ = qInstallMessageHandler(handle);
    }
    ExpectedWarning(const ExpectedWarning&) = delete;
    ExpectedWarning& operator=(const ExpectedWarning&) = delete;
    ~ExpectedWarning()
    {
        std::lock_guard lock(mutex_);
        qInstallMessageHandler(previous_);
        active_ = nullptr;
    }
    [[nodiscard]] bool seen() const
    {
        std::lock_guard lock(mutex_);
        return count_ != 0;
    }
private:
    static void handle(QtMsgType type, const QMessageLogContext& context, const QString& message)
    {
        QtMessageHandler previous;
        {
            std::lock_guard lock(mutex_);
            if (active_ && type == QtWarningMsg && message == active_->expected_) {
                ++active_->count_;
                return;
            }
            previous = previous_;
        }
        if (previous) previous(type, context, message);
        else {
            const auto text = message.toLocal8Bit();
            std::fwrite(text.constData(), 1, static_cast<std::size_t>(text.size()), stderr);
            std::fputc('\n', stderr);
        }
    }
    QString expected_;
    unsigned count_ = 0;
    inline static std::mutex mutex_;
    inline static ExpectedWarning* active_ = nullptr;
    inline static QtMessageHandler previous_ = nullptr;
};
} // namespace helper_ctest
