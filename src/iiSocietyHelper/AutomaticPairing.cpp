#include "AutomaticPairing.h"
#include <QScopedValueRollback>
#include <algorithm>

AutomaticPairing::AutomaticPairing(NearbyDevices *nearby, iiServerHost::LanPeer *peer,
    std::function<bool()> host, std::function<bool(const QString &)> join, std::function<void()> stop, QObject *parent)
    : QObject(parent), m_nearby(nearby), m_peer(peer), m_host(std::move(host)), m_join(std::move(join)), m_stop(std::move(stop)) {
    m_timer.setInterval(500);
    connect(&m_timer, &QTimer::timeout, this, &AutomaticPairing::pump); m_timer.start();
    connect(nearby, &NearbyDevices::changed, this, &AutomaticPairing::schedule);
    connect(peer, &iiServerHost::LanPeer::changed, this, &AutomaticPairing::schedule);
    connect(nearby, &NearbyDevices::identityEnding, this, &AutomaticPairing::reset);
    connect(nearby, &NearbyDevices::authenticationEnding, this, &AutomaticPairing::reset);
    connect(nearby, &NearbyDevices::invitationEnded, this, [this] { if (active()) finish(false); });
    connect(peer, &iiServerHost::LanPeer::paired, this, [this](const QString &id) {
        if (id == m_current) finish(true);
    });
}
void AutomaticPairing::schedule() {
    if (m_scheduled) return;
    m_scheduled = true;
    QTimer::singleShot(0, this, [this] { m_scheduled = false; pump(); });
}
void AutomaticPairing::reset() {
    const bool owned = m_ownsTransport; m_ownsTransport = false;
    m_current.clear(); m_queue.clear(); m_leader.clear(); m_retryAt.clear(); m_attempts.clear();
    if (owned) { m_nearby->cancel(); m_stop(); }
    emit changed();
}
void AutomaticPairing::setEnabled(bool enabled) {
    if (m_enabled == enabled) return;
    m_enabled = enabled;
    if (!enabled) {
        // Manual pairing may keep existing connections, but takes over new work.
        if (active()) { m_nearby->cancel(); m_peer->cancelPairing(); }
        m_current.clear(); m_queue.clear();
    }
    emit changed();
    if (enabled) schedule();
}
QVariantList AutomaticPairing::queue() const {
    QVariantList result;
    for (const auto &id : m_queue) result.append(QVariantMap{{"id", id},
        {"state", id == m_current ? "connecting" : m_retryAt.value(id) > QDateTime::currentMSecsSinceEpoch() ? "retrying" : "queued"}});
    return result;
}
QString AutomaticPairing::status() const {
    if (!m_enabled) return tr("Automatic pairing is paused.");
    if (!m_nearby->authenticated()) return tr("Sign in to prepare local automatic pairing on this device.");
    if (active()) return tr("Pairing with a verified device on your local network…");
    if (!m_queue.isEmpty()) return tr("%1 device(s) waiting to pair. Failed connections retry automatically.").arg(m_queue.size());
    if (m_leader.isEmpty()) return tr("Waiting for a desktop with an open Society container.");
    return tr("Verified devices on this account connect automatically over the local network.");
}
void AutomaticPairing::finish(bool success) {
    if (m_current.isEmpty()) return;
    const auto id = m_current; m_current.clear();
    if (success) { m_retryAt.remove(id); m_attempts.remove(id); m_nearby->complete(); }
    else {
        const int attempts = qMin(m_attempts.value(id) + 1, 5); m_attempts[id] = attempts;
        m_retryAt[id] = QDateTime::currentMSecsSinceEpoch() + qMin(8000, 250 * (1 << (attempts - 1)));
        m_nearby->cancel(); m_peer->cancelPairing();
    }
    emit changed();
    schedule();
}
void AutomaticPairing::pump() {
    if (m_pumping || !m_nearby->authenticated()) return;
    if (!m_enabled) {
        if (m_nearby->incomingAutomatic()) m_nearby->decline();
        return;
    }
    QScopedValueRollback guard(m_pumping, true);
    const auto rows = m_nearby->devices();
    QStringList devices, hosts, established;
    for (const auto &value : rows) {
        const auto row = value.toMap(); if (!row.value("verified").toBool()) continue;
        const auto id = row.value("id").toString(); devices.append(id);
        if (row.value("autoHost").toBool()) hosts.append(id);
        const auto primary = row.value("primary").toString();
        if (!primary.isEmpty()) established.append(primary);
    }
    if (m_nearby->automaticHostAvailable()) hosts.append(m_nearby->deviceId());
    std::sort(hosts.begin(), hosts.end()); std::sort(devices.begin(), devices.end());
    auto leader = hosts.isEmpty() ? QString() : hosts.first();
    established.removeDuplicates(); established.sort();
    // A replica follows its durable primary even while that host is offline.
    // Existing drive owners outrank a newly discovered independent desktop.
    if (!m_nearby->primaryHost().isEmpty()) leader = m_nearby->primaryHost();
    else if (!established.isEmpty()) leader = established.first();
    // DNS-SD records can briefly disappear during key or interface refresh.
    // An authenticated, still-open TLS connection does not depend on that event.
    if (m_nearby->primaryHost().isEmpty() && m_ownsTransport && m_peer->connected() && !devices.contains(m_leader)) leader = m_leader;
    if (leader != m_leader) {
        if (active()) finish(false);
        if (m_ownsTransport) { m_ownsTransport = false; m_stop(); }
        m_leader = leader; emit changed();
    }
    const bool host = !leader.isEmpty() && leader == m_nearby->deviceId();
    const auto connected = m_peer->pairedDeviceIds();
    QStringList queue;
    for (const auto &id : devices) if (!connected.contains(id) && (host || id == leader)) queue.append(id);
    if (queue != m_queue) { m_queue = queue; emit changed(); }
    for (const auto &id : m_retryAt.keys()) if (!devices.contains(id)) { m_retryAt.remove(id); m_attempts.remove(id); }
    if (active()) {
        if (!devices.contains(m_current) || QDateTime::currentMSecsSinceEpoch() >= m_deadline || m_peer->phase() == "error") { finish(false); return; }
        if (m_peer->phase() == "confirming") {
            if (host) {
                const auto code = m_nearby->remoteVerificationCode();
                if (!code.isEmpty()) {
                    if (code != m_peer->verificationCode() || !m_peer->confirmDevice()) finish(false);
                }
            } else m_nearby->proveConnection(m_peer->verificationCode());
        }
        return;
    }
    if (m_nearby->hasIncoming() && m_nearby->incomingAutomatic()) {
        if (host || m_nearby->incomingId() != leader || connected.contains(leader)) { m_nearby->decline(); return; }
        m_current = leader; m_deadline = QDateTime::currentMSecsSinceEpoch() + 20000;
        const auto link = m_nearby->accept(); m_ownsTransport = true;
        if (link.isEmpty() || !m_join(link)) finish(false);
        emit changed(); return;
    }
    if (!host || m_queue.isEmpty() || m_nearby->hasIncoming()) return;
    // Do not replace an explicit QR offer or an in-flight manual invitation.
    if (QStringList{"showing", "connecting", "verifying", "confirming"}.contains(m_peer->phase())) return;
    for (const auto &id : m_queue) {
        if (m_retryAt.value(id) > QDateTime::currentMSecsSinceEpoch()) continue;
        m_current = id; m_deadline = QDateTime::currentMSecsSinceEpoch() + 20000; m_ownsTransport = true;
        if (!m_host() || !m_nearby->invite(id, m_peer->createDeviceOffer(id), true)) finish(false);
        emit changed(); return;
    }
}
