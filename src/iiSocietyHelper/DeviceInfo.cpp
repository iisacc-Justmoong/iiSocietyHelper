#include "DeviceInfo.h"
#include <QCryptographicHash>
#include <QSettings>
#include <QUuid>
#ifdef Q_OS_ANDROID
#include <QJniObject>
#include <QJniEnvironment>
#include <QCoreApplication>
#endif

#ifdef Q_OS_IOS
QVariantMap societyIosAccountDevice();
#endif

QVariantMap societyAccountDevice(QVariantMap device) {
#ifdef Q_OS_IOS
    const auto native = societyIosAccountDevice();
    for (auto it = native.cbegin(); it != native.cend(); ++it) device.insert(it.key(), it.value());
#elif defined(Q_OS_ANDROID)
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    const auto resolver = context.callObjectMethod("getContentResolver", "()Landroid/content/ContentResolver;");
    const auto key = QJniObject::fromString("android_id");
    const auto androidId = QJniObject::callStaticObjectMethod("android/provider/Settings$Secure", "getString",
        "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;", resolver.object(), key.object());
    if (androidId.isValid() && !androidId.toString().isEmpty())
        device.insert("id", QString::fromLatin1(QCryptographicHash::hash("iisacc-android-device-v1:" + androidId.toString().toUtf8(), QCryptographicHash::Sha256).toHex()));
    const auto resources = context.callObjectMethod("getResources", "()Landroid/content/res/Resources;");
    const auto configuration = resources.callObjectMethod("getConfiguration", "()Landroid/content/res/Configuration;");
    device.insert("type", configuration.getField<jint>("smallestScreenWidthDp") >= 600 ? "tablet" : "phone");
    QJniEnvironment environment;
    if (environment->ExceptionCheck()) environment->ExceptionClear();
#endif
    if (device.value("id").toString().isEmpty()) {
        // A random installation identity is used only when the OS has none.
        QSettings settings(QSettings::IniFormat, QSettings::UserScope, "iisacc", "AccountDevice");
        auto id = settings.value("installationId").toString();
        if (id.isEmpty()) {
            id = QString::fromLatin1(QCryptographicHash::hash(QUuid::createUuid().toRfc4122(), QCryptographicHash::Sha256).toHex());
            settings.setValue("installationId", id); settings.sync();
        }
        device.insert("id", id);
    }
    device.insert("appId", "com.iisacc.society");
    device.insert("appVersion", SOCIETY_APP_VERSION);
    return device;
}
