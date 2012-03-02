/****************************************************************************
**
** Copyright (C) 2012 Nokia Corporation and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/
**
** This file is part of the QtQml module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** GNU Lesser General Public License Usage
** This file may be used under the terms of the GNU Lesser General Public
** License version 2.1 as published by the Free Software Foundation and
** appearing in the file LICENSE.LGPL included in the packaging of this
** file. Please review the following information to ensure the GNU Lesser
** General Public License version 2.1 requirements will be met:
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Nokia gives you certain additional
** rights. These rights are described in the Nokia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU General
** Public License version 3.0 as published by the Free Software Foundation
** and appearing in the file LICENSE.GPL included in the packaging of this
** file. Please review the following information to ensure the GNU General
** Public License version 3.0 requirements will be met:
** http://www.gnu.org/copyleft/gpl.html.
**
** Other Usage
** Alternatively, this file may be used in accordance with the terms and
** conditions contained in a signed written agreement between you and Nokia.
**
**
**
**
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qqmldebugclient_p.h"

#include "qpacketprotocol_p.h"

#include <QtCore/qdebug.h>
#include <QtCore/qstringlist.h>
#include <QtNetwork/qnetworkproxy.h>

#include <private/qobject_p.h>

QT_BEGIN_NAMESPACE

const int protocolVersion = 1;
const QString serverId = QLatin1String("QQmlDebugServer");
const QString clientId = QLatin1String("QQmlDebugClient");

class QQmlDebugClientPrivate : public QObjectPrivate
{
    Q_DECLARE_PUBLIC(QQmlDebugClient)
public:
    QQmlDebugClientPrivate();

    QString name;
    QQmlDebugConnection *connection;
};

class QQmlDebugConnectionPrivate : public QObject
{
    Q_OBJECT
public:
    QQmlDebugConnectionPrivate(QQmlDebugConnection *c);
    QQmlDebugConnection *q;
    QPacketProtocol *protocol;
    QIODevice *device;

    bool gotHello;
    QHash <QString, float> serverPlugins;
    QHash<QString, QQmlDebugClient *> plugins;

    void advertisePlugins();
    void connectDeviceSignals();

public Q_SLOTS:
    void connected();
    void readyRead();
    void deviceAboutToClose();
};

QQmlDebugConnectionPrivate::QQmlDebugConnectionPrivate(QQmlDebugConnection *c)
    : QObject(c), q(c), protocol(0), device(0), gotHello(false)
{
    protocol = new QPacketProtocol(q, this);
    QObject::connect(c, SIGNAL(connected()), this, SLOT(connected()));
    QObject::connect(protocol, SIGNAL(readyRead()), this, SLOT(readyRead()));
}

void QQmlDebugConnectionPrivate::advertisePlugins()
{
    if (!q->isConnected())
        return;

    QPacket pack;
    pack << serverId << 1 << plugins.keys();
    protocol->send(pack);
    q->flush();
}

void QQmlDebugConnectionPrivate::connected()
{
    QPacket pack;
    pack << serverId << 0 << protocolVersion << plugins.keys();
    protocol->send(pack);
    q->flush();
}

void QQmlDebugConnectionPrivate::readyRead()
{
    if (!gotHello) {
        QPacket pack = protocol->read();
        QString name;

        pack >> name;

        bool validHello = false;
        if (name == clientId) {
            int op = -1;
            pack >> op;
            if (op == 0) {
                int version = -1;
                pack >> version;
                if (version == protocolVersion) {
                    QStringList pluginNames;
                    QList<float> pluginVersions;
                    pack >> pluginNames;
                    if (!pack.isEmpty())
                        pack >> pluginVersions;

                    const int pluginNamesSize = pluginNames.size();
                    const int pluginVersionsSize = pluginVersions.size();
                    for (int i = 0; i < pluginNamesSize; ++i) {
                        float pluginVersion = 1.0;
                        if (i < pluginVersionsSize)
                            pluginVersion = pluginVersions.at(i);
                        serverPlugins.insert(pluginNames.at(i), pluginVersion);
                    }

                    validHello = true;
                }
            }
        }

        if (!validHello) {
            qWarning("QQmlDebugConnection: Invalid hello message");
            QObject::disconnect(protocol, SIGNAL(readyRead()), this, SLOT(readyRead()));
            return;
        }
        gotHello = true;

        QHash<QString, QQmlDebugClient *>::Iterator iter = plugins.begin();
        for (; iter != plugins.end(); ++iter) {
            QQmlDebugClient::State newState = QQmlDebugClient::Unavailable;
            if (serverPlugins.contains(iter.key()))
                newState = QQmlDebugClient::Enabled;
            iter.value()->stateChanged(newState);
        }
    }

    while (protocol->packetsAvailable()) {
        QPacket pack = protocol->read();
        QString name;
        pack >> name;

        if (name == clientId) {
            int op = -1;
            pack >> op;

            if (op == 1) {
                // Service Discovery
                QHash<QString, float> oldServerPlugins = serverPlugins;
                serverPlugins.clear();

                QStringList pluginNames;
                QList<float> pluginVersions;
                pack >> pluginNames;
                if (!pack.isEmpty())
                    pack >> pluginVersions;

                const int pluginNamesSize = pluginNames.size();
                const int pluginVersionsSize = pluginVersions.size();
                for (int i = 0; i < pluginNamesSize; ++i) {
                    float pluginVersion = 1.0;
                    if (i < pluginVersionsSize)
                        pluginVersion = pluginVersions.at(i);
                    serverPlugins.insert(pluginNames.at(i), pluginVersion);
                }

                QHash<QString, QQmlDebugClient *>::Iterator iter = plugins.begin();
                for (; iter != plugins.end(); ++iter) {
                    const QString pluginName = iter.key();
                    QQmlDebugClient::State newSate = QQmlDebugClient::Unavailable;
                    if (serverPlugins.contains(pluginName))
                        newSate = QQmlDebugClient::Enabled;

                    if (oldServerPlugins.contains(pluginName)
                            != serverPlugins.contains(pluginName)) {
                        iter.value()->stateChanged(newSate);
                    }
                }
            } else {
                qWarning() << "QQmlDebugConnection: Unknown control message id" << op;
            }
        } else {
            QByteArray message;
            pack >> message;

            QHash<QString, QQmlDebugClient *>::Iterator iter =
                    plugins.find(name);
            if (iter == plugins.end()) {
                qWarning() << "QQmlDebugConnection: Message received for missing plugin" << name;
            } else {
                (*iter)->messageReceived(message);
            }
        }
    }
}

void QQmlDebugConnectionPrivate::deviceAboutToClose()
{
    // This is nasty syntax but we want to emit our own aboutToClose signal (by calling QIODevice::close())
    // without calling the underlying device close fn as that would cause an infinite loop
    q->QIODevice::close();
}

QQmlDebugConnection::QQmlDebugConnection(QObject *parent)
    : QIODevice(parent), d(new QQmlDebugConnectionPrivate(this))
{
}

QQmlDebugConnection::~QQmlDebugConnection()
{
    QHash<QString, QQmlDebugClient*>::iterator iter = d->plugins.begin();
    for (; iter != d->plugins.end(); ++iter) {
        iter.value()->d_func()->connection = 0;
        iter.value()->stateChanged(QQmlDebugClient::NotConnected);
    }
}

bool QQmlDebugConnection::isConnected() const
{
    return state() == QAbstractSocket::ConnectedState;
}

qint64 QQmlDebugConnection::readData(char *data, qint64 maxSize)
{
    return d->device->read(data, maxSize);
}

qint64 QQmlDebugConnection::writeData(const char *data, qint64 maxSize)
{
    return d->device->write(data, maxSize);
}

qint64 QQmlDebugConnection::bytesAvailable() const
{
    return d->device->bytesAvailable();
}

bool QQmlDebugConnection::isSequential() const
{
    return true;
}

void QQmlDebugConnection::close()
{
    if (isOpen()) {
        QIODevice::close();
        d->device->close();
        emit stateChanged(QAbstractSocket::UnconnectedState);

        QHash<QString, QQmlDebugClient*>::iterator iter = d->plugins.begin();
        for (; iter != d->plugins.end(); ++iter) {
            iter.value()->stateChanged(QQmlDebugClient::NotConnected);
        }
    }
}

bool QQmlDebugConnection::waitForConnected(int msecs)
{
    QAbstractSocket *socket = qobject_cast<QAbstractSocket*>(d->device);
    if (socket)
        return socket->waitForConnected(msecs);
    return false;
}

QAbstractSocket::SocketState QQmlDebugConnection::state() const
{
    QAbstractSocket *socket = qobject_cast<QAbstractSocket*>(d->device);
    if (socket)
        return socket->state();

    return QAbstractSocket::UnconnectedState;
}

void QQmlDebugConnection::flush()
{
    QAbstractSocket *socket = qobject_cast<QAbstractSocket*>(d->device);
    if (socket) {
        socket->flush();
        return;
    }
}

void QQmlDebugConnection::connectToHost(const QString &hostName, quint16 port)
{
    QTcpSocket *socket = new QTcpSocket(d);
    socket->setProxy(QNetworkProxy::NoProxy);
    d->device = socket;
    d->connectDeviceSignals();
    d->gotHello = false;
    connect(socket, SIGNAL(stateChanged(QAbstractSocket::SocketState)), this, SIGNAL(stateChanged(QAbstractSocket::SocketState)));
    connect(socket, SIGNAL(error(QAbstractSocket::SocketError)), this, SIGNAL(error(QAbstractSocket::SocketError)));
    connect(socket, SIGNAL(connected()), this, SIGNAL(connected()));
    socket->connectToHost(hostName, port);
    QIODevice::open(ReadWrite | Unbuffered);
}

void QQmlDebugConnectionPrivate::connectDeviceSignals()
{
    connect(device, SIGNAL(bytesWritten(qint64)), q, SIGNAL(bytesWritten(qint64)));
    connect(device, SIGNAL(readyRead()), q, SIGNAL(readyRead()));
    connect(device, SIGNAL(aboutToClose()), this, SLOT(deviceAboutToClose()));
}

//

QQmlDebugClientPrivate::QQmlDebugClientPrivate()
    : connection(0)
{
}

QQmlDebugClient::QQmlDebugClient(const QString &name, 
                                                 QQmlDebugConnection *parent)
    : QObject(*(new QQmlDebugClientPrivate), parent)
{
    Q_D(QQmlDebugClient);
    d->name = name;
    d->connection = parent;

    if (!d->connection)
        return;

    if (d->connection->d->plugins.contains(name)) {
        qWarning() << "QQmlDebugClient: Conflicting plugin name" << name;
        d->connection = 0;
    } else {
        d->connection->d->plugins.insert(name, this);
        d->connection->d->advertisePlugins();
    }
}

QQmlDebugClient::~QQmlDebugClient()
{
    Q_D(QQmlDebugClient);
    if (d->connection && d->connection->d) {
        d->connection->d->plugins.remove(d->name);
        d->connection->d->advertisePlugins();
    }
}

QString QQmlDebugClient::name() const
{
    Q_D(const QQmlDebugClient);
    return d->name;
}

float QQmlDebugClient::serviceVersion() const
{
    Q_D(const QQmlDebugClient);
    if (d->connection->d->serverPlugins.contains(d->name))
        return d->connection->d->serverPlugins.value(d->name);
    return -1;
}

QQmlDebugClient::State QQmlDebugClient::state() const
{
    Q_D(const QQmlDebugClient);
    if (!d->connection
            || !d->connection->isConnected()
            || !d->connection->d->gotHello)
        return NotConnected;

    if (d->connection->d->serverPlugins.contains(d->name))
        return Enabled;

    return Unavailable;
}

void QQmlDebugClient::sendMessage(const QByteArray &message)
{
    Q_D(QQmlDebugClient);
    if (state() != Enabled)
        return;

    QPacket pack;
    pack << d->name << message;
    d->connection->d->protocol->send(pack);
    d->connection->flush();
}

void QQmlDebugClient::stateChanged(State)
{
}

void QQmlDebugClient::messageReceived(const QByteArray &)
{
}

QT_END_NAMESPACE

#include <qqmldebugclient.moc>