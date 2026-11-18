#include "QtWarningCapture.h"
#include <QCoreApplication>
#include <QScopeGuard>
#include <atomic>
#include <thread>

namespace {
std::atomic<int> forwarded{0};
void recordOther(QtMsgType, const QMessageLogContext&, const QString&) { ++forwarded; }
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    const auto previous = qInstallMessageHandler(recordOther);
    const auto restore = qScopeGuard([&] { qInstallMessageHandler(previous); });
    {
        helper_ctest::ExpectedWarning expected(QStringLiteral("expected fixture warning"));
        std::jthread worker([] { qWarning("expected fixture warning"); });
        worker.join();
        qWarning("different warning");
        if (!expected.seen() || forwarded.load() != 1) return 1;
    }
    qWarning("restored warning");
    return forwarded.load() == 2 ? 0 : 1;
}
