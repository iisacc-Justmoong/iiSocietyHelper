#include "TestWorkspace.h"
#include "iiSocietyHelper.h"
#include "DeliveryStore.h"
#include "ObjectTestTypes.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include "QtSignalCounter.h"
#include <QTemporaryDir>
#include "CTestSupport.h"
#include <QCoreApplication>
#include <QThread>

using namespace iiSocietyHelper;

class ObjectTests : public QObject {
public:
    void initTestCase() { qRegisterMetaType<GenerationJob>(); }

    void valueTypesSurviveJsonQueueEncodingExactly()
    {
        QString error = "old error";
        const auto job = sampleJob();
        auto payload = ObjectCodec::encode(QVariant::fromValue(job), &error);
        CTEST_CHECK_MESSAGE(!payload.isEmpty(), qPrintable(error));
        CTEST_CHECK(error.isEmpty());
        payload = QJsonDocument::fromJson(QJsonDocument(QJsonObject::fromVariantMap(payload)).toJson()).object().toVariantMap();
        const auto value = ObjectCodec::decode(payload, &error);
        CTEST_CHECK_MESSAGE(error.isEmpty(), qPrintable(error));
        CTEST_COMPARE(value.metaType(), QMetaType::fromType<GenerationJob>());
        CTEST_COMPARE(value.value<GenerationJob>(), job);
        const QVariantMap map{{"largeSigned", job.frame}, {"largeUnsigned", job.seed}, {"bytes", job.data},
            {"date", job.createdAt}, {"url", job.source}, {"nested", QVariantList{true, 1.25, QVariant::fromValue(job)}}};
        CTEST_COMPARE(ObjectCodec::decode(ObjectCodec::encode(map, &error), &error).toMap(), map);
        CTEST_CHECK(error.isEmpty());
    }

    void qobjectSnapshotsCaptureNestedStoredPropertiesWithoutPointers()
    {
        TransferObject root, child;
        root.detail = &child;
        child.title = "nested";
        root.setObjectName("runtime name");
        root.setProperty("dynamicSecret", "not-declared");
        QString error;
        const auto payload = ObjectCodec::encode(&root, &error);
        CTEST_CHECK_MESSAGE(error.isEmpty(), qPrintable(error));
        root.title = "modified";
        child.title = "modified";
        const auto snapshot = ObjectCodec::decode(payload, &error).value<ObjectSnapshot>();
        CTEST_COMPARE(snapshot.className, "TransferObject");
        CTEST_COMPARE(snapshot.properties.size(), 4);
        CTEST_COMPARE(snapshot.properties.value("title"), "original");
        CTEST_COMPARE(snapshot.properties.value("count").toLongLong(), 9007199254740993LL);
        CTEST_COMPARE(snapshot.properties.value("bytes").toByteArray(), QByteArray::fromHex("00ff80"));
        const auto nested = snapshot.properties.value("detail").value<ObjectSnapshot>();
        CTEST_COMPARE(nested.properties.value("title"), "nested");
        CTEST_CHECK(nested.properties.value("detail").isNull());
        CTEST_CHECK(error.isEmpty());
    }

    void accountUsesItsCompleteCanonicalSnapshotAndCanBeReadAgain()
    {
        iisacc::accounts::AccountManager manager;
        CTEST_CHECK(manager.readAccount({{"sub", "account_001"}, {"email", "author@example.org"},
            {"userId", "@example_author"}, {"societyCloudMembership", "Pro"}, {"passwordEnabled", true},
            {"authorDetails", QVariantMap{{"organization", "Example Studio"}}}}));
        const auto expected = manager.account()->toVariantMap();
        QString error;
        const auto payload = ObjectCodec::encode(manager.account(), &error);
        manager.clear();
        const auto snapshot = ObjectCodec::decode(payload, &error).value<ObjectSnapshot>();
        CTEST_COMPARE(snapshot.className, "iisacc::accounts::Account");
        CTEST_COMPARE(snapshot.properties, expected);
        CTEST_COMPARE(snapshot.properties.size(), 11);
        CTEST_CHECK(snapshot.properties.contains("societyContainerDrive"));
        CTEST_CHECK(snapshot.properties.value("societyContainerDrive").isNull());
        CTEST_COMPARE(snapshot.properties.value("authorDetails").toMap().size(), 20);
        iisacc::accounts::AccountManager restored;
        CTEST_CHECK(restored.readAccount(snapshot.properties));
        CTEST_COMPARE(restored.account()->toVariantMap(), expected);
        CTEST_CHECK(error.isEmpty());
    }

    void rejectsUnsupportedPointersCyclesDepthAndSize()
    {
        QString error;
        CTEST_CHECK(ObjectCodec::encode(QVariant::fromValue(UnsupportedObject{}), &error).isEmpty());
        CTEST_CHECK(!error.isEmpty());
        int value = 1;
        CTEST_CHECK(ObjectCodec::encode(QVariant::fromValue(static_cast<void *>(&value)), &error).isEmpty());
        TransferObject object;
        object.detail = &object;
        CTEST_CHECK(ObjectCodec::encode(&object, &error).isEmpty());
        CTEST_CHECK(!error.isEmpty());
        CTEST_CHECK(ObjectCodec::encode(static_cast<const QObject *>(nullptr), &error).isEmpty());
        QVariant deep = "leaf";
        for (int i = 0; i < 40; ++i) deep = QVariantList{deep};
        CTEST_CHECK(ObjectCodec::encode(deep, &error).isEmpty());
        CTEST_CHECK(ObjectCodec::encode(QByteArray(70000, 'x'), &error).isEmpty());
        CTEST_CHECK(!error.isEmpty());
    }

    void rejectsInvalidVersionTypeBase64AndTruncatedStreams()
    {
        const auto original = ObjectCodec::encode(QVariant::fromValue(sampleJob()));
        for (int mode = 0; mode < 7; ++mode) {
            auto payload = original;
            if (mode == 0) payload["version"] = 2;
            if (mode == 1) payload["streamVersion"] = 999;
            if (mode == 2) payload["typeName"] = "UnavailableType";
            if (mode == 3) payload["data"] = "@@@";
            if (mode == 4) payload["data"] = QString::fromLatin1(QByteArray::fromBase64(payload["data"].toByteArray()).chopped(1).toBase64());
            if (mode == 5) payload["data"] = QString::fromLatin1((QByteArray::fromBase64(payload["data"].toByteArray()) + "trailing").toBase64());
            if (mode == 6) payload["unexpected"] = true;
            QString error;
            CTEST_CHECK(!ObjectCodec::decode(payload, &error).isValid());
            CTEST_CHECK(!error.isEmpty());
        }
        QString error;
        CTEST_CHECK(!ObjectCodec::decode({{"ordinary", "sendData payload"}}, &error).isValid());
    }

    void objectFailuresNeverCreatePartialOutboxMessages()
    {
        QTemporaryDir root((society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + QStringLiteral("/object-failure-XXXXXX")));
        Helper helper;
        CallbackObject callback;
        bool accessed = false;
        callback.onRead = [&] { accessed = true; };
        CTEST_CHECK(helper.sendObject("object", &callback).isEmpty());
        CTEST_CHECK(!accessed);
        CTEST_CHECK(helper.start({"com.iisacc.objects", "Objects", "1"}, {root.path(), 50, 400}));
        DeliveryStore store;
        CTEST_CHECK(store.open(root.path()));
        const auto count = store.pendingCount();
        helper_ctest::SignalCounter queued(&helper, &Helper::dataQueued);
        CTEST_CHECK(helper.sendObject("bad", UnsupportedObject{}).isEmpty());
        CTEST_CHECK(!helper.errorString().isEmpty());
        CTEST_CHECK(helper.sendObject("large", QByteArray(70000, 'x')).isEmpty());
        CTEST_COMPARE(store.pendingCount(), count);
        CTEST_COMPARE(queued.size(), 0);
        CTEST_CHECK(!helper.sendObject("good", sampleJob()).isEmpty());
        CTEST_COMPARE(store.pendingCount(), count + 1);
        CTEST_COMPARE(queued.size(), 1);
    }

    void getterRestartAndDeletionCannotQueueUnderAnotherSender()
    {
        QTemporaryDir root((society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + QStringLiteral("/object-reentry-XXXXXX")));
        Helper helper;
        const ObservationOptions options{root.path(), 50, 400};
        CTEST_CHECK(helper.start({"com.iisacc.first", "First", "1"}, options));
        CallbackObject source;
        bool restarted = false;
        source.onRead = [&] {
            helper.stop();
            restarted = helper.start({"com.iisacc.second", "Second", "1"}, options);
        };
        const auto rejected = helper.sendObject("must.not.queue", &source);
        CTEST_CHECK(restarted);
        CTEST_CHECK(rejected.isEmpty());
        DeliveryStore store;
        CTEST_CHECK(store.open(root.path()));
        CTEST_CHECK(store.receivePending() >= 3);
        for (const auto &entry : store.readAfter(0)) CTEST_CHECK(entry.toMap().value("topic") != "must.not.queue");
        QPointer<Helper> doomed = new Helper;
        CTEST_CHECK(doomed->start({"com.iisacc.doomed", "Doomed", "1"}, options));
        source.onRead = [&] { delete doomed.data(); };
        CTEST_CHECK(doomed->sendObject("must.not.queue", &source).isEmpty());
        CTEST_CHECK(!doomed);
    }

    void foreignThreadObjectIsRejected()
    {
        QThread worker;
        auto *object = new TransferObject;
        object->moveToThread(&worker);
        connect(&worker, &QThread::finished, object, &QObject::deleteLater);
        worker.start();
        QString error;
        const auto payload = ObjectCodec::encode(object, &error);
        worker.quit();
        CTEST_CHECK(worker.wait(5000));
        CTEST_CHECK(payload.isEmpty());
        CTEST_CHECK(!error.isEmpty());
    }

    void capturesNestedValuesBeforeLaterGettersDestroyThem()
    {
        DeletingSiblingObject source;
        QString error;
        const auto payload = ObjectCodec::encode(&source, &error);
        CTEST_CHECK_MESSAGE(!payload.isEmpty(), qPrintable(error));
        CTEST_CHECK(!source.child);
        const auto snapshot = ObjectCodec::decode(payload, &error).value<ObjectSnapshot>();
        CTEST_COMPARE(snapshot.properties.value("detail").value<ObjectSnapshot>().properties.value("title"), "original");
        CTEST_COMPARE(snapshot.properties.value("trigger"), 1);
        QPointer<CallbackObject> temporary = new CallbackObject;
        temporary->onRead = [&] { delete temporary.data(); };
        CTEST_CHECK(ObjectCodec::encode(temporary.data(), &error).isEmpty());
        CTEST_CHECK(!temporary);
        CTEST_CHECK(!error.isEmpty());
    }

    void errorNotificationMayDestroyHelperWithoutAStaleSignal()
    {
        QTemporaryDir root((society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + QStringLiteral("/object-error-lifetime-XXXXXX")));
        QPointer<Helper> helper = new Helper;
        CTEST_CHECK(helper->start({"com.iisacc.error", "Error", "1"}, {root.path(), 50, 400}));
        bool staleError = false;
        connect(helper, &Helper::errorChanged, this, [&] { delete helper.data(); });
        connect(helper, &Helper::errorOccurred, this, [&] { staleError = true; });
        CTEST_CHECK(helper->sendObject("unsupported", UnsupportedObject{}).isEmpty());
        CTEST_CHECK(!helper);
        CTEST_CHECK(!staleError);
    }

    void qmlCanQueueTheQObjectDirectly()
    {
        QTemporaryDir root((society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + QStringLiteral("/object-qml-XXXXXX")));
        Helper helper;
        TransferObject object;
        CTEST_CHECK(helper.start({"com.iisacc.qml", "QML", "1"}, {root.path(), 50, 400}));
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("helper", &helper);
        engine.rootContext()->setContextProperty("source", &object);
        QQmlExpression call(engine.rootContext(), nullptr, "helper.sendObject('object.qml', source)");
        const auto id = call.evaluate().toString();
        CTEST_CHECK_MESSAGE(!call.hasError() && !id.isEmpty(), qPrintable(call.error().toString()));
        DeliveryStore store;
        CTEST_CHECK(store.open(root.path()));
        CTEST_CHECK(store.receivePending() >= 2);
        bool found = false;
        for (const auto &entry : store.readAfter(0)) {
            const auto message = entry.toMap();
            if (message.value("id") != id) continue;
            found = true;
            const auto snapshot = ObjectCodec::decode(message.value("payload").toMap()).value<ObjectSnapshot>();
            CTEST_COMPARE(snapshot.properties.value("title"), "original");
        }
        CTEST_CHECK(found);
    }

    void senderAndReceiverProcessesRestoreObjectsAfterSenderExit()
    {
        QTemporaryDir root((society_test::root(QString::fromUtf8(HELPER_TEST_DIRECTORY)) + QStringLiteral("/object-process-XXXXXX")));
        for (const auto &mode : {"send", "receive", "receive"}) {
            QProcess process;
            process.start(QStringLiteral(HELPER_OBJECT_PEER), {mode, root.path()});
            CTEST_CHECK(process.waitForFinished(10000));
            CTEST_COMPARE(process.exitStatus(), QProcess::NormalExit);
            const auto output = process.readAllStandardError();
            CTEST_CHECK_MESSAGE(process.exitCode() == 0, output.constData());
            if (QString::fromLatin1(mode) == "receive") CTEST_COMPARE(process.readAllStandardOutput().trimmed(), "objects=2");
        }
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    helper_ctest::CTestRunner runner;
    ObjectTests tests;
    tests.initTestCase();
    runner.run("valueTypesSurviveJsonQueueEncodingExactly", [&] { tests.valueTypesSurviveJsonQueueEncodingExactly(); });
    runner.run("qobjectSnapshotsCaptureNestedStoredPropertiesWithoutPointers", [&] { tests.qobjectSnapshotsCaptureNestedStoredPropertiesWithoutPointers(); });
    runner.run("accountUsesItsCompleteCanonicalSnapshotAndCanBeReadAgain", [&] { tests.accountUsesItsCompleteCanonicalSnapshotAndCanBeReadAgain(); });
    runner.run("rejectsUnsupportedPointersCyclesDepthAndSize", [&] { tests.rejectsUnsupportedPointersCyclesDepthAndSize(); });
    runner.run("rejectsInvalidVersionTypeBase64AndTruncatedStreams", [&] { tests.rejectsInvalidVersionTypeBase64AndTruncatedStreams(); });
    runner.run("objectFailuresNeverCreatePartialOutboxMessages", [&] { tests.objectFailuresNeverCreatePartialOutboxMessages(); });
    runner.run("getterRestartAndDeletionCannotQueueUnderAnotherSender", [&] { tests.getterRestartAndDeletionCannotQueueUnderAnotherSender(); });
    runner.run("foreignThreadObjectIsRejected", [&] { tests.foreignThreadObjectIsRejected(); });
    runner.run("capturesNestedValuesBeforeLaterGettersDestroyThem", [&] { tests.capturesNestedValuesBeforeLaterGettersDestroyThem(); });
    runner.run("errorNotificationMayDestroyHelperWithoutAStaleSignal", [&] { tests.errorNotificationMayDestroyHelperWithoutAStaleSignal(); });
    runner.run("qmlCanQueueTheQObjectDirectly", [&] { tests.qmlCanQueueTheQObjectDirectly(); });
    runner.run("senderAndReceiverProcessesRestoreObjectsAfterSenderExit", [&] { tests.senderAndReceiverProcessesRestoreObjectsAfterSenderExit(); });
    return runner.result();
}
