

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

namespace {
// The IP to watch for on local machine:
const QHostAddress WATCH_IP(QStringLiteral("192.168.144.10"));
}

// Helper: find the interface human-readable name that has WATCH_IP assigned and is up.
// Returns empty string if not found.
static QString findInterfaceNameWithIp()
{
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        // Must be an Ethernet-like interface and be up
        if (iface.type() != QNetworkInterface::Ethernet)
            continue;
        if (!iface.flags().testFlag(QNetworkInterface::IsUp))
            continue;

        // Check address entries for the IPv4 address
        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            if (entry.ip() == WATCH_IP) {
                // return the human readable name (what netsh reports)
                return iface.humanReadableName();
            }
        }
    }
    return QString();
}

void EthernetDisable::monitorAndDisableEthernet(int idleSeconds)
{
#ifndef Q_OS_WIN
    qDebug() << "Ethernet monitoring only supported on Windows.";
    return;
#endif

    QtConcurrent::run([=]() {

        // -------- STEP 0: Discover interface by WATCH_IP --------
        QString adapterName = findInterfaceNameWithIp();
        if (adapterName.isEmpty()) {
            qDebug() << "No interface found with IP" << WATCH_IP.toString();
            return;
        }

        qDebug() << "Monitoring traffic for interface (by IP)" << WATCH_IP.toString()
                 << "-> adapter name:" << adapterName;

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

            // Re-validate that the interface still has the WATCH_IP and is up.
            // If it was removed or changed, stop monitoring.
            QString currentAdapter = findInterfaceNameWithIp();
            if (currentAdapter.isEmpty() || currentAdapter != adapterName) {
                qDebug() << "Interface with IP" << WATCH_IP.toString()
                << "is no longer present or has changed. Stopping monitor.";
                return;
            }

            Traffic now = getTraffic(adapterName);

            qint64 rxDiff = now.inBytes - last.inBytes;
            qint64 txDiff = now.outBytes - last.outBytes;

            last = now;

            qDebug() << "[Traffic]" << WATCH_IP.toString() << "(" << adapterName << ")"
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

        qDebug() << "🔍 Ethernet watcher thread started (watching IP)"
                 << WATCH_IP.toString();

        bool lastState = false;

        while (!QThread::currentThread()->isInterruptionRequested() &&
               !QCoreApplication::closingDown())
        {
            // find interface which has our IP and is up
            QString adapterName = findInterfaceNameWithIp();

            if (adapterName.isEmpty()) {
                // IP not present yet
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
                qDebug() << "✔ Interface with IP" << WATCH_IP.toString()
                    << "is CONNECTED → Starting monitor for adapter:" << adapterName;

                // Start monitoring ONLY this adapter (identified by the IP)
                monitorAndDisableEthernet(idleSeconds);
                return; // watcher returns because monitor will handle lifecycle
            }

            lastState = connected;
            QThread::sleep(1);
        }
    });
}
