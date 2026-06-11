#ifndef ETHERNETDISABLE_H
#define ETHERNETDISABLE_H

#include <QObject>

class EthernetDisable : public QObject
{
    Q_OBJECT
public:
    explicit EthernetDisable(QObject *parent = nullptr);

    // Start monitoring and disable Ethernet when traffic = 0
    void monitorAndDisableEthernet(int idleSeconds);
    Q_INVOKABLE void startEthernetWatcher(int idleSeconds);
    // bool disable = false ;
};

#endif // ETHERNETDISABLE_H
