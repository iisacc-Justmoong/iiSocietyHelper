#include "DeviceInfo.h"
#include <QCryptographicHash>
#import <UIKit/UIKit.h>

QVariantMap societyIosAccountDevice() {
    UIDevice *device = UIDevice.currentDevice;
    const auto vendor = QString::fromNSString(device.identifierForVendor.UUIDString);
    QVariantMap result{{"type", device.userInterfaceIdiom == UIUserInterfaceIdiomPad ? "tablet" : "phone"},
        {"name", QString::fromNSString(device.name)}};
    if (!vendor.isEmpty()) result.insert("id", QString::fromLatin1(QCryptographicHash::hash(
        "iisacc-ios-device-v1:" + vendor.toUtf8(), QCryptographicHash::Sha256).toHex()));
    return result;
}
