#include "DiscoveryService.h"
#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>
#include <QJsonDocument>
#include <QPointer>

namespace {
constexpr char javaClass[] = "com/iisacc/society/SocietyDiscovery";
QHash<jlong, QPointer<DiscoveryService>> services;
jlong nextId = 0;
void discoveryEvent(JNIEnv *, jclass, jlong id, jstring message) {
    const auto bytes = QJniObject(message).toString().toUtf8();
    QMetaObject::invokeMethod(qApp, [id, bytes] {
        auto service = services.value(id); if (!service || bytes.size() > 4096) return;
        const auto data = QJsonDocument::fromJson(bytes).object(); const auto op = data.value("op").toString();
        if (op == "found") emit service->found(data.value("key").toString(), data.value("record").toObject(),
            QHostAddress(data.value("address").toString()), quint16(data.value("port").toInt()));
        else if (op == "lost") emit service->lost(data.value("key").toString());
        else if (op == "error") emit service->failed(data.value("message").toString());
    }, Qt::QueuedConnection);
}
class AndroidDiscovery final : public DiscoveryService {
    QJniObject bridge;
    jlong id = 0;
public:
    using DiscoveryService::DiscoveryService;
    ~AndroidDiscovery() override { stop(); }
    void start(const QJsonObject &record, quint16 port) override {
        stop(); QJniEnvironment env;
        static const bool registered = env.registerNativeMethods(javaClass, {{"event", "(JLjava/lang/String;)V", reinterpret_cast<void *>(discoveryEvent)}});
        const auto context = QNativeInterface::QAndroidApplication::context();
        if (!registered || !context.isValid()) { emit failed(tr("Local device discovery could not start.")); return; }
        id = ++nextId; services.insert(id, this);
        bridge = QJniObject(javaClass, "(Landroid/content/Context;J)V", context.object(), id);
        if (bridge.isValid()) bridge.callMethod<void>("start", "(Ljava/lang/String;I)V",
            QJniObject::fromString(QString::fromUtf8(QJsonDocument(record).toJson(QJsonDocument::Compact))).object(), jint(port));
        if (!bridge.isValid() || env.checkAndClearExceptions()) { stop(); emit failed(tr("Local device discovery could not start.")); }
    }
    void stop() override {
        services.remove(id); id = 0;
        if (bridge.isValid()) bridge.callMethod<void>("stop");
        bridge = {}; QJniEnvironment env; env.checkAndClearExceptions();
    }
};
}
DiscoveryService *DiscoveryService::createNetwork(QObject *parent) { return new AndroidDiscovery(parent); }
