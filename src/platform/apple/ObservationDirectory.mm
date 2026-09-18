#include <QString>
#include <QtGlobal>
#include <QDir>
#import <Foundation/Foundation.h>

namespace iiSocietyHelper {
#ifdef Q_OS_IOS
bool applePrepareDeliveryDirectory(const QString &path, QString *error)
{
    @autoreleasepool {
        NSFileManager *manager = NSFileManager.defaultManager;
        // Keep the shared WAL and its sidecars in one protection class so a
        // locked device cannot make half of a committed database inaccessible.
        NSMutableArray<NSString *> *paths = [NSMutableArray arrayWithObjects:
            path.toNSString(), QDir(path).absoluteFilePath("..").toNSString(), nil];
        for (NSString *name in @[@"delivery.sqlite", @"delivery.sqlite-wal", @"delivery.sqlite-shm"]) {
            NSString *file = [path.toNSString() stringByAppendingPathComponent:name];
            if ([manager fileExistsAtPath:file]) [paths addObject:file];
        }
        for (NSString *file in paths) {
            // Never follow a link while applying attributes to existing files.
            NSError *failure = nil;
            NSDictionary *attributes = [manager attributesOfItemAtPath:file error:&failure];
            if (!attributes || [attributes[NSFileType] isEqual:NSFileTypeSymbolicLink]
                || ![manager setAttributes:@{NSFileProtectionKey: NSFileProtectionCompleteUntilFirstUserAuthentication}
                              ofItemAtPath:file error:&failure]) {
                if (error) *error = failure ? QString::fromNSString(failure.localizedDescription)
                    : QStringLiteral("Society delivery files cannot be redirected.");
                return false;
            }
        }
        return true;
    }
}
#endif
QString appleObservationDirectory(QString *error)
{
    @autoreleasepool {
        // Reuse the existing Society storage entitlement and Info.plist contract.
        NSString *group = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"SocietyAppGroup"];
        if (group.length) {
            NSURL *container = [[NSFileManager defaultManager] containerURLForSecurityApplicationGroupIdentifier:group];
            if (container) return QString::fromNSString([[container URLByAppendingPathComponent:
                @"Library/Application Support/iiSocietyHelper/v1" isDirectory:YES] path]);
            if (error) *error = QStringLiteral("The Society App Group is not accessible to this app.");
            return {};
        }
#if defined(Q_OS_IOS)
        if (error) *error = QStringLiteral("iOS observation requires SocietyAppGroup and its App Group entitlement.");
#else
        if (qEnvironmentVariableIsSet("APP_SANDBOX_CONTAINER_ID") && error)
            *error = QStringLiteral("Sandboxed apps need the same Society App Group for mutual observation.");
#endif
        return {};
    }
}
}
