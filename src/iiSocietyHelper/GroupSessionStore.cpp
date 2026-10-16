#include <iiFileProviderDevice.h>
#include "GroupSessionStore.h"
#include "StateCrypto.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <SharedStorage.h>
#include <QTimer>
#include <QUuid>
#include <cstring>
#ifdef Q_OS_WIN
#include <windows.h>
#include <aclapi.h>
#include <vector>
#endif
#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QJniObject>
#endif
#ifdef Q_OS_DARWIN
QString societyAppleStateDirectory();
bool societyProtectStatePath(const QString &path);
iisacc::accounts::SessionStore *societyAppleStateKeyStore(QObject *parent);
#endif

namespace {
#ifdef Q_OS_WIN
bool protectWindowsState(const QString &path, bool directory) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD size = 0; GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> storage(size);
    const bool read = GetTokenInformation(token, TokenUser, storage.data(), size, &size);
    CloseHandle(token); if (!read) return false;
    const auto user = reinterpret_cast<TOKEN_USER *>(storage.data());
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = FILE_ALL_ACCESS; access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = directory ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID; access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(user->User.Sid);
    PACL acl = nullptr;
    if (SetEntriesInAclW(1, &access, nullptr, &acl) != ERROR_SUCCESS) return false;
    const auto result = SetNamedSecurityInfoW(const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(path.utf16())),
        SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, acl, nullptr);
    LocalFree(acl); return result == ERROR_SUCCESS;
}
#endif
constexpr auto tombstone = "SGS0";
constexpr auto version = "SGS1";
const QString masterKey = QStringLiteral("group-state-aes256-v1");
QByteArray context(const QString &key) { return "com.iisacc.society.group-state.v1:" + key.toUtf8(); }
}

QString GroupSessionStore::defaultDirectory() {
    const auto storage = iiSocietyContainer::SharedStorage::open({}, nullptr, true);
    return storage ? storage->runtimePath(QStringLiteral("state")) : QString();
}
GroupSessionStore *GroupSessionStore::create(QObject *parent) {
    auto *result = new GroupSessionStore(defaultDirectory(), nullptr, nullptr, parent);
    result->m_followsSelection = true;
    if (!result->m_directory.isEmpty()) result->m_managedRoot = iiFileProvider::Directory::cleanPath(result->m_directory + "/../..");
#ifdef Q_OS_DARWIN
    result->m_keys = societyAppleStateKeyStore(result);
    result->m_previousDirectory = societyAppleStateDirectory();
#else
    result->m_keys = SessionStore::create("com.iisacc.society.group-state", result);
#ifdef Q_OS_ANDROID
    const QJniObject app = QNativeInterface::QAndroidApplication::context();
    const auto base = app.callObjectMethod("getNoBackupFilesDir", "()Ljava/io/File;");
    if (base.isValid()) result->m_previousDirectory = iiFileProvider::Directory(base.callObjectMethod("getAbsolutePath", "()Ljava/lang/String;").toString())
        .filePath("group.com.iisacc.society/State");
#else
    result->m_previousDirectory = iiFileProvider::Directory(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath("iisacc/Group Containers/group.com.iisacc.society/State");
#endif
#endif
    result->m_legacy = SessionStore::create("com.iisacc.society", result);
    return result;
}
GroupSessionStore::GroupSessionStore(QString directory, SessionStore *keys, SessionStore *legacy, QObject *parent, QString previousDirectory)
    : SessionStore(parent), m_directory(std::move(directory)), m_previousDirectory(std::move(previousDirectory)), m_keys(keys), m_legacy(legacy), m_followsSelection(m_directory.isEmpty()) {}
bool GroupSessionStore::prepare() {
    if (m_followsSelection) {
        const auto storage = iiSocietyContainer::SharedStorage::open({}, &m_error, true);
        if (!storage) return false;
        const auto directory = storage->ensureRuntimeDirectory(QStringLiteral("state"), &m_error);
        if (directory.isEmpty()) return false;
        if (directory != m_directory) m_observedGenerations.clear();
        m_directory = directory;
        m_managedRoot = storage->drive().rootPath();
    }
    if (m_directory.isEmpty() || !iiFileProvider::Directory::isAbsolutePath(m_directory) || !m_keys) return fail("The Society group location or key store is unavailable.");
    QString current = iiFileProvider::Directory::rootPath();
#ifdef Q_OS_WIN
    current = QFileInfo(m_directory).absoluteFilePath().left(3);
#endif
    if (!m_managedRoot.isEmpty()) {
        // Mobile sandboxes cannot inspect arbitrary ancestors such as /var or
        // /data/user. Start at the root returned by the OS entitlement API.
        QFileInfo root(m_managedRoot);
        if (root.isSymLink() || root.isJunction()) return fail("The managed Society group is redirected.");
        root.refresh(); if (!root.isDir()) return fail("The managed Society group is unavailable.");
        current = m_managedRoot;
    }
    for (const auto &part : iiFileProvider::Directory(current).relativeFilePath(iiFileProvider::Directory::cleanPath(m_directory)).split('/')) {
        if (part.isEmpty() || part == "." || part == "..") return fail("The Society state path is invalid.");
        current = iiFileProvider::Directory(current).filePath(part);
        QFileInfo info(current);
        if (info.isSymLink() || info.isJunction()) return fail("The Society state directory is redirected.");
        if (!info.exists() && !iiFileProvider::Directory().mkdir(current)) return fail("The Society state directory cannot be created.");
        info.refresh(); if (!info.isDir()) return fail("The Society state directory is unavailable.");
    }
    if (!iiFileProvider::FileDevice::setPermissions(m_directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) return fail("Society state permissions could not be applied.");
#ifdef Q_OS_WIN
    if (!protectWindowsState(m_directory, true)) return fail("Society state ACL could not be applied.");
#endif
#ifdef Q_OS_DARWIN
    if (!societyProtectStatePath(m_directory)) return fail("Society state data protection could not be applied.");
#endif
    return true;
}
QString GroupSessionStore::path(const QString &key) const {
    return iiFileProvider::Directory(m_directory).filePath(QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex()) + ".state");
}
bool GroupSessionStore::migrateRecord(const QString &key) {
    if (m_previousDirectory.isEmpty() || m_previousDirectory == m_directory || QFileInfo::exists(path(key))
        || QFileInfo::exists(path(key) + ".revocation")) return true;
    const QFileInfo previous(m_previousDirectory);
    if (!previous.exists()) return true;
    if (!previous.isDir() || previous.isSymLink() || previous.isJunction()) return fail("The previous Society state location is redirected.");
    // Copy only the requested encrypted record and its revocation marker. The
    // old directory stays read-only, including its locks and unrelated files.
    for (const auto &suffix : {QStringLiteral(".revocation"), QString()}) {
        const auto source = iiFileProvider::Directory(m_previousDirectory).filePath(QFileInfo(path(key)).fileName() + suffix);
        const QFileInfo info(source);
        if (!info.exists()) continue;
        iiFileProvider::FileDevice file(source);
        const auto maximum = suffix.isEmpty() ? MaximumSocietyStateBytes + 48 : 16;
        if (info.isSymLink() || info.isJunction() || !info.isFile() || info.size() > maximum
            || (!suffix.isEmpty() && info.size() != 16) || !file.open(QIODevice::ReadOnly))
            return fail("The previous Society encrypted record is invalid.");
        const auto bytes = file.readAll();
        if (!suffix.isEmpty() || bytes == tombstone || bytes.startsWith(version)) {
            if (!save(key, bytes, suffix)) return false;
        } else return fail("The previous Society encrypted record is unsupported.");
    }
    return true;
}
QByteArray GroupSessionStore::generation(const QString &key) const {
    if (!iiFileProvider::Directory::isAbsolutePath(m_directory)) return {};
    iiFileProvider::FileDevice file(path(key) + ".revocation"); const QFileInfo info(file);
    if (info.isSymLink() || info.isJunction()) return {};
    if (!info.exists()) return QByteArray(16, '\0');
    if (!info.isFile() || !file.open(QIODevice::ReadOnly) || file.size() != 16) return {};
    return file.readAll();
}
bool GroupSessionStore::save(const QString &key, const QByteArray &bytes, const QString &suffix) {
    const QFileInfo info(path(key) + suffix);
    if (info.isSymLink() || info.isJunction()) return fail("The Society state record is redirected.");
    iiFileProvider::SaveFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        || file.write(bytes) != bytes.size() || !file.commit()) return fail("The Society state record could not be committed.");
#ifdef Q_OS_WIN
    return protectWindowsState(info.absoluteFilePath(), false) || fail("Society state record ACL could not be applied.");
#elif defined(Q_OS_DARWIN)
    return societyProtectStatePath(info.absoluteFilePath()) || fail("Society state record protection could not be applied.");
#else
    return true;
#endif
}
void GroupSessionStore::read(const QString &key, Completion completion) {
    if ((m_followsSelection || !m_previousDirectory.isEmpty()) && !prepare()) {
        QTimer::singleShot(0, this, [completion = std::move(completion)] { completion({Error::Unavailable, {}}); });
        return;
    }
    const auto token = generation(key); m_observedGenerations[key] = token;
    enqueue({Operation::Read, key, {}, std::move(completion), m_revisions.value(key), token});
}
void GroupSessionStore::write(const QString &key, const QByteArray &data, Completion completion) {
    if (m_followsSelection && !prepare()) {
        QTimer::singleShot(0, this, [completion = std::move(completion)] { completion({Error::Unavailable, {}}); });
        return;
    }
    if (!m_observedGenerations.contains(key)) m_observedGenerations[key] = generation(key);
    enqueue({Operation::Write, key, data, std::move(completion), ++m_revisions[key], m_observedGenerations.value(key)});
}
void GroupSessionStore::remove(const QString &key, Completion completion) {
    const auto revision = ++m_revisions[key];
    // Commit revocation before asynchronous Keychain work or process exit.
    // The generation marker also invalidates an older write in another process.
    // A racing ciphertext write cannot make that generation readable again.
    const auto token = QUuid::createUuid().toRfc4122();
    bool ok = prepare() && save(key, token, ".revocation");
    if (ok) m_observedGenerations[key] = token;
    if (ok) save(key, tombstone);
    if (!ok) { completion({Error::Unavailable, {}}); return; }
    enqueue({Operation::Remove, key, {}, std::move(completion), revision, token});
}
void GroupSessionStore::enqueue(Request request) {
    m_pending.enqueue(std::move(request));
    if (!m_running) { m_running = true; QTimer::singleShot(0, this, [this] { next(); }); }
}
void GroupSessionStore::next() {
    if (m_pending.isEmpty()) { m_running = false; return; }
    auto request = m_pending.dequeue();
    m_error.clear();
    if (request.revision != m_revisions.value(request.key) || request.generation != generation(request.key)) { finish(std::move(request), {Error::Missing, {}}); return; }
    if (request.key.isEmpty() || request.key.size() > 512 || request.generation.size() != 16 || request.data.size() > MaximumSocietyStateBytes || !prepare()) {
        finish(std::move(request), {Error::Unavailable, {}}); return;
    }
    m_lock = std::make_unique<iiFileProvider::LockFile>(iiFileProvider::Directory(m_directory).filePath("state.lock"));
    if (!request.lockDeadline) request.lockDeadline = QDateTime::currentMSecsSinceEpoch() + 10000;
    if (!m_lock->tryLock(0)) {
        m_lock.reset();
        if (QDateTime::currentMSecsSinceEpoch() < request.lockDeadline) {
            m_pending.prepend(std::move(request)); QTimer::singleShot(50, this, [this] { next(); }); return;
        }
        qWarning("Society group state remained busy in another store."); finish(std::move(request), {Error::Unavailable, {}}); return;
    }
    if (request.operation == Operation::Remove) {
        if (!m_legacy) { finish(std::move(request), {}); return; }
        const QPointer<GroupSessionStore> guard(this);
        m_legacy->remove(request.key, [guard, request](Result) mutable { if (guard) guard->finish(std::move(request), {}); });
        return;
    }
    if (request.operation == Operation::Write) {
        withKey(std::move(request), true, [this](Request r, QByteArray key) { store(std::move(r), key); }); return;
    }
    if (!m_previousDirectory.isEmpty() && !QFileInfo::exists(path(request.key))
        && !QFileInfo::exists(path(request.key) + ".revocation")) {
        if (!migrateRecord(request.key)) { finish(std::move(request), {Error::Unavailable, {}}); return; }
        request.generation = generation(request.key);
        m_observedGenerations[request.key] = request.generation;
    }
    iiFileProvider::FileDevice file(path(request.key)); const QFileInfo info(file);
    if (info.isSymLink() || info.isJunction() || (info.exists() && (!info.isFile() || !file.open(QIODevice::ReadOnly) || file.size() > MaximumSocietyStateBytes + 48))) {
        finish(std::move(request), {Error::Unavailable, {}}); return;
    }
    if (!info.exists()) {
        if (request.generation != QByteArray(16, '\0')) { finish(std::move(request), {Error::Missing, {}}); return; }
        if (!m_legacy) { finish(std::move(request), {Error::Missing, {}}); return; }
        const QPointer<GroupSessionStore> guard(this);
        m_legacy->read(request.key, [guard, request](Result result) mutable {
            if (!guard) return;
            if (result.error != Error::None) { guard->finish(std::move(request), result); return; }
            request.data = result.data;
            guard->withKey(std::move(request), true, [guard](Request r, QByteArray key) {
                if (guard) guard->store(std::move(r), key, true);
            });
        }); return;
    }
    const auto bytes = file.readAll();
    if (bytes == tombstone) { finish(std::move(request), {Error::Missing, {}}); return; }
    if (!bytes.startsWith(version)) { finish(std::move(request), {Error::Unavailable, {}}); return; }
    if (bytes.mid(4, 16) != request.generation) { finish(std::move(request), {Error::Missing, {}}); return; }
    withKey(std::move(request), false, [this, bytes](Request r, QByteArray key) {
        const auto plain = openSocietyState(bytes.mid(20), key, context(r.key) + r.generation);
        finish(std::move(r), {plain.isEmpty() ? Error::Unavailable : Error::None, plain});
    });
}
void GroupSessionStore::withKey(Request request, bool create, std::function<void(Request, QByteArray)> continuation) {
    const QPointer<GroupSessionStore> guard(this);
    m_keys->read(masterKey, [guard, request, create, continuation](Result result) mutable {
        if (!guard) return;
        if (result.error == Error::None && result.data.size() == 32) { continuation(std::move(request), result.data); return; }
        if (result.error != Error::Missing || !create) { guard->fail("The Society group encryption key is unavailable."); guard->finish(std::move(request), {Error::Unavailable, {}}); return; }
        QByteArray key(32, Qt::Uninitialized);
        for (int index = 0; index < 32; index += 4) {
            const auto random = QRandomGenerator::system()->generate(); memcpy(key.data() + index, &random, 4);
        }
        guard->m_keys->write(masterKey, key, [guard, request, key, continuation](Result saved) mutable {
            if (!guard) return;
            if (saved.error != Error::None) { guard->fail("The Society group encryption key could not be saved."); guard->finish(std::move(request), {Error::Unavailable, {}}); return; }
            continuation(std::move(request), key);
        });
    });
}
void GroupSessionStore::store(Request request, const QByteArray &key, bool migrating) {
    if (m_followsSelection && defaultDirectory() != m_directory) {
        fail("The selected Society container changed before the state write.");
        finish(std::move(request), {Error::Unavailable, {}}); return;
    }
    if (request.revision != m_revisions.value(request.key) || request.generation != generation(request.key)) { finish(std::move(request), {Error::Missing, {}}); return; }
    const auto sealed = sealSocietyState(request.data, key, context(request.key) + request.generation);
    if (sealed.isEmpty() || !save(request.key, QByteArray(version) + request.generation + sealed)) { finish(std::move(request), {Error::Unavailable, {}}); return; }
    if (migrating && m_legacy) {
        const QPointer<GroupSessionStore> guard(this);
        m_legacy->remove(request.key, [guard, request](Result) mutable {
            if (guard) { const auto data = request.data; guard->finish(std::move(request), {Error::None, data}); }
        });
    } else finish(std::move(request), {});
}
void GroupSessionStore::finish(Request request, Result result) {
    m_lock.reset();
    if (request.revision != m_revisions.value(request.key) || request.generation != generation(request.key)) result = {Error::Missing, {}};
    const QPointer<GroupSessionStore> guard(this);
    request.completion(std::move(result));
    if (guard) QTimer::singleShot(0, this, [this] { next(); });
}
