#include <QString>
#include <QDir>
#include <QStandardPaths>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <iiAcountManager/SessionStore.h>
#import <Foundation/Foundation.h>
#import <Security/Security.h>
#include <TargetConditionals.h>

namespace {
NSString *groupIdentifier() {
    NSString *group = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"SocietyAppGroup"];
    return group.length ? group : @"group.com.iisacc.society";
}
#if TARGET_OS_OSX
enum class StateLocation { Unavailable, AppGroup, Development };
StateLocation stateLocation() {
    SecCodeRef code = nullptr;
    if (SecCodeCopySelf(kSecCSDefaultFlags, &code) != errSecSuccess) return StateLocation::Unavailable;
    CFDictionaryRef information = nullptr;
    const auto status = SecCodeCopySigningInformation(code, kSecCSSigningInformation, &information);
    CFRelease(code);
    if (status != errSecSuccess) return StateLocation::Unavailable;
    NSDictionary *signing = CFBridgingRelease(information);
    NSDictionary *entitlements = signing[(__bridge id)kSecCodeInfoEntitlementsDict];
    // A URL from NSFileManager does not prove permission to a team-owned group.
    // Unsandboxed local builds have no developer team. Give their encrypted
    // state a distinct, stable location shared by the app and its helper.
    if (!signing[(__bridge id)kSecCodeInfoTeamIdentifier]
        && ![entitlements[@"com.apple.security.app-sandbox"] boolValue])
        return StateLocation::Development;
    return StateLocation::AppGroup;
}
#endif
// The wrapping key has no app-sandbox restore marker. iOS uses the App Group's
// Keychain access group, so reinstalling one participating app cannot orphan it.
class AppleGroupKeyStore final : public iisacc::accounts::SessionStore {
public:
    using SessionStore::SessionStore;
    NSMutableDictionary *query(const QString &key) {
        NSString *service = [groupIdentifier() stringByAppendingString:@".encrypted-state"];
#if TARGET_OS_OSX
        if (stateLocation() == StateLocation::Development)
            service = [groupIdentifier() stringByAppendingString:@".development.encrypted-state"];
#endif
        NSMutableDictionary *result = [@{(__bridge id)kSecClass:(__bridge id)kSecClassGenericPassword,
            (__bridge id)kSecAttrService:service,
            (__bridge id)kSecAttrAccount:key.toNSString()} mutableCopy];
#if TARGET_OS_IPHONE
        result[(__bridge id)kSecAttrAccessGroup] = groupIdentifier();
#endif
        return result;
    }
    void run(std::function<Result()> operation, Completion done) {
        auto *watcher = new QFutureWatcher<Result>(this);
        connect(watcher, &QFutureWatcher<Result>::finished, this, [watcher, done] {
            const auto result = watcher->result(); watcher->deleteLater(); done(result);
        });
        watcher->setFuture(QtConcurrent::run(std::move(operation)));
    }
    static Result readQuery(NSMutableDictionary *q) {
        q[(__bridge id)kSecReturnData] = @YES;
        q[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
#if TARGET_OS_IPHONE
        q[(__bridge id)kSecUseAuthenticationUI] = (__bridge id)kSecUseAuthenticationUIFail;
#endif
        CFTypeRef value = nullptr;
        const auto status = SecItemCopyMatching((__bridge CFDictionaryRef)q, &value);
        if (status != errSecSuccess && status != errSecItemNotFound) qWarning("Society group key read failed: %d", int(status));
        NSData *data = CFBridgingRelease(value);
        return {status == errSecSuccess ? Error::None : status == errSecItemNotFound ? Error::Missing : Error::Unavailable,
            status == errSecSuccess ? QByteArray(static_cast<const char *>(data.bytes), data.length) : QByteArray()};
    }
    void read(const QString &key, Completion done) override {
        auto *q = query(key);
        run([q] { @autoreleasepool { return readQuery(q); } }, std::move(done));
    }
    void write(const QString &key, const QByteArray &data, Completion done) override {
        auto *q = query(key);
        run([q, data] {
            @autoreleasepool {
                auto *insert = [q mutableCopy];
                insert[(__bridge id)kSecValueData] = [NSData dataWithBytes:data.constData() length:data.size()];
#if TARGET_OS_IPHONE
                insert[(__bridge id)kSecAttrAccessible] = (__bridge id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly;
#endif
                const auto status = SecItemAdd((__bridge CFDictionaryRef)insert, nullptr);
                if (status == errSecDuplicateItem) {
                    const auto existing = readQuery(q);
                    return Result{existing.error == Error::None && existing.data == data ? Error::None : Error::Unavailable, {}};
                }
                if (status != errSecSuccess) qWarning("Society group key write failed: %d", int(status));
                return Result{status == errSecSuccess ? Error::None : Error::Unavailable, {}};
            }
        }, std::move(done));
    }
    void remove(const QString &key, Completion done) override {
        auto *q = query(key);
        run([q] { @autoreleasepool {
            const auto status = SecItemDelete((__bridge CFDictionaryRef)q);
            return Result{status == errSecSuccess || status == errSecItemNotFound ? Error::None : Error::Unavailable, {}};
        } }, std::move(done));
    }
};
}
iisacc::accounts::SessionStore *societyAppleStateKeyStore(QObject *parent) { return new AppleGroupKeyStore(parent); }

QString societyAppleStateDirectory() {
    @autoreleasepool {
#if TARGET_OS_OSX
        const auto location = stateLocation();
        if (location == StateLocation::Unavailable) return {};
        if (location == StateLocation::Development) {
            const auto base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
            const auto group = QString::fromNSString(groupIdentifier());
            if (base.isEmpty() || group.contains('/') || group == "." || group == "..") return {};
            return QDir(base).filePath("iisacc/Development Groups/" + group
                + "/Library/Application Support/SocietyState");
        }
#endif
        NSURL *root = [[NSFileManager defaultManager] containerURLForSecurityApplicationGroupIdentifier:groupIdentifier()];
        if (!root) return {};
        return QString::fromNSString([[[root URLByResolvingSymlinksInPath]
            URLByAppendingPathComponent:@"Library/Application Support/SocietyState" isDirectory:YES] path]);
    }
}
bool societyProtectStatePath(const QString &path) {
    @autoreleasepool {
        NSURL *url = [NSURL fileURLWithPath:path.toNSString()];
        NSError *error = nil;
        if (![url setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:&error]) return false;
#if TARGET_OS_IPHONE
        if (![[NSFileManager defaultManager] setAttributes:@{NSFileProtectionKey:NSFileProtectionCompleteUntilFirstUserAuthentication}
            ofItemAtPath:path.toNSString() error:&error]) return false;
#endif
        return true;
    }
}
