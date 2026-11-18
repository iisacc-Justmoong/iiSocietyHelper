#include "CTestSupport.h"
#include "QtSignalCounter.h"
#include <QCoreApplication>
#include <QTimer>

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    helper_ctest::CTestRunner runner;
    runner.run("countsOnlyDeliveredSignals", [] {
        QTimer timer;
        helper_ctest::SignalCounter counter(&timer, &QTimer::timeout);
        CTEST_COMPARE(counter.size(), 0);
        CTEST_CHECK(QMetaObject::invokeMethod(&timer, "timeout", Qt::DirectConnection));
        CTEST_CHECK(QMetaObject::invokeMethod(&timer, "timeout", Qt::DirectConnection));
        CTEST_COMPARE(counter.size(), 2);
    });
    return runner.result();
}
