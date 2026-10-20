#pragma once
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
class Host final : public QObject {
    Q_OBJECT
public:
    explicit Host(Engine engine = {}, QObject *parent = nullptr);
    ~Host() override;
    bool open(const QString &container);
    void close();
    QJsonObject handle(const QString &authenticatedPeer, const QJsonObject &request);
private:
    class Private;
    std::unique_ptr<Private> d;
};
}
