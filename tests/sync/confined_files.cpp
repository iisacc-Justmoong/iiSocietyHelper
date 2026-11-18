#include "../TestWorkspace.h"
#include "ConfinedFiles.h"
#include <iiFileProviderPresentation.h>
#include "TestLink.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QCoreApplication>
#include "../CTestSupport.h"
#include <SocietyDrive.h>
#include <QScopeGuard>

using iiSocietyHelper::Sync::detail::ConfinedFiles;
using iiSocietyHelper::Sync::detail::FileState;

class ConfinedFilesTests final {
public:
    void previewPublicationPreservesPinnedStamp() {
        QTemporaryDir directory((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/preview-stamp-XXXXXX")));
        ConfinedFiles files; CTEST_CHECK(files.open(directory.path()));
        CTEST_CHECK(files.append("document.txt", 0, "original"));
        iiFileProvider::native::Presentation::cancelPending(std::filesystem::path(directory.path().toStdU16String()));
        const auto path = std::filesystem::path(directory.filePath("document.txt").toStdU16String());
        iiSocietyHelper::Sync::detail::FileState before, after;
        CTEST_CHECK(files.state("document.txt", &before));
        (void)iiFileProvider::native::Presentation::prepare(path);
        CTEST_CHECK(files.state("document.txt", &after));
        CTEST_COMPARE(after.stamp, before.stamp);
        const auto modified = std::filesystem::last_write_time(path);
        QFile changed(directory.filePath("document.txt")); CTEST_CHECK(changed.open(QIODevice::WriteOnly));
        CTEST_COMPARE(changed.write("modified"), 8); changed.close();
        std::filesystem::last_write_time(path, modified);
        CTEST_CHECK(files.state("document.txt", &after));
        CTEST_CHECK(after.stamp != before.stamp);
    }
    void ownPreviewAttributesPreservePinnedReadsWhileExternalChangesStillInvalidate() {
        QTemporaryDir dir((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/presentation-stamp-XXXXXX"))); CTEST_CHECK(dir.isValid());
        const auto path = dir.filePath("asset.txt");
        { QFile file(path); CTEST_CHECK(file.open(QIODevice::WriteOnly)); CTEST_COMPARE(file.write("original"), 8); }
        ConfinedFiles files; CTEST_CHECK(files.open(dir.path())); FileState before, after;
        CTEST_CHECK(files.state("asset.txt", &before));
        const auto native = std::filesystem::path(path.toStdU16String());
        (void)iiFileProvider::native::Presentation::prepare(native);
        CTEST_CHECK(files.state("asset.txt", &after)); CTEST_COMPARE(after.stamp, before.stamp);
        CTEST_COMPARE(files.read("asset.txt", 0, 8, before.stamp), QByteArray("original"));
        const auto time = std::filesystem::last_write_time(native);
        { QFile file(path); CTEST_CHECK(file.open(QIODevice::WriteOnly)); CTEST_COMPARE(file.write("modified"), 8); }
        std::filesystem::last_write_time(native, time);
        CTEST_CHECK(files.state("asset.txt", &after)); CTEST_CHECK(after.stamp != before.stamp);
        CTEST_CHECK(files.read("asset.txt", 0, 8, before.stamp).isEmpty());
    }
    void directoryPublicFilesRemainConfinedAndSupportPrivateRecovery() {
        QTemporaryDir dir((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/native-files-XXXXXX.societycontainer"))); CTEST_CHECK(dir.isValid());
        const auto root = dir.path();
        const auto drive = iiSocietyContainer::SocietyDrive::create(root); CTEST_CHECK(drive);
        const auto publicRoot = drive->sectionPath(iiSocietyContainer::StoreSection::Files);
        ConfinedFiles files; CTEST_CHECK_MESSAGE(files.open(root), qPrintable(files.error));
        CTEST_CHECK(files.mkdir("Files/Documents/Nested"));
        CTEST_COMPARE(publicRoot, root + "/Files"); CTEST_CHECK(QFileInfo(publicRoot).isDir());
        CTEST_CHECK(files.append("Files/Documents/Nested/example.txt", 0, "original"));
        CTEST_CHECK(QFileInfo::exists(publicRoot + "/Documents/Nested/example.txt"));
        CTEST_CHECK(files.mkdir(".society-sync/transfers"));
        CTEST_CHECK(files.append(".society-sync/transfers/incoming", 0, "updated"));
        CTEST_CHECK_MESSAGE(files.install(".society-sync/transfers/incoming", "Files/Documents/Nested/example.txt"), qPrintable(files.error));
        CTEST_COMPARE(files.read("Files/Documents/Nested/example.txt", 0, 100), QByteArray("updated"));
        CTEST_COMPARE(QDir(root + "/.society-sync/recovery").entryList({"*.json"}, QDir::Files).size(), 1);
        const auto originalPath = std::filesystem::path((publicRoot + "/Documents/Nested/example.txt").toStdU16String());
        (void)iiFileProvider::native::Presentation::prepare(originalPath);
        const auto derived = iiFileProvider::native::Presentation::companionPath(originalPath);
        CTEST_CHECK(std::filesystem::exists(derived));
        CTEST_CHECK(files.remove("Files/Documents/Nested/example.txt"));
        CTEST_CHECK(!std::filesystem::exists(derived));
        CTEST_CHECK(!QFileInfo::exists(publicRoot + "/Documents/Nested/example.txt"));
        CTEST_CHECK_MESSAGE(files.remove("Files/Documents/Nested"), qPrintable(files.error));
        CTEST_CHECK(files.mkdir("Files/Documents/Nested"));
        CTEST_CHECK(files.append("Files/Documents/Nested/keep.txt", 0, "adoption recovery"));
        CTEST_CHECK_MESSAGE(files.detach("Files/Documents/Nested", ".society-sync/detached/test/Files/Documents/Nested"), qPrintable(files.error));
        CTEST_COMPARE(files.read(".society-sync/detached/test/Files/Documents/Nested/keep.txt", 0, 100), QByteArray("adoption recovery"));
        CTEST_CHECK(!QFileInfo::exists(publicRoot + "/Documents/Nested"));
        CTEST_CHECK(!files.read("Files/../Models/private", 0, 10).size());
        const auto retired = root + ".retired";
        const auto cleanup = qScopeGuard([&] { QDir(retired).removeRecursively(); });
        CTEST_CHECK(QDir().rename(root, retired));
        CTEST_CHECK(!files.intact());
    }
    void hashingReportsRealByteProgressAndCanStopAtAChunkBoundary() {
        QTemporaryDir dir((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/hash-progress-XXXXXX")));
        ConfinedFiles files; CTEST_CHECK(files.open(dir.path()));
        const QByteArray bytes(3 * 1024 * 1024 + 71, 'p');
        QFile output(dir.filePath("model")); CTEST_CHECK(output.open(QIODevice::WriteOnly));
        CTEST_COMPARE(output.write(bytes), bytes.size()); output.close();
        QList<qint64> observed;
        bool validProgress = true;
        files.hashProgress = [&](const QString &path, qint64 done, qint64 total) {
            if (path != QString("model") || total != bytes.size() ||
                (!observed.isEmpty() && done <= observed.last())) {
                validProgress = false;
                return;
            }
            observed.append(done);
        };
        CTEST_COMPARE(files.hash("model"), QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()));
        CTEST_CHECK(validProgress); CTEST_CHECK(!observed.isEmpty());
        CTEST_COMPARE(observed.first(), 0); CTEST_COMPARE(observed.last(), bytes.size());
        CTEST_CHECK(observed.size() > 2);
        observed.clear();
        files.cancelled = [&] { return !observed.isEmpty() && observed.last() > 0; };
        CTEST_CHECK(files.hash("model").isEmpty());
        CTEST_CHECK(validProgress); CTEST_CHECK(!observed.isEmpty());
        CTEST_COMPARE(files.error, QString("cancelled"));
        CTEST_CHECK(observed.last() < bytes.size());
    }
    void sha256MatchesAcrossPaddingAndReadBoundaries() {
        QTemporaryDir dir((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/files-digest-XXXXXX")));
        ConfinedFiles files; CTEST_CHECK(files.open(dir.path()));
        for (const int size : {0, 1, 55, 56, 63, 64, 65, 1024 * 1024, 1024 * 1024 + 333}) {
            QByteArray bytes(size, Qt::Uninitialized);
            for (int i = 0; i < size; ++i) bytes[i] = char(i % 251);
            const auto name = QString::number(size);
            QFile output(dir.filePath(name)); CTEST_CHECK(output.open(QIODevice::WriteOnly));
            CTEST_COMPARE(output.write(bytes), bytes.size()); output.close();
            FileState state; CTEST_CHECK(files.state(name, &state));
            CTEST_COMPARE(files.hash(name, state.stamp), QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()));
        }
        CTEST_COMPARE(files.hash("0"), QStringLiteral("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    }
    void chunksResumeAndReplacementKeepsOriginal() {
        QTemporaryDir dir((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/files-chunks-XXXXXX"))); CTEST_CHECK(dir.isValid());
        ConfinedFiles files; CTEST_CHECK_MESSAGE(files.open(dir.path()), qPrintable(files.error));
        CTEST_CHECK(files.mkdir("Files/nested")); CTEST_CHECK(files.mkdir(".society-sync/transfers"));
        CTEST_CHECK(files.append("Files/nested/item", 0, "old"));
        CTEST_CHECK(files.append(".society-sync/transfers/incoming", 0, "abc"));
        CTEST_CHECK(files.append(".society-sync/transfers/incoming", 0, "abc"));
        CTEST_CHECK(!files.append(".society-sync/transfers/incoming", 0, "xyz"));
        CTEST_CHECK(!files.append(".society-sync/transfers/incoming", 2, "cd"));
        CTEST_CHECK(!files.append(".society-sync/transfers/incoming", 4, "e"));
        ConfinedFiles resumed; CTEST_CHECK(resumed.open(dir.path()));
        CTEST_CHECK(resumed.append(".society-sync/transfers/incoming", 3, "def"));
        CTEST_CHECK_MESSAGE(resumed.install(".society-sync/transfers/incoming", "Files/nested/item"), qPrintable(resumed.error));
        CTEST_COMPARE(resumed.read("Files/nested/item", 0, 100), QByteArray("abcdef"));
        const QDir recovery(dir.filePath(".society-sync/recovery"));
        const auto saved = recovery.entryList({"*.json"}, QDir::Files); CTEST_COMPARE(saved.size(), 1);
        QFile metadata(recovery.filePath(saved[0])); CTEST_CHECK(metadata.open(QIODevice::ReadOnly));
        const auto backup = QJsonDocument::fromJson(metadata.readAll()).object().value("backup").toString();
        CTEST_COMPARE(resumed.read(backup, 0, 100), QByteArray("old"));
        CTEST_CHECK(resumed.remove("Files/nested/item")); CTEST_CHECK(resumed.remove("Files/nested/item"));
        CTEST_COMPARE(recovery.entryList({"*.json"}, QDir::Files).size(), 2);
        CTEST_CHECK(resumed.remove("Files/nested"));
    }
    void enumerationStampsAndCancellation() {
        QTemporaryDir dir((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/files-list-XXXXXX")));
        ConfinedFiles files; CTEST_CHECK(files.open(dir.path())); CTEST_CHECK(files.mkdir("Files"));
        for (int i = 0; i < 300; ++i) CTEST_CHECK(files.append("Files/항목-" + QString::number(i), 0, "entry"));
        bool ok = false; const auto names = files.list("Files", &ok); CTEST_CHECK_MESSAGE(ok, qPrintable(files.error)); CTEST_COMPARE(names.size(), 300);
        CTEST_CHECK(names.contains("항목-299"));
        const QByteArray bytes(1024 * 1024 + 333, 'x'); CTEST_CHECK(files.append("Files/large", 0, bytes));
        FileState original; CTEST_CHECK(files.state("Files/large", &original)); CTEST_COMPARE(original.size, bytes.size());
        CTEST_COMPARE(files.hash("Files/large", original.stamp), QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()));
        CTEST_COMPARE(files.read("Files/large", 1024 * 1024, 512, original.stamp), QByteArray(333, 'x'));
        CTEST_CHECK(files.read("Files/large", 0, 1024 * 1024 + 1).isEmpty()); CTEST_CHECK(!files.error.isEmpty());
        CTEST_CHECK(files.append("Files/large", bytes.size(), "changed"));
        CTEST_CHECK(files.hash("Files/large", original.stamp).isEmpty());
        files.cancelled = [] { return true; };
        CTEST_CHECK(files.hash("Files/large").isEmpty()); CTEST_CHECK(!files.mkdir("cancelled"));
        files.cancelled = {};
        CTEST_CHECK(files.state("missing/child", &original)); CTEST_COMPARE(original.kind, "deleted");
        CTEST_CHECK(!files.remove("Files")); // Nonempty directories must survive.
    }
    void pathsCannotEscapeAndLegacyLinksCanBeDetached() {
        QTemporaryDir dir((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/files-boundary-XXXXXX")));
        const auto root = dir.filePath("drive"); CTEST_CHECK(QDir().mkdir(root));
        ConfinedFiles files; CTEST_CHECK(files.open(root)); CTEST_CHECK(files.mkdir("Files"));
        QFile outside(dir.filePath("outside")); CTEST_CHECK(outside.open(QIODevice::WriteOnly)); outside.write("outside"); outside.close();
        CTEST_CHECK(nativeTestLink(outside.fileName(), root + "/Files/link"));
        CTEST_CHECK(nativeTestLink(dir.path(), root + "/Files/directory-link"));
        FileState state;
        for (const auto &path : {QString("../outside"), QString("Files/../../outside"), QString("Files/link"), QString("Files/directory-link/outside")}) {
            CTEST_CHECK_MESSAGE(!files.state(path, &state), qPrintable(path));
            CTEST_CHECK(files.read(path, 0, 10).isEmpty()); CTEST_CHECK(!files.append(path, 0, "overwrite"));
            CTEST_CHECK(!files.remove(path));
        }
        CTEST_CHECK(!files.append("Files/stream:secret", 0, "data"));
        CTEST_CHECK(!files.detach("Files/link", "Files/escaped"));
        CTEST_CHECK_MESSAGE(files.detach("Files/link", ".society-sync/detached/old/link"), qPrintable(files.error));
        CTEST_CHECK(files.detach("Files/link", ".society-sync/detached/old/link"));
        CTEST_CHECK(files.detach("Files/directory-link", ".society-sync/detached/old/directory-link"));
        CTEST_CHECK(outside.open(QIODevice::ReadOnly)); CTEST_COMPARE(outside.readAll(), QByteArray("outside")); outside.close();
        CTEST_CHECK(files.append("Files/new", 0, "new"));
        CTEST_CHECK(!files.detach("Files/new", ".society-sync/detached/old/link"));
        CTEST_COMPARE(files.read("Files/new", 0, 10), QByteArray("new"));
        CTEST_CHECK(removeNativeTestLink(root + "/.society-sync/detached/old/link"));
        CTEST_CHECK(removeNativeTestLink(root + "/.society-sync/detached/old/directory-link"));
        CTEST_CHECK(QDir(dir.path()).rename("drive", "old")); CTEST_CHECK(QDir().mkdir(root));
        CTEST_CHECK(!files.intact()); CTEST_CHECK(!files.append("Files/new", 0, "wrong root"));
    }
#ifdef Q_OS_WIN
    void windowsAliasesAndOpenWritersFailClosed() {
        QTemporaryDir dir((society_test::root(QString::fromUtf8(SYNC_TEST_DIRECTORY)) + QStringLiteral("/files-win-XXXXXX"))); ConfinedFiles files;
        CTEST_CHECK(files.open(dir.path())); CTEST_CHECK(files.mkdir("Files"));
        for (const auto *name : {"trailing.", "space ", "CON", "NUL.txt", "LPT1", "a?b", "a|b"})
            CTEST_CHECK(!files.append("Files/" + QString::fromLatin1(name), 0, "data"));
        CTEST_CHECK(files.append("Files/ITEM", 0, "original")); CTEST_CHECK(!files.exactPath("Files/item"));
        QFile writer(dir.filePath("Files/ITEM")); CTEST_CHECK(writer.open(QIODevice::ReadWrite));
        CTEST_CHECK(files.hash("Files/ITEM").isEmpty()); CTEST_CHECK(!files.append("Files/ITEM", 8, "x")); writer.close();
        CTEST_CHECK(!files.hash("Files/ITEM").isEmpty());
        bool attempted = false, renamed = false;
        // openPath checks cancellation before pinning. The callback in the hash
        // loop must run while the full directory chain is held.
        int calls = 0;
        files.cancelled = [&] {
            if (++calls > 1 && !attempted) { attempted = true; renamed = QDir().rename(dir.path(), dir.path() + "-moved"); }
            return false;
        };
        CTEST_CHECK(!files.hash("Files/ITEM").isEmpty()); CTEST_CHECK(attempted); CTEST_CHECK(!renamed);
        files.cancelled = {};
    }
#endif
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    helper_ctest::CTestRunner runner;
    ConfinedFilesTests tests;
    runner.run("previewPublicationPreservesPinnedStamp", [&] { tests.previewPublicationPreservesPinnedStamp(); });
    runner.run("ownPreviewAttributesPreservePinnedReadsWhileExternalChangesStillInvalidate", [&] { tests.ownPreviewAttributesPreservePinnedReadsWhileExternalChangesStillInvalidate(); });
    runner.run("directoryPublicFilesRemainConfinedAndSupportPrivateRecovery", [&] { tests.directoryPublicFilesRemainConfinedAndSupportPrivateRecovery(); });
    runner.run("hashingReportsRealByteProgressAndCanStopAtAChunkBoundary", [&] { tests.hashingReportsRealByteProgressAndCanStopAtAChunkBoundary(); });
    runner.run("sha256MatchesAcrossPaddingAndReadBoundaries", [&] { tests.sha256MatchesAcrossPaddingAndReadBoundaries(); });
    runner.run("chunksResumeAndReplacementKeepsOriginal", [&] { tests.chunksResumeAndReplacementKeepsOriginal(); });
    runner.run("enumerationStampsAndCancellation", [&] { tests.enumerationStampsAndCancellation(); });
    runner.run("pathsCannotEscapeAndLegacyLinksCanBeDetached", [&] { tests.pathsCannotEscapeAndLegacyLinksCanBeDetached(); });
#ifdef Q_OS_WIN
    runner.run("windowsAliasesAndOpenWritersFailClosed", [&] { tests.windowsAliasesAndOpenWritersFailClosed(); });
#endif
    return runner.result();
}
