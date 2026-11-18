#include "TestWorkspace.h"
#include "iiSocietyHelper.h"
#include <iiAccountManager/AccountManager.h>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include "QtSignalCounter.h"
#include <QTemporaryDir>
#include "CTestSupport.h"
#include <QCoreApplication>
#include <QThread>
#include <iostream>

using iiSocietyHelper::Helper;
using iisacc::accounts::AccountManager;

namespace {
QVariantMap profile(const QString &userId = "@example_author")
{
    return {{"sub", "helper-author-001"}, {"email", "author@example.org"},
        {"displayName", "Example Author"}, {"userId", userId}, {"avatarUrl", QVariant::fromValue(nullptr)},
        {"passwordEnabled", true}, {"emailMarketingConsent", true},
        {"termsAcceptedAt", "2026-09-09T00:00:00.123Z"}, {"societyCloudMembership", "Pro"},
        {"authorDetails", QVariantMap{{"fullName", "Example Author"}, {"organization", "Example Studio"},
            {"links", QVariantList{QVariantMap{{"relation", "portfolio"}, {"label", "Work"}, {"url", "https://example.org/"}}}},
            {"identifiers", QVariantList{QVariantMap{{"scheme", "studio-id"}, {"value", "author-001"}}}}}}};
}
}

class AccountReferenceTests final {
public:
    void borrowingAndDetachingPreserveOwnership()
    {
        QObject owner;
        AccountManager manager(&owner);
        Helper helper;
        helper_ctest::SignalCounter managers(&helper, &Helper::accountManagerChanged);
        helper_ctest::SignalCounter accounts(&helper, &Helper::accountChanged);
        CTEST_CHECK(!helper.accountManager());
        CTEST_CHECK(!helper.account());
        CTEST_CHECK(helper.setAccountManager(nullptr));
        CTEST_COMPARE(managers.size(), 0);
        CTEST_CHECK(helper.setAccountManager(&manager));
        CTEST_COMPARE(helper.accountManager(), &manager);
        CTEST_COMPARE(helper.account(), manager.account());
        CTEST_COMPARE(manager.parent(), &owner);
        CTEST_COMPARE(managers.size(), 1);
        CTEST_COMPARE(accounts.size(), 1);
        CTEST_CHECK(!helper.account()->isPresent());
        CTEST_CHECK(!helper.isRunning());
        CTEST_CHECK(helper.setAccountManager(&manager));
        CTEST_COMPARE(managers.size(), 1);
        CTEST_COMPARE(accounts.size(), 1);
        CTEST_CHECK(helper.setAccountManager(nullptr));
        CTEST_CHECK(!helper.account());
        CTEST_CHECK(!helper.accountManager());
        CTEST_COMPARE(managers.size(), 2);
        CTEST_COMPARE(accounts.size(), 2);
        CTEST_CHECK(manager.readAccount(profile()));
        CTEST_COMPARE(accounts.size(), 2);
    }

    void completeModelAndChangesUseTheSameObjects()
    {
        AccountManager manager;
        Helper helper;
        CTEST_CHECK(helper.setAccountManager(&manager));
        auto *account = manager.account();
        auto *author = account->authorDetails();
        helper_ctest::SignalCounter changes(&helper, &Helper::accountChanged);
        bool consistentNotifications = true;
        QObject notificationContext;
        QObject::connect(&helper, &Helper::accountChanged, &notificationContext, [&] {
            consistentNotifications = consistentNotifications && helper.account() == account;
            if (helper.account()) {
                consistentNotifications = consistentNotifications && helper.account()->authorDetails() == author;
                consistentNotifications = consistentNotifications && helper.account()->toVariantMap() == manager.account()->toVariantMap();
            }
        });
        CTEST_CHECK(manager.readAccount(profile()));
        CTEST_COMPARE(helper.account()->userId(), "@example_author");
        CTEST_COMPARE(helper.account()->toVariantMap().size(), 11);
        CTEST_CHECK(helper.account()->toVariantMap().contains("societyContainerDrive"));
        CTEST_CHECK(helper.account()->toVariantMap().value("societyContainerDrive").isNull());
        CTEST_COMPARE(helper.account()->authorDetails()->toVariantMap().size(), 20);
        CTEST_COMPARE(helper.account()->authorDetails()->links().first().url, QUrl("https://example.org/"));
        CTEST_COMPARE(helper.account()->authorDetails()->identifiers().first().value, "author-001");
        CTEST_CHECK(helper.account()->passwordEnabled());
        CTEST_CHECK(helper.account()->emailMarketingConsent());
        CTEST_CHECK(helper.account()->termsAcceptedAt().isValid());
        CTEST_COMPARE(changes.size(), 1);
        CTEST_CHECK(manager.readAccount(profile()));
        CTEST_COMPARE(changes.size(), 1);
        auto changed = profile();
        auto details = changed["authorDetails"].toMap();
        details["organization"] = "Updated Studio";
        changed["authorDetails"] = details;
        CTEST_CHECK(manager.readAccount(changed));
        CTEST_COMPARE(changes.size(), 2);
        CTEST_COMPARE(author->organization(), "Updated Studio");
        manager.clear();
        CTEST_COMPARE(changes.size(), 3);
        CTEST_CHECK(!helper.account()->isPresent());
        CTEST_CHECK(helper.account()->authorDetails()->links().isEmpty());
        CTEST_CHECK(manager.readAccount(profile()));
        CTEST_CHECK(!manager.readJson("invalid"));
        CTEST_COMPARE(helper.accountManager()->state(), AccountManager::State::Failed);
        CTEST_CHECK(helper.account()->toVariantMap().isEmpty());
        CTEST_CHECK(helper.account()->authorDetails()->organization().isEmpty());
        CTEST_CHECK(consistentNotifications);
    }

    void replacementDisconnectsOldManagerAndDestructionClearsBothReferences()
    {
        auto first = std::make_unique<AccountManager>();
        auto second = std::make_unique<AccountManager>();
        Helper helper;
        CTEST_CHECK(helper.setAccountManager(first.get()));
        CTEST_CHECK(helper.setAccountManager(second.get()));
        helper_ctest::SignalCounter managers(&helper, &Helper::accountManagerChanged);
        helper_ctest::SignalCounter accounts(&helper, &Helper::accountChanged);
        CTEST_CHECK(first->readAccount(profile()));
        first.reset();
        CTEST_COMPARE(accounts.size(), 0);
        CTEST_COMPARE(managers.size(), 0);
        CTEST_COMPARE(helper.accountManager(), second.get());
        CTEST_CHECK(second->readAccount(profile("@second_author")));
        CTEST_COMPARE(accounts.size(), 1);
        bool detachedWhenNotified = false;
        QObject notificationContext;
        QObject::connect(&helper, &Helper::accountManagerChanged, &notificationContext, [&] {
            detachedWhenNotified = !helper.accountManager() && !helper.account();
        });
        second.reset();
        CTEST_CHECK(detachedWhenNotified);
        CTEST_COMPARE(accounts.size(), 2);
        CTEST_COMPARE(managers.size(), 1);
        CTEST_CHECK(!helper.account());
        CTEST_CHECK(!helper.accountManager());
    }

    void helpersShareTheManagerWithoutOwningOrResettingIt()
    {
        AccountManager manager;
        CTEST_CHECK(manager.readAccount(profile()));
        Helper second;
        {
            Helper first;
            CTEST_CHECK(first.setAccountManager(&manager));
            CTEST_CHECK(second.setAccountManager(&manager));
            CTEST_COMPARE(first.account(), second.account());
            first.stop();
            CTEST_CHECK(first.account()->isPresent());
        }
        CTEST_CHECK(manager.account()->isPresent());
        CTEST_COMPARE(second.account(), manager.account());
        CTEST_COMPARE(manager.state(), AccountManager::State::Ready);
        CTEST_CHECK(manager.readAccount(profile("@still_present")));
        CTEST_COMPARE(second.account()->userId(), "@still_present");
    }

    void foreignThreadManagerIsRejectedWithoutChangingTheBinding()
    {
        AccountManager local;
        Helper helper;
        CTEST_CHECK(helper.setAccountManager(&local));
        helper_ctest::SignalCounter changes(&helper, &Helper::accountChanged);
        QThread worker;
        auto *foreign = new AccountManager;
        foreign->moveToThread(&worker);
        QObject::connect(&worker, &QThread::finished, foreign, &QObject::deleteLater);
        worker.start();
        const bool accepted = helper.setAccountManager(foreign);
        worker.quit();
        CTEST_CHECK(worker.wait(5000));
        CTEST_CHECK(!accepted);
        CTEST_COMPARE(helper.accountManager(), &local);
        CTEST_COMPARE(changes.size(), 0);
    }

    void replacingManagerFromNotificationSuppressesStaleAccountNotification()
    {
        AccountManager first, second;
        Helper helper;
        helper_ctest::SignalCounter accounts(&helper, &Helper::accountChanged);
        bool replaced = false;
        bool consistentNotifications = true;
        QObject notificationContext;
        QObject::connect(&helper, &Helper::accountManagerChanged, &notificationContext, [&] {
            if (helper.accountManager() == &first) replaced = helper.setAccountManager(&second);
        });
        QObject::connect(&helper, &Helper::accountChanged, &notificationContext, [&] {
            consistentNotifications = consistentNotifications && helper.accountManager() == &second;
            consistentNotifications = consistentNotifications && helper.account() == second.account();
        });
        CTEST_CHECK(!helper.setAccountManager(&first));
        CTEST_CHECK(replaced);
        CTEST_CHECK(consistentNotifications);
        CTEST_COMPARE(accounts.size(), 1);
        CTEST_COMPARE(helper.accountManager(), &second);
    }

    void destroyingObjectsFromBindingNotificationIsSafe()
    {
        AccountManager manager;
        QPointer<Helper> helper = new Helper;
        QObject::connect(helper, &Helper::accountManagerChanged, &manager, [&] { delete helper.data(); });
        CTEST_CHECK(!helper->setAccountManager(&manager));
        CTEST_CHECK(!helper);
        CTEST_CHECK(manager.readAccount(profile()));

        Helper surviving;
        auto temporary = std::make_unique<AccountManager>();
        helper_ctest::SignalCounter accounts(&surviving, &Helper::accountChanged);
        QObject::connect(&surviving, &Helper::accountManagerChanged, &surviving, [&] {
            if (surviving.accountManager()) temporary.reset();
        });
        CTEST_CHECK(!surviving.setAccountManager(temporary.get()));
        CTEST_CHECK(!surviving.accountManager());
        CTEST_CHECK(!surviving.account());
        CTEST_COMPARE(accounts.size(), 1);
    }

    void qmlReadsTypedReferencesAndTracksProfileAndDestruction()
    {
        Helper helper;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("societyHelper", &helper);
        QQmlExpression user(engine.rootContext(), nullptr,
            "societyHelper.account ? societyHelper.account.userId : ''");
        user.setNotifyOnValueChanged(true);
        CTEST_COMPARE(user.evaluate().toString(), "");
        CTEST_CHECK_MESSAGE(!user.hasError(), qPrintable(user.error().toString()));
        helper_ctest::SignalCounter changes(&user, &QQmlExpression::valueChanged);
        auto manager = std::make_unique<AccountManager>();
        CTEST_CHECK(helper.setAccountManager(manager.get()));
        CTEST_COMPARE(user.evaluate().toString(), "");
        CTEST_CHECK(manager->readAccount(profile()));
        CTEST_COMPARE(user.evaluate().toString(), "@example_author");
        QQmlExpression details(engine.rootContext(), nullptr,
            "societyHelper.accountManager.account === societyHelper.account && "
            "societyHelper.account.authorDetails.organization === 'Example Studio' && "
            "societyHelper.account.authorDetails.links[0].relation === 'portfolio'");
        CTEST_CHECK_MESSAGE(details.evaluate().toBool(), qPrintable(details.error().toString()));
        CTEST_CHECK(manager->readAccount(profile("@updated_author")));
        CTEST_COMPARE(user.evaluate().toString(), "@updated_author");
        manager.reset();
        CTEST_COMPARE(user.evaluate().toString(), "");
        CTEST_CHECK(!user.hasError());
        CTEST_CHECK(changes.size() >= 4);
    }

    void observationDoesNotPublishTheReferencedAccount()
    {
        std::cout << "account observation: create fixture\n" << std::flush;
        QTemporaryDir directory((society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + QStringLiteral("/account-presence-XXXXXX")));
        CTEST_CHECK(directory.isValid());
        AccountManager manager;
        Helper helper;
        CTEST_CHECK(manager.readAccount(profile()));
        CTEST_CHECK(helper.setAccountManager(&manager));
        helper_ctest::SignalCounter queued(&helper, &Helper::dataQueued);
        CTEST_CHECK(helper.start({"com.iisacc.account.reference", "Account reference", "0.6.0"}, {directory.path(), 50, 400}));
        std::cout << "account observation: started\n" << std::flush;
        CTEST_COMPARE(queued.size(), 1);
        CTEST_CHECK(manager.readAccount(profile("@updated_author")));
        manager.clear();
        CTEST_COMPARE(queued.size(), 1);
        QFile record(directory.filePath(helper.self().instanceId + ".json"));
        CTEST_CHECK(record.open(QIODevice::ReadOnly));
        const auto object = QJsonDocument::fromJson(record.readAll()).object();
        const auto expected = QStringList{"activity", "applicationId", "heartbeat", "instanceId", "name",
            "processId", "protocol", "startedAt", "version"};
        CTEST_COMPARE(object.keys(), expected);
        std::cout << "account observation: stopping\n" << std::flush;
        helper.stop();
        std::cout << "account observation: stopped\n" << std::flush;
        CTEST_COMPARE(helper.accountManager(), &manager);
    }
};

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    helper_ctest::CTestRunner runner;
    AccountReferenceTests tests;
    runner.run("borrowingAndDetachingPreserveOwnership", [&] { tests.borrowingAndDetachingPreserveOwnership(); });
    runner.run("completeModelAndChangesUseTheSameObjects", [&] { tests.completeModelAndChangesUseTheSameObjects(); });
    runner.run("replacementDisconnectsOldManagerAndDestructionClearsBothReferences", [&] { tests.replacementDisconnectsOldManagerAndDestructionClearsBothReferences(); });
    runner.run("helpersShareTheManagerWithoutOwningOrResettingIt", [&] { tests.helpersShareTheManagerWithoutOwningOrResettingIt(); });
    runner.run("foreignThreadManagerIsRejectedWithoutChangingTheBinding", [&] { tests.foreignThreadManagerIsRejectedWithoutChangingTheBinding(); });
    runner.run("replacingManagerFromNotificationSuppressesStaleAccountNotification", [&] { tests.replacingManagerFromNotificationSuppressesStaleAccountNotification(); });
    runner.run("destroyingObjectsFromBindingNotificationIsSafe", [&] { tests.destroyingObjectsFromBindingNotificationIsSafe(); });
    runner.run("qmlReadsTypedReferencesAndTracksProfileAndDestruction", [&] { tests.qmlReadsTypedReferencesAndTracksProfileAndDestruction(); });
    runner.run("observationDoesNotPublishTheReferencedAccount", [&] {
        tests.observationDoesNotPublishTheReferencedAccount();
        std::cout << "account observation: fixture released\n" << std::flush;
    });
    return runner.result();
}
