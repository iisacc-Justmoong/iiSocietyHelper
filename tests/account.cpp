#include "iiSocietyHelper.h"
#include <iiAcountManager/AccountManager.h>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

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

class AccountReferenceTests final : public QObject {
    Q_OBJECT
private slots:
    void borrowingAndDetachingPreserveOwnership()
    {
        QObject owner;
        AccountManager manager(&owner);
        Helper helper;
        QSignalSpy managers(&helper, &Helper::accountManagerChanged);
        QSignalSpy accounts(&helper, &Helper::accountChanged);
        QVERIFY(!helper.accountManager());
        QVERIFY(!helper.account());
        QVERIFY(helper.setAccountManager(nullptr));
        QCOMPARE(managers.size(), 0);
        QVERIFY(helper.setAccountManager(&manager));
        QCOMPARE(helper.accountManager(), &manager);
        QCOMPARE(helper.account(), manager.account());
        QCOMPARE(manager.parent(), &owner);
        QCOMPARE(managers.size(), 1);
        QCOMPARE(accounts.size(), 1);
        QVERIFY(!helper.account()->isPresent());
        QVERIFY(!helper.isRunning());
        QVERIFY(helper.setAccountManager(&manager));
        QCOMPARE(managers.size(), 1);
        QCOMPARE(accounts.size(), 1);
        QVERIFY(helper.setAccountManager(nullptr));
        QVERIFY(!helper.account());
        QVERIFY(!helper.accountManager());
        QCOMPARE(managers.size(), 2);
        QCOMPARE(accounts.size(), 2);
        QVERIFY(manager.readAccount(profile()));
        QCOMPARE(accounts.size(), 2);
    }

    void completeModelAndChangesUseTheSameObjects()
    {
        AccountManager manager;
        Helper helper;
        QVERIFY(helper.setAccountManager(&manager));
        auto *account = manager.account();
        auto *author = account->authorDetails();
        QSignalSpy changes(&helper, &Helper::accountChanged);
        connect(&helper, &Helper::accountChanged, &helper, [&] {
            QCOMPARE(helper.account(), account);
            QCOMPARE(helper.account()->authorDetails(), author);
            QCOMPARE(helper.account()->toVariantMap(), manager.account()->toVariantMap());
        });
        QVERIFY(manager.readAccount(profile()));
        QCOMPARE(helper.account()->userId(), "@example_author");
        QCOMPARE(helper.account()->toVariantMap().size(), 10);
        QCOMPARE(helper.account()->authorDetails()->toVariantMap().size(), 20);
        QCOMPARE(helper.account()->authorDetails()->links().first().url, QUrl("https://example.org/"));
        QCOMPARE(helper.account()->authorDetails()->identifiers().first().value, "author-001");
        QVERIFY(helper.account()->passwordEnabled());
        QVERIFY(helper.account()->emailMarketingConsent());
        QVERIFY(helper.account()->termsAcceptedAt().isValid());
        QCOMPARE(changes.size(), 1);
        QVERIFY(manager.readAccount(profile()));
        QCOMPARE(changes.size(), 1);
        auto changed = profile();
        auto details = changed["authorDetails"].toMap();
        details["organization"] = "Updated Studio";
        changed["authorDetails"] = details;
        QVERIFY(manager.readAccount(changed));
        QCOMPARE(changes.size(), 2);
        QCOMPARE(author->organization(), "Updated Studio");
        manager.clear();
        QCOMPARE(changes.size(), 3);
        QVERIFY(!helper.account()->isPresent());
        QVERIFY(helper.account()->authorDetails()->links().isEmpty());
        QVERIFY(manager.readAccount(profile()));
        QVERIFY(!manager.readJson("invalid"));
        QCOMPARE(helper.accountManager()->state(), AccountManager::State::Failed);
        QVERIFY(helper.account()->toVariantMap().isEmpty());
        QVERIFY(helper.account()->authorDetails()->organization().isEmpty());
    }

    void replacementDisconnectsOldManagerAndDestructionClearsBothReferences()
    {
        auto first = std::make_unique<AccountManager>();
        auto second = std::make_unique<AccountManager>();
        Helper helper;
        QVERIFY(helper.setAccountManager(first.get()));
        QVERIFY(helper.setAccountManager(second.get()));
        QSignalSpy managers(&helper, &Helper::accountManagerChanged);
        QSignalSpy accounts(&helper, &Helper::accountChanged);
        QVERIFY(first->readAccount(profile()));
        first.reset();
        QCOMPARE(accounts.size(), 0);
        QCOMPARE(managers.size(), 0);
        QCOMPARE(helper.accountManager(), second.get());
        QVERIFY(second->readAccount(profile("@second_author")));
        QCOMPARE(accounts.size(), 1);
        connect(&helper, &Helper::accountManagerChanged, &helper, [&] {
            QVERIFY(!helper.accountManager());
            QVERIFY(!helper.account());
        });
        second.reset();
        QCOMPARE(accounts.size(), 2);
        QCOMPARE(managers.size(), 1);
        QVERIFY(!helper.account());
        QVERIFY(!helper.accountManager());
    }

    void helpersShareTheManagerWithoutOwningOrResettingIt()
    {
        AccountManager manager;
        QVERIFY(manager.readAccount(profile()));
        Helper second;
        {
            Helper first;
            QVERIFY(first.setAccountManager(&manager));
            QVERIFY(second.setAccountManager(&manager));
            QCOMPARE(first.account(), second.account());
            first.stop();
            QVERIFY(first.account()->isPresent());
        }
        QVERIFY(manager.account()->isPresent());
        QCOMPARE(second.account(), manager.account());
        QCOMPARE(manager.state(), AccountManager::State::Ready);
        QVERIFY(manager.readAccount(profile("@still_present")));
        QCOMPARE(second.account()->userId(), "@still_present");
    }

    void foreignThreadManagerIsRejectedWithoutChangingTheBinding()
    {
        AccountManager local;
        Helper helper;
        QVERIFY(helper.setAccountManager(&local));
        QSignalSpy changes(&helper, &Helper::accountChanged);
        QThread worker;
        auto *foreign = new AccountManager;
        foreign->moveToThread(&worker);
        connect(&worker, &QThread::finished, foreign, &QObject::deleteLater);
        worker.start();
        const bool accepted = helper.setAccountManager(foreign);
        worker.quit();
        QVERIFY(worker.wait(5000));
        QVERIFY(!accepted);
        QCOMPARE(helper.accountManager(), &local);
        QCOMPARE(changes.size(), 0);
    }

    void replacingManagerFromNotificationSuppressesStaleAccountNotification()
    {
        AccountManager first, second;
        Helper helper;
        QSignalSpy accounts(&helper, &Helper::accountChanged);
        connect(&helper, &Helper::accountManagerChanged, &helper, [&] {
            if (helper.accountManager() == &first) QVERIFY(helper.setAccountManager(&second));
        });
        connect(&helper, &Helper::accountChanged, &helper, [&] {
            QCOMPARE(helper.accountManager(), &second);
            QCOMPARE(helper.account(), second.account());
        });
        QVERIFY(!helper.setAccountManager(&first));
        QCOMPARE(accounts.size(), 1);
        QCOMPARE(helper.accountManager(), &second);
    }

    void destroyingObjectsFromBindingNotificationIsSafe()
    {
        AccountManager manager;
        QPointer<Helper> helper = new Helper;
        connect(helper, &Helper::accountManagerChanged, &manager, [&] { delete helper.data(); });
        QVERIFY(!helper->setAccountManager(&manager));
        QVERIFY(!helper);
        QVERIFY(manager.readAccount(profile()));

        Helper surviving;
        auto temporary = std::make_unique<AccountManager>();
        QSignalSpy accounts(&surviving, &Helper::accountChanged);
        connect(&surviving, &Helper::accountManagerChanged, &surviving, [&] {
            if (surviving.accountManager()) temporary.reset();
        });
        QVERIFY(!surviving.setAccountManager(temporary.get()));
        QVERIFY(!surviving.accountManager());
        QVERIFY(!surviving.account());
        QCOMPARE(accounts.size(), 1);
    }

    void qmlReadsTypedReferencesAndTracksProfileAndDestruction()
    {
        Helper helper;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("societyHelper", &helper);
        QQmlExpression user(engine.rootContext(), nullptr,
            "societyHelper.account ? societyHelper.account.userId : ''");
        user.setNotifyOnValueChanged(true);
        QCOMPARE(user.evaluate().toString(), "");
        QVERIFY2(!user.hasError(), qPrintable(user.error().toString()));
        QSignalSpy changes(&user, &QQmlExpression::valueChanged);
        auto manager = std::make_unique<AccountManager>();
        QVERIFY(helper.setAccountManager(manager.get()));
        QCOMPARE(user.evaluate().toString(), "");
        QVERIFY(manager->readAccount(profile()));
        QCOMPARE(user.evaluate().toString(), "@example_author");
        QQmlExpression details(engine.rootContext(), nullptr,
            "societyHelper.accountManager.account === societyHelper.account && "
            "societyHelper.account.authorDetails.organization === 'Example Studio' && "
            "societyHelper.account.authorDetails.links[0].relation === 'portfolio'");
        QVERIFY2(details.evaluate().toBool(), qPrintable(details.error().toString()));
        QVERIFY(manager->readAccount(profile("@updated_author")));
        QCOMPARE(user.evaluate().toString(), "@updated_author");
        manager.reset();
        QCOMPARE(user.evaluate().toString(), "");
        QVERIFY(!user.hasError());
        QVERIFY(changes.size() >= 4);
    }

    void observationDoesNotPublishTheReferencedAccount()
    {
        QTemporaryDir directory(QStringLiteral(HELPER_TEST_DIRECTORY "/account-presence-XXXXXX"));
        QVERIFY(directory.isValid());
        AccountManager manager;
        Helper helper;
        QVERIFY(manager.readAccount(profile()));
        QVERIFY(helper.setAccountManager(&manager));
        QSignalSpy queued(&helper, &Helper::dataQueued);
        QVERIFY(helper.start({"com.iisacc.account.reference", "Account reference", "0.6.0"}, {directory.path(), 50, 400}));
        QCOMPARE(queued.size(), 1);
        QVERIFY(manager.readAccount(profile("@updated_author")));
        manager.clear();
        QCOMPARE(queued.size(), 1);
        QFile record(directory.filePath(helper.self().instanceId + ".json"));
        QVERIFY(record.open(QIODevice::ReadOnly));
        const auto object = QJsonDocument::fromJson(record.readAll()).object();
        const auto expected = QStringList{"activity", "applicationId", "heartbeat", "instanceId", "name",
            "processId", "protocol", "startedAt", "version"};
        QCOMPARE(object.keys(), expected);
        helper.stop();
        QCOMPARE(helper.accountManager(), &manager);
    }
};

QTEST_GUILESS_MAIN(AccountReferenceTests)
#include "account.moc"
