#include "Remote.h"
#include <QCryptographicHash>
#include <QUuid>

namespace iiSocietyHelper::Generation {
Remote::Remote(Sender send, QObject *parent) : QObject(parent), m_send(std::move(send)) {
    m_timer.setInterval(250);
    connect(&m_timer, &QTimer::timeout, this, &Remote::tick);
}
bool Remote::start(const QJsonObject &job) {
    if (active() || QUuid(job.value("id").toString()).isNull()) return false;
    m_job = job; m_result = {}; m_image.clear(); m_pending.clear();
    m_accepted = m_cancelled = false; m_disconnected.start();
    m_timer.start(); QTimer::singleShot(0, this, &Remote::tick); return true;
}
void Remote::setPaused(bool paused) {
    m_paused = paused; m_pending.clear(); m_disconnected.restart();
    if (!paused) QTimer::singleShot(0, this, &Remote::tick);
}
void Remote::cancel() {
    if (!active()) return;
    m_cancelled = true; m_pending.clear(); tick();
}
void Remote::tick() {
    if (!active() || m_paused) return;
    if (!m_pending.isEmpty() && m_pendingAge.elapsed() < 10000) return;
    m_pending.clear();
    if (m_disconnected.elapsed() > 120000) {
        complete("failed", m_cancelled ? "The host could not confirm cancellation. Reconnect to Society to inspect the host job."
                                      : "The Society host is unavailable. Reconnect and try again."); return;
    }
    m_action = m_cancelled ? "cancel" : !m_accepted ? "submit" : m_result.isEmpty() ? "status" : "read";
    QJsonObject request{{"op", "society.generation"}, {"schema", 1}, {"action", m_action}, {"id", m_job.value("id")}};
    if (m_action == "submit") request["job"] = m_job;
    if (m_action == "read") request["offset"] = double(m_image.size());
    m_pending = m_send(request);
    m_pendingAge.restart();
    if (m_pending.isEmpty()) emit progress({{"state", "waiting-host"}, {"backend", "remote"}, {"ready", false}});
}
void Remote::receive(const QString &id, const QJsonObject &response) {
    if (!active() || m_paused || id != m_pending || id.isEmpty()) return;
    m_pending.clear();
    if (!response.value("ok").toBool()) {
        const auto error = response.value("error").toString();
        if (QStringList{"timeout", "peer_unavailable", "host_unavailable", "disconnected", "connection_lost",
                        "not_connected_or_busy", "stopped", "transport_unavailable"}.contains(error)) return;
        complete("failed", error.isEmpty() ? "The Society host rejected remote generation." : error); return;
    }
    m_disconnected.restart();
    if (response.value("schema").toInt() != 1 || response.value("id") != m_job.value("id")) {
        complete("failed", "The Society host returned an invalid generation response."); return;
    }
    if (m_action == "read") {
        const auto encoded = response.value("data").toString().toLatin1();
        const auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
        const auto size = m_result.value("bytes").toInteger();
        if (!decoded || encoded.size() > 350000 || decoded.decoded.isEmpty()
            || response.value("offset").toInteger(-1) != m_image.size() || m_image.size() + decoded.decoded.size() > size) {
            complete("failed", "The remote image transfer is invalid."); return;
        }
        m_image.append(decoded.decoded);
        if (m_image.size() == size) {
            if (QCryptographicHash::hash(m_image, QCryptographicHash::Sha256).toHex() != m_result.value("sha256").toString().toLatin1())
                complete("failed", "The remote image checksum does not match.");
            else complete("completed");
        } else QTimer::singleShot(0, this, &Remote::tick);
        return;
    }
    if (!m_accepted && m_action == "submit") { m_accepted = true; emit accepted(); if (!active()) return; }
    const auto state = response.value("state").toString();
    if (state == "failed" || state == "cancelled") { complete(state, response.value("error").toString()); return; }
    if (state == "completed") {
        const auto bytes = response.value("bytes").toInteger();
        if (bytes <= 0 || bytes > 64 * 1024 * 1024 || response.value("sha256").toString().size() != 64) {
            complete("failed", "The remote image exceeds the supported transfer size."); return;
        }
        m_result = response;
        if (m_cancelled) { complete("cancelled"); return; }
        QTimer::singleShot(0, this, &Remote::tick);
    } else if (state != "queued" && state != "running" && state != "cancelling") {
        complete("failed", "The Society host returned an invalid job state."); return;
    }
    emit progress({{"state", state == "completed" ? "receiving-image" : "remote-" + state},
        {"backend", "remote"}, {"ready", false}, {"phase", response.value("phase")},
        {"step", response.value("step")}, {"total", response.value("total")}});
}
void Remote::complete(const QString &state, const QString &error) {
    const auto image = state == "completed" ? m_image : QByteArray();
    const auto generation = m_result.value("generation").toObject();
    m_job = {}; m_pending.clear(); m_image.clear(); m_result = {}; m_timer.stop();
    emit finished(state, image, generation, error);
}
}
