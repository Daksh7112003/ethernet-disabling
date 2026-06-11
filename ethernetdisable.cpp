#include "EthernetDisable.h"

#include <QDebug>
#include <QNetworkInterface>
#include <QProcess>
#include <QThread>
#include <QtConcurrent>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QAbstractSocket>
#include <QFile>
#include <QTextStream>
#include <QCoreApplication>

EthernetDisable::EthernetDisable(QObject *parent)
    : QObject(parent)
{
}

namespace {
// Helper: find an Ethernet interface that is up and has an IPv4 address.
// On Windows, it looks for "Ethernet", "Ethernet 1", or "Ethernet1".
// On Android, it looks for "eth0" or "eth1".
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

        bool isMatched = false;
#if defined(Q_OS_WIN)
        // Check if name is exactly "Ethernet", "Ethernet 1", or "Ethernet1" (case-insensitive)
        if (name.compare(QStringLiteral("Ethernet"), Qt::CaseInsensitive) == 0 ||
            name.compare(QStringLiteral("Ethernet 1"), Qt::CaseInsensitive) == 0 ||
            name.compare(QStringLiteral("Ethernet1"), Qt::CaseInsensitive) == 0)
        {
            isMatched = true;
        }
#elif defined(Q_OS_ANDROID)
        // Check if name is exactly "eth0" or "eth1" (case-insensitive)
        if (name.compare(QStringLiteral("eth0"), Qt::CaseInsensitive) == 0 ||
            name.compare(QStringLiteral("eth1"), Qt::CaseInsensitive) == 0)
        {
            isMatched = true;
        }
#else
        // Fallback
        if (name.compare(QStringLiteral("Ethernet"), Qt::CaseInsensitive) == 0) {
            isMatched = true;
        }
#endif

        if (isMatched) {
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
#if !defined(Q_OS_WIN) && !defined(Q_OS_ANDROID)
    qDebug() << "Ethernet monitoring only supported on Windows and Android.";
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
#if defined(Q_OS_WIN)
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
#else
            // Read /proc/net/dev on Linux/Android
            QFile file(QStringLiteral("/proc/net/dev"));
            if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QTextStream in(&file);
                while (!in.atEnd()) {
                    QString line = in.readLine().trimmed();
                    if (line.startsWith(ifaceName + QStringLiteral(":"))) {
                        // Line looks like: eth0: 123456 123 0 0 ... 654321 321 0 0 ...
                        // Strip the interface name and colon
                        QString data = line.mid(ifaceName.length() + 1).trimmed();
                        QStringList parts = data.split(QRegExp("\\s+"), Qt::SkipEmptyParts);
                        if (parts.size() >= 9) {
                            bool ok1 = false, ok2 = false;
                            // parts[0] is Received bytes, parts[8] is Transmitted bytes
                            t.inBytes = parts[0].toLongLong(&ok1);
                            t.outBytes = parts[8].toLongLong(&ok2);
                            if (ok1 && ok2) {
                                return t;
                            }
                        }
                    }
                }
            }
#endif
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

#if defined(Q_OS_WIN)
                    QProcess::execute(
                        "netsh",
                        {"interface", "set", "interface", adapterName, "admin=disable"}
                        );
#elif defined(Q_OS_ANDROID)
                    QProcess::execute(
                        "su",
                        {"-c", QStringLiteral("ip link set %1 down").arg(adapterName)}
                        );
#endif

                    // 🔥 WAIT 15 SECONDS AND RE-ENABLE
                    qDebug() << "⏳ Waiting 15 seconds before enabling adapter...";
                    QThread::sleep(15);

#if defined(Q_OS_WIN)
                    QProcess::execute("netsh",
                                      {"interface", "set", "interface", adapterName, "admin=enable"});
#elif defined(Q_OS_ANDROID)
                    QProcess::execute("su",
                                      {"-c", QStringLiteral("ip link set %1 up").arg(adapterName)});
#endif
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

            bool connected = false;
#if defined(Q_OS_WIN)
            // We still check link state using netsh to determine "Connected" text
            QProcess p;
            p.start("netsh", {"interface", "show", "interface", adapterName});
            p.waitForFinished(800);

            QString out = p.readAllStandardOutput();
            connected = out.contains("Connected");
#else
            // On Android/Linux, check link carrier or assume connected if found
            QFile file(QStringLiteral("/sys/class/net/%1/operstate").arg(adapterName));
            if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QString state = file.readAll().trimmed();
                connected = (state == QStringLiteral("up") || state == QStringLiteral("unknown"));
            } else {
                connected = true; // Fallback
            }
#endif

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
