#pragma once
#include "Export.h"
#include <QObject>
#include <QJsonObject>
#include <Generation/NativeDiffusion.hpp>
#include <memory>

namespace iiSocietyHelper::Generation {
using Engine = std::function<iiLocalDiffusion::NativeGenerationResult(
    const iiLocalDiffusion::NativeGenerationRequest &, const iiLocalDiffusion::NativeGenerationOptions &,
    const std::atomic_bool &, const iiLocalDiffusion::NativeProgressCallback &)>;
// The caller authenticates the peer and account before handle(). The service
// further confines model references to its container and jobs to their owner.
class IISOCIETYHELPER_GENERATION_EXPORT Host final : public QObject {
    Q_OBJECT
public:
    explicit Host(Engine engine = {}, QObject *parent = nullptr);
    ~Host() override;
    bool open(const QString &container);
    void close();
    // Final shutdown: request cancellation and detach the in-flight engine
    // without waiting. Engine callbacks and captured state must remain valid
    // until the engine returns. A retired host cannot be reopened.
    void shutdownAsync();
    QJsonObject handle(const QString &authenticatedPeer, const QJsonObject &request);
private:
    class Private;
    std::unique_ptr<Private> d;
};
}
