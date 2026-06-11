

#include "EthernetDisable.h"

#include <QDebug>
#include <QNetworkInterface>
#include <QProcess>
#include <QThread>
#include <QtConcurrent>
#include <QElapsedTimer>
#include <QHostAddress>

EthernetDisable::EthernetDisable(QObject *parent)
    : QObject(parent)
{
}

#include <QAbstractSocket>

namespace {
// Helper: find an Ethernet interface named "Ethernet" (or starting with "Ethernet ") that is up and has an IPv4 address.
// Returns the human-readable adapter name and writes the IP address to outIp.
static QString findEthernetAdapter(QHostAddress &outIp)
{
    qDebug() << "[EthernetDisable] Scanning network interfaces...";
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        QString name = iface.humanReadableName();
        // qDebug() << "[EthernetDisable] Found interface:" << name << "Type:" << iface.type() << "IsUp:" << iface.flags().testFlag(QNetworkInterface::IsUp);

        // Must be an Ethernet-like interface and be up
        if (iface.type() != QNetworkInterface::Ethernet)
            continue;
        if (!iface.flags().testFlag(QNetworkInterface::IsUp))
            continue;

        // Check if name is exactly "Ethernet", "Ethernet 1", or "Ethernet1" (case-insensitive)
        if (name.compare("Ethernet", Qt::CaseInsensitive) == 0 ||
            name.compare("Ethernet 1", Qt::CaseInsensitive) == 0 ||
            name.compare("Ethernet1", Qt::CaseInsensitive) == 0)
        {
            qDebug() << "[EthernetDisable] Interface matched 1st Ethernet port:" << name;
            // Find its first IPv4 address
            for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
                QHostAddress ip = entry.ip();
                if (ip.protocol() == QAbstractSocket::IPv4Protocol) {
                    outIp = ip;
                    qDebug() << "[EthernetDisable] Selected dynamic IP:" << ip.toString() << "on" << name;
                    return name;
                } else {
                    qDebug() << "[EthernetDisable] Skipping non-IPv4 address:" << ip.toString();
                }
            }
            qDebug() << "[EthernetDisable] No IPv4 address found for interface:" << name;
        }
    }
    qDebug() << "[EthernetDisable] No matching active 1st Ethernet port found.";
    return QString();
}
}

void EthernetDisable::monitorAndDisableEthernet(int idleSeconds)
{
#ifndef Q_OS_WIN
    qDebug() << "Ethernet monitoring only supported on Windows.";
    return;
#endif

    QtConcurrent::run([=]() {

        // -------- STEP 0: Discover Ethernet adapter and its IP --------
        QHostAddress initialIp;
        QString adapterName = findEthernetAdapter(initialIp);
        if (adapterName.isEmpty()) {
            qDebug() << "No Ethernet interface found.";
            return;
        }

        qDebug() << "Monitoring traffic for interface" << adapterName
                 << "with IP" << initialIp.toString();

        // -------- STEP 1: RX/TX Byte reader --------
        struct Traffic {
            qint64 inBytes = -1;
            qint64 outBytes = -1;
        };

        auto getTraffic = [&](const QString &ifaceName) -> Traffic {
            Traffic t;
            QProcess p;
            p.start("netsh", {"interface", "ipv4", "show", "subinterfaces"});
            p.waitForFinished(1500);

            QString out = p.readAllStandardOutput();
            QStringList lines = out.split("\n", Qt::SkipEmptyParts);

            for (QString line : lines) {
                line = line.trimmed();

                // netsh output usually ends the line with the interface name
                if (line.endsWith(ifaceName)) {
                    QStringList parts = line.split(QRegExp("\\s+"), Qt::SkipEmptyParts);

                    // typical netsh subinterfaces columns: MTU  Bytes In  Bytes Out  Interface
                    // we guard with parts.size() >= 5 similar to original code
                    if (parts.size() >= 5) {
                        bool ok1 = false, ok2 = false;
                        t.inBytes  = parts[2].toLongLong(&ok1);
                        t.outBytes = parts[3].toLongLong(&ok2);

                        if (ok1 && ok2)
                            return t;
                    }
                }
            }
            return t;
        };

        // -------- STEP 2: Monitoring --------
        Traffic last = getTraffic(adapterName);
        if (last.inBytes < 0 || last.outBytes < 0) {
            qDebug() << "Failed to read Ethernet counters for adapter" << adapterName;
            return;
        }

        QElapsedTimer idleTimer;
        idleTimer.start();

        while (!QThread::currentThread()->isInterruptionRequested()) {
            QThread::sleep(1);

            // Re-validate that the interface is still there and has an IP.
            // If it was removed, stop monitoring.
            QHostAddress currentIp;
            QString currentAdapter = findEthernetAdapter(currentIp);
            if (currentAdapter.isEmpty() || currentAdapter != adapterName) {
                qDebug() << "Interface" << adapterName
                         << "is no longer present or has changed. Stopping monitor.";
                return;
            }

            Traffic now = getTraffic(adapterName);

            qint64 rxDiff = now.inBytes - last.inBytes;
            qint64 txDiff = now.outBytes - last.outBytes;

            last = now;

            qDebug() << "[Traffic]" << currentIp.toString() << "(" << adapterName << ")"
                     << " RX:" << rxDiff << "bytes/sec"
                     << " TX:" << txDiff << "bytes/sec";

            // ❗ IDLE CHECK ONLY ON RX
            if (rxDiff <= 0) {
                if (idleTimer.elapsed() > idleSeconds * 1000) {
                    qDebug() << "⚠ No RX traffic for" << idleSeconds
                             << "seconds → disabling adapter:" << adapterName;

                    QProcess::execute(
                        "netsh",
                        {"interface", "set", "interface", adapterName, "admin=disable"}
                        );

                    // 🔥 WAIT 15 SECONDS AND RE-ENABLE
                    qDebug() << "⏳ Waiting 15 seconds before enabling adapter...";
                    QThread::sleep(15);

                    QProcess::execute("netsh",
                                      {"interface", "set", "interface", adapterName, "admin=enable"});
                    qDebug() << "✔ Adapter enabled again.";

                    // After re-enable, start watcher again to wait for the IP to be present again
                    startEthernetWatcher(4);
                    return;
                }
            } else {
                idleTimer.restart();
            }
        }
    });
}

void EthernetDisable::startEthernetWatcher(int idleSeconds)
{
    QtConcurrent::run([=]() {

        qDebug() << "🔍 Ethernet watcher thread started.";

        bool lastState = false;
        QHostAddress currentIp;

        while (!QThread::currentThread()->isInterruptionRequested() &&
               !QCoreApplication::closingDown())
        {
            // find interface which is our Ethernet adapter and is up
            QString adapterName = findEthernetAdapter(currentIp);

            if (adapterName.isEmpty()) {
                // Adapter not active or has no IP yet
                lastState = false;
                QThread::sleep(1);
                continue;
            }

            // We still check link state using netsh to determine "Connected" text
            QProcess p;
            p.start("netsh", {"interface", "show", "interface", adapterName});
            p.waitForFinished(800);

            QString out = p.readAllStandardOutput();
            bool connected = out.contains("Connected");

            // Trigger only on state change: Disconnected -> Connected
            if (connected && !lastState) {
                qDebug() << "✔ Interface" << adapterName << "with IP" << currentIp.toString()
                         << "is CONNECTED → Starting monitor.";

                // Start monitoring ONLY this adapter
                monitorAndDisableEthernet(idleSeconds);
                return; // watcher returns because monitor will handle lifecycle
            }

            lastState = connected;
            QThread::sleep(1);
        }
    });
}
