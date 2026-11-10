#include "../../src/Client/LocalFrame.h"
#include <QTest>

using iiSocietyHelper::LocalFrame;
class LocalFrameTests : public QObject {
    Q_OBJECT
private slots:
    void fragmentedAndCoalescedFrames() {
        const QJsonObject first{{"requestId", "one"}, {"data", QString(350000, 'a')}};
        const QJsonObject second{{"requestId", "two"}};
        const auto bytes = LocalFrame::encode(first) + LocalFrame::encode(second);
        QVERIFY(!bytes.isEmpty());
        LocalFrame parser;
        QVERIFY(parser.append(bytes.first(2))); QVERIFY(!parser.take());
        QVERIFY(parser.append(bytes.mid(2, 27))); QVERIFY(!parser.take());
        QVERIFY(parser.append(bytes.mid(29)));
        QCOMPARE(parser.take().value(), first); QCOMPARE(parser.take().value(), second);
        QVERIFY(!parser.take()); QVERIFY(!parser.invalid());
    }
    void rejectsOversizedAndMalformedFrames() {
        QVERIFY(LocalFrame::encode({{"data", QString(600000, 'a')}}).isEmpty());
        LocalFrame tooLarge; QVERIFY(!tooLarge.append(QByteArray::fromHex("00080001"))); QVERIFY(tooLarge.invalid());
        LocalFrame empty; QVERIFY(!empty.append(QByteArray::fromHex("00000000"))); QVERIFY(empty.invalid());
        LocalFrame invalid; QVERIFY(invalid.append(QByteArray::fromHex("000000015b")));
        QVERIFY(!invalid.take()); QVERIFY(invalid.invalid());
        QVERIFY(!invalid.append(LocalFrame::encode({{"x", 1}})));
    }
};
QTEST_GUILESS_MAIN(LocalFrameTests)
#include "local_frame.moc"
