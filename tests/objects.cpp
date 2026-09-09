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
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

using namespace iiSocietyHelper;

class ObjectTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qRegisterMetaType<GenerationJob>(); }

    void valueTypesSurviveJsonQueueEncodingExactly()
    {
        QString error = "old error";
        const auto job = sampleJob();
        auto payload = ObjectCodec::encode(QVariant::fromValue(job), &error);
        QVERIFY2(!payload.isEmpty(), qPrintable(error));
        QVERIFY(error.isEmpty());
        payload = QJsonDocument::fromJson(QJsonDocument(QJsonObject::fromVariantMap(payload)).toJson()).object().toVariantMap();
        const auto value = ObjectCodec::decode(payload, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(value.metaType(), QMetaType::fromType<GenerationJob>());
        QCOMPARE(value.value<GenerationJob>(), job);
        const QVariantMap map{{"largeSigned", job.frame}, {"largeUnsigned", job.seed}, {"bytes", job.data},
            {"date", job.createdAt}, {"url", job.source}, {"nested", QVariantList{true, 1.25, QVariant::fromValue(job)}}};
        QCOMPARE(ObjectCodec::decode(ObjectCodec::encode(map, &error), &error).toMap(), map);
        QVERIFY(error.isEmpty());
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
        QVERIFY2(error.isEmpty(), qPrintable(error));
        root.title = "modified";
        child.title = "modified";
        const auto snapshot = ObjectCodec::decode(payload, &error).value<ObjectSnapshot>();
        QCOMPARE(snapshot.className, "TransferObject");
        QCOMPARE(snapshot.properties.size(), 4);
        QCOMPARE(snapshot.properties.value("title"), "original");
        QCOMPARE(snapshot.properties.value("count").toLongLong(), 9007199254740993LL);
        QCOMPARE(snapshot.properties.value("bytes").toByteArray(), QByteArray::fromHex("00ff80"));
        const auto nested = snapshot.properties.value("detail").value<ObjectSnapshot>();
        QCOMPARE(nested.properties.value("title"), "nested");
        QVERIFY(nested.properties.value("detail").isNull());
        QVERIFY(error.isEmpty());
    }

    void accountUsesItsCompleteCanonicalSnapshotAndCanBeReadAgain()
    {
        iisacc::accounts::AccountManager manager;
        QVERIFY(manager.readAccount({{"sub", "account_001"}, {"email", "author@example.org"},
            {"userId", "@example_author"}, {"societyCloudMembership", "Pro"}, {"passwordEnabled", true},
            {"authorDetails", QVariantMap{{"organization", "Example Studio"}}}}));
        const auto expected = manager.account()->toVariantMap();
        QString error;
        const auto payload = ObjectCodec::encode(manager.account(), &error);
        manager.clear();
        const auto snapshot = ObjectCodec::decode(payload, &error).value<ObjectSnapshot>();
        QCOMPARE(snapshot.className, "iisacc::accounts::Account");
        QCOMPARE(snapshot.properties, expected);
        QCOMPARE(snapshot.properties.size(), 10);
        QCOMPARE(snapshot.properties.value("authorDetails").toMap().size(), 20);
        iisacc::accounts::AccountManager restored;
        QVERIFY(restored.readAccount(snapshot.properties));
        QCOMPARE(restored.account()->toVariantMap(), expected);
        QVERIFY(error.isEmpty());
    }

    void rejectsUnsupportedPointersCyclesDepthAndSize()
    {
        QString error;
        QVERIFY(ObjectCodec::encode(QVariant::fromValue(UnsupportedObject{}), &error).isEmpty());
        QVERIFY(!error.isEmpty());
        int value = 1;
        QVERIFY(ObjectCodec::encode(QVariant::fromValue(static_cast<void *>(&value)), &error).isEmpty());
        TransferObject object;
        object.detail = &object;
        QVERIFY(ObjectCodec::encode(&object, &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(ObjectCodec::encode(static_cast<const QObject *>(nullptr), &error).isEmpty());
        QVariant deep = "leaf";
        for (int i = 0; i < 40; ++i) deep = QVariantList{deep};
        QVERIFY(ObjectCodec::encode(deep, &error).isEmpty());
        QVERIFY(ObjectCodec::encode(QByteArray(70000, 'x'), &error).isEmpty());
        QVERIFY(!error.isEmpty());
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
            QVERIFY(!ObjectCodec::decode(payload, &error).isValid());
            QVERIFY(!error.isEmpty());
        }
        QString error;
        QVERIFY(!ObjectCodec::decode({{"ordinary", "sendData payload"}}, &error).isValid());
    }

    void objectFailuresNeverCreatePartialOutboxMessages()
    {
        QTemporaryDir root(HELPER_TEST_DIRECTORY "/object-failure-XXXXXX");
        Helper helper;
        CallbackObject callback;
        bool accessed = false;
        callback.onRead = [&] { accessed = true; };
        QVERIFY(helper.sendObject("object", &callback).isEmpty());
        QVERIFY(!accessed);
        QVERIFY(helper.start({"com.iisacc.objects", "Objects", "1"}, {root.path(), 50, 400}));
        DeliveryStore store;
        QVERIFY(store.open(root.path()));
        const auto count = store.pendingCount();
        QSignalSpy queued(&helper, &Helper::dataQueued);
        QVERIFY(helper.sendObject("bad", UnsupportedObject{}).isEmpty());
        QVERIFY(!helper.errorString().isEmpty());
        QVERIFY(helper.sendObject("large", QByteArray(70000, 'x')).isEmpty());
        QCOMPARE(store.pendingCount(), count);
        QCOMPARE(queued.size(), 0);
        QVERIFY(!helper.sendObject("good", sampleJob()).isEmpty());
        QCOMPARE(store.pendingCount(), count + 1);
        QCOMPARE(queued.size(), 1);
    }

    void getterRestartAndDeletionCannotQueueUnderAnotherSender()
    {
        QTemporaryDir root(HELPER_TEST_DIRECTORY "/object-reentry-XXXXXX");
        Helper helper;
        const ObservationOptions options{root.path(), 50, 400};
        QVERIFY(helper.start({"com.iisacc.first", "First", "1"}, options));
        CallbackObject source;
        source.onRead = [&] {
            helper.stop();
            QVERIFY(helper.start({"com.iisacc.second", "Second", "1"}, options));
        };
        QVERIFY(helper.sendObject("must.not.queue", &source).isEmpty());
        DeliveryStore store;
        QVERIFY(store.open(root.path()));
        QVERIFY(store.receivePending() >= 3);
        for (const auto &entry : store.readAfter(0)) QVERIFY(entry.toMap().value("topic") != "must.not.queue");
        QPointer<Helper> doomed = new Helper;
        QVERIFY(doomed->start({"com.iisacc.doomed", "Doomed", "1"}, options));
        source.onRead = [&] { delete doomed.data(); };
        QVERIFY(doomed->sendObject("must.not.queue", &source).isEmpty());
        QVERIFY(!doomed);
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
        QVERIFY(worker.wait(5000));
        QVERIFY(payload.isEmpty());
        QVERIFY(!error.isEmpty());
    }

    void capturesNestedValuesBeforeLaterGettersDestroyThem()
    {
        DeletingSiblingObject source;
        QString error;
        const auto payload = ObjectCodec::encode(&source, &error);
        QVERIFY2(!payload.isEmpty(), qPrintable(error));
        QVERIFY(!source.child);
        const auto snapshot = ObjectCodec::decode(payload, &error).value<ObjectSnapshot>();
        QCOMPARE(snapshot.properties.value("detail").value<ObjectSnapshot>().properties.value("title"), "original");
        QCOMPARE(snapshot.properties.value("trigger"), 1);
        QPointer<CallbackObject> temporary = new CallbackObject;
        temporary->onRead = [&] { delete temporary.data(); };
        QVERIFY(ObjectCodec::encode(temporary.data(), &error).isEmpty());
        QVERIFY(!temporary);
        QVERIFY(!error.isEmpty());
    }

    void errorNotificationMayDestroyHelperWithoutAStaleSignal()
    {
        QTemporaryDir root(HELPER_TEST_DIRECTORY "/object-error-lifetime-XXXXXX");
        QPointer<Helper> helper = new Helper;
        QVERIFY(helper->start({"com.iisacc.error", "Error", "1"}, {root.path(), 50, 400}));
        bool staleError = false;
        connect(helper, &Helper::errorChanged, this, [&] { delete helper.data(); });
        connect(helper, &Helper::errorOccurred, this, [&] { staleError = true; });
        QVERIFY(helper->sendObject("unsupported", UnsupportedObject{}).isEmpty());
        QVERIFY(!helper);
        QVERIFY(!staleError);
    }

    void qmlCanQueueTheQObjectDirectly()
    {
        QTemporaryDir root(HELPER_TEST_DIRECTORY "/object-qml-XXXXXX");
        Helper helper;
        TransferObject object;
        QVERIFY(helper.start({"com.iisacc.qml", "QML", "1"}, {root.path(), 50, 400}));
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("helper", &helper);
        engine.rootContext()->setContextProperty("source", &object);
        QQmlExpression call(engine.rootContext(), nullptr, "helper.sendObject('object.qml', source)");
        const auto id = call.evaluate().toString();
        QVERIFY2(!call.hasError() && !id.isEmpty(), qPrintable(call.error().toString()));
        DeliveryStore store;
        QVERIFY(store.open(root.path()));
        QVERIFY(store.receivePending() >= 2);
        bool found = false;
        for (const auto &entry : store.readAfter(0)) {
            const auto message = entry.toMap();
            if (message.value("id") != id) continue;
            found = true;
            const auto snapshot = ObjectCodec::decode(message.value("payload").toMap()).value<ObjectSnapshot>();
            QCOMPARE(snapshot.properties.value("title"), "original");
        }
        QVERIFY(found);
    }

    void senderAndReceiverProcessesRestoreObjectsAfterSenderExit()
    {
        QTemporaryDir root(HELPER_TEST_DIRECTORY "/object-process-XXXXXX");
        for (const auto &mode : {"send", "receive", "receive"}) {
            QProcess process;
            process.start(QStringLiteral(HELPER_OBJECT_PEER), {mode, root.path()});
            QVERIFY(process.waitForFinished(10000));
            QCOMPARE(process.exitStatus(), QProcess::NormalExit);
            const auto output = process.readAllStandardError();
            QVERIFY2(process.exitCode() == 0, output.constData());
            if (QString::fromLatin1(mode) == "receive") QCOMPARE(process.readAllStandardOutput().trimmed(), "objects=2");
        }
    }
};
QTEST_GUILESS_MAIN(ObjectTests)
#include "objects.moc"
