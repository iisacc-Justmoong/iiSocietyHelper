#include "iiSocietyHelper.h"
#include "DeliveryStore.h"
#include "ObjectTestTypes.h"
#include <QCoreApplication>
#include <QTextStream>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 3) return 2;
    qRegisterMetaType<GenerationJob>();
    const QString mode = QString::fromLocal8Bit(argv[1]);
    const QString directory = QString::fromLocal8Bit(argv[2]);
    if (mode == "send") {
        iiSocietyHelper::Helper helper;
        if (!helper.start({"com.iisacc.object.sender", "Object sender", "1"}, {directory, 50, 400})) return 3;
        auto job = sampleJob();
        if (helper.sendObject("generation.object", job).isEmpty()) return 4;
        auto object = std::make_unique<TransferObject>();
        object->title = "sent from another process";
        if (helper.sendObject("profile.object", object.get()).isEmpty()) return 5;
        job.id = "changed after send";
        object.reset();
        return 0;
    }
    if (mode != "receive") return 2;
    iiSocietyHelper::DeliveryStore store;
    if (!store.open(directory) || store.receivePending() < 0) return 6;
    int found = 0;
    const auto rows = store.readAfter(0);
    for (const auto &entry : rows) {
        const auto row = entry.toMap();
        const auto topic = row.value("topic").toString();
        if (topic != "generation.object" && topic != "profile.object") continue;
        QString error;
        const auto value = iiSocietyHelper::ObjectCodec::decode(row.value("payload").toMap(), &error);
        if (!error.isEmpty() || !value.isValid()) return 7;
        if (topic == "generation.object") {
            if (value.metaType() != QMetaType::fromType<GenerationJob>() || value.value<GenerationJob>() != sampleJob()) return 8;
        } else {
            const auto snapshot = value.value<iiSocietyHelper::ObjectSnapshot>();
            if (snapshot.className != "TransferObject" || snapshot.properties.value("title") != "sent from another process"
                || snapshot.properties.value("count").toLongLong() != 9007199254740993LL) return 9;
        }
        ++found;
    }
    if (found != 2 || !store.acknowledge("com.iisacc.object.receiver", rows.last().toMap().value("sequence").toLongLong())) return 10;
    QTextStream(stdout) << "objects=2" << Qt::endl;
    return 0;
}
