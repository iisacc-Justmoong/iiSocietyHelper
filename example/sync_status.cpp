#include <iiSocietyHelper/Sync/Controller.h>
#include "ManualStatusExecutor.h"
#include <QCoreApplication>
#include <QMetaObject>
#include <iostream>
#include <memory>

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    auto target = std::make_shared<std::weak_ptr<ManualStatusExecutor>>();
    auto receiver = std::make_shared<ManualStatusExecutor>([&application, target] {
        QMetaObject::invokeMethod(&application, [target] {
            if (auto executor = target->lock()) executor->drain();
        }, Qt::QueuedConnection);
    });
    *target = receiver;
    iiSocietyHelper::Sync::Controller controller({});
    auto subscription = controller.status().subscribe(*receiver, [](auto event) {
        std::cout << "sync=" << event.value << '\n';
    });
    if (!subscription) return 1;
    bool read = false;
    auto query = receiver->reserve([&] {
        read = true; std::cout << "sync=" << controller.status().snapshot().value << '\n';
        application.quit();
    });
    if (!query || !query->signal()) return 2;
    application.exec();
    query->cancel(); subscription.close();
    return read ? 0 : 3;
}
