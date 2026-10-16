#pragma once
#include <iiAcountManager/SessionStore.h>
#include <QHash>
#include <QTimer>

class MemorySessionStore : public iisacc::accounts::SessionStore {
public:
    using SessionStore::SessionStore;
    QHash<QString, QByteArray> values;
    bool unavailable = false, failRemove = false, holdRead = false;
    std::function<void()> releaseRead;
    void read(const QString &key, Completion done) override {
        auto run = [this, key, done] { done(unavailable ? Result{Error::Unavailable, {}}
            : values.contains(key) ? Result{Error::None, values.value(key)} : Result{Error::Missing, {}}); };
        if (holdRead) { holdRead = false; releaseRead = run; } else QTimer::singleShot(0, this, run);
    }
    void write(const QString &key, const QByteArray &data, Completion done) override {
        if (!unavailable) values[key] = data;
        QTimer::singleShot(0, this, [done, fail = unavailable] { done({fail ? Error::Unavailable : Error::None, {}}); });
    }
    void remove(const QString &key, Completion done) override {
        if (!failRemove) values.remove(key);
        QTimer::singleShot(0, this, [done, fail = failRemove] { done({fail ? Error::Unavailable : Error::None, {}}); });
    }
};
