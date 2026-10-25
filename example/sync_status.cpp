#include <iiSocietyHelper/Sync/Controller.h>
#include <iiSDKCore/Runtime/EventLoop.h>
#include <QCoreApplication>
#include <iostream>

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    ii::runtime::EventLoop receiver;
    iiSocietyHelper::Sync::Controller controller({});
    auto subscription = controller.status().changes().subscribe(receiver, {}, [](auto event) {
        std::cout << "sync=" << event.value << '\n';
    });
    if (!subscription) return 1;
    bool read = false;
    const auto submitted = controller.status().query().post(receiver, {}, [&](auto reply) {
        if (reply) { read = true; std::cout << "sync=" << *reply << '\n'; }
    });
    if (!submitted) return 2;
    receiver.poll();
    return read ? 0 : 3;
}
