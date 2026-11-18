#include <iiSocietyHelper/GroupSessionStore.h>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QStandardPaths>
#include <QUuid>
#include "../CTestSupport.h"
#include "../QtEventWait.h"
#include "../TestWorkspace.h"
#include <atomic>
#include <memory>
#include <QThread>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QScopeGuard>
#include <SocietyDrive.h>
#import <Security/Security.h>

iisacc::accounts::SessionStore *societyAppleStateKeyStore(QObject *parent);

class AppleStateTests {
public:
    void adHocProcessesShareTheSelectedContainerStateLocation() {
        // This executable deliberately has the linker's ad-hoc signature and
        // no sandbox entitlement. It must never address a protected App Group.
        SecCodeRef code = nullptr;
        CTEST_COMPARE(SecCodeCopySelf(kSecCSDefaultFlags, &code), errSecSuccess);
        CFDictionaryRef information = nullptr;
        const auto status = SecCodeCopySigningInformation(code, kSecCSSigningInformation, &information);
        CFRelease(code);
        CTEST_COMPARE(status, errSecSuccess);
        const bool hasTeam = CFDictionaryContainsKey(information, kSecCodeInfoTeamIdentifier);
        CFRelease(information);
        CTEST_CHECK(!hasTeam);
        QTemporaryDir fixture(society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + "/apple-state-XXXXXX.societycontainer");
        CTEST_CHECK(fixture.isValid());
        CTEST_CHECK(iiSocietyContainer::SocietyDrive::create(fixture.path()));
        const auto previous = qgetenv("SOCIETY_CONTAINER_PATH");
        const auto cleanup = qScopeGuard([&] { previous.isNull() ? qunsetenv("SOCIETY_CONTAINER_PATH") : qputenv("SOCIETY_CONTAINER_PATH", previous); });
        qputenv("SOCIETY_CONTAINER_PATH", fixture.path().toUtf8());
        const auto expected = fixture.filePath(".society-runtime/state");
        CTEST_COMPARE(GroupSessionStore::defaultDirectory(), expected);
        const auto priorName = QCoreApplication::applicationName();
        const auto restoreName = qScopeGuard([&] { QCoreApplication::setApplicationName(priorName); });
        QCoreApplication::setApplicationName("SocietyDaemon");
        CTEST_COMPARE(GroupSessionStore::defaultDirectory(), expected);
        QCoreApplication::setApplicationName("Society");
        CTEST_COMPARE(GroupSessionStore::defaultDirectory(), expected);
        CTEST_CHECK(!QFileInfo::exists(expected)); // Path lookup creates no runtime state.
    }
    void nativeKeychainRoundTrip() {
        // Only a disposable, noncredential item is touched. The encrypted file
        // regression suite separately exercises restart and revocation races.
        std::unique_ptr<iisacc::accounts::SessionStore> keys(societyAppleStateKeyStore(nullptr));
        const auto key = "native-storage-probe-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QByteArray fixture("Synthetic Society storage verification");
        using Store = iisacc::accounts::SessionStore;
        struct CallbackState {
            std::atomic_bool done{false};
            std::atomic_bool callbackOnOwner{true};
            Store::Result result;
        };
        const auto state = std::make_shared<CallbackState>();
        const auto* owner = QThread::currentThread();
        const auto receive = [state, owner](auto value) {
            if (QThread::currentThread() != owner) state->callbackOnOwner.store(false);
            state->result = std::move(value);
            state->done.store(true, std::memory_order_release);
        };
        const auto finished = [&] { return state->done.load(std::memory_order_acquire); };
        keys->write(key, fixture, receive);
        CTEST_CHECK(!finished());
        CTEST_WAIT_VERIFY(finished(), 10000);
        CTEST_CHECK(state->callbackOnOwner.load());
        CTEST_COMPARE(state->result.error, Store::Error::None);
        state->done.store(false);
        keys->read(key, receive);
        CTEST_CHECK(!finished());
        CTEST_WAIT_VERIFY(finished(), 10000);
        CTEST_CHECK(state->callbackOnOwner.load());
        const bool matched = state->result.error == Store::Error::None && state->result.data == fixture;
        state->done.store(false);
        keys->remove(key, receive);
        CTEST_WAIT_VERIFY(finished(), 10000);
        CTEST_CHECK(state->callbackOnOwner.load());
        CTEST_COMPARE(state->result.error, Store::Error::None);
        CTEST_CHECK(matched);
        state->done.store(false);
        keys->read(key, receive);
        CTEST_CHECK(!finished());
        CTEST_WAIT_VERIFY(finished(), 10000);
        CTEST_CHECK(state->callbackOnOwner.load());
        CTEST_COMPARE(state->result.error, Store::Error::Missing);
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    helper_ctest::CTestRunner runner(argc == 2 ? argv[1] : "");
    AppleStateTests tests;
    struct FlushDeferredDeletes {
        ~FlushDeferredDeletes() { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }
    };
    runner.run("adHocProcessesShareTheSelectedContainerStateLocation", [&] { FlushDeferredDeletes cleanup; tests.adHocProcessesShareTheSelectedContainerStateLocation(); });
    runner.run("nativeKeychainRoundTrip", [&] { FlushDeferredDeletes cleanup; tests.nativeKeychainRoundTrip(); });
    return runner.result();
}
