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

#ifndef QQMLENGINE_H
#define QQMLENGINE_H

#include <QtCore/qurl.h>
#include <QtCore/qobject.h>
#include <QtCore/qmap.h>
#include <QtQml/qjsengine.h>
#include <QtQml/qqmlerror.h>
#include <QtQml/qqmldebug.h>

QT_BEGIN_HEADER

QT_BEGIN_NAMESPACE


class QQmlComponent;
class QQmlEnginePrivate;
class QQmlImportsPrivate;
class QQmlExpression;
class QQmlContext;
class QQmlType;
class QUrl;
class QScriptContext;
class QQmlImageProvider;
class QNetworkAccessManager;
class QQmlNetworkAccessManagerFactory;
class QQmlIncubationController;
class Q_QML_EXPORT QQmlEngine : public QJSEngine
{
    Q_PROPERTY(QString offlineStoragePath READ offlineStoragePath WRITE setOfflineStoragePath)
    Q_OBJECT
public:
    QQmlEngine(QObject *p = 0);
    virtual ~QQmlEngine();

    QQmlContext *rootContext() const;

    void clearComponentCache();

    QStringList importPathList() const;
    void setImportPathList(const QStringList &paths);
    void addImportPath(const QString& dir);

    QStringList pluginPathList() const;
    void setPluginPathList(const QStringList &paths);
    void addPluginPath(const QString& dir);

    bool importPlugin(const QString &filePath, const QString &uri, QString *errorString); // XXX: Qt 5: Remove this function
    bool importPlugin(const QString &filePath, const QString &uri, QList<QQmlError> *errors);

    void setNetworkAccessManagerFactory(QQmlNetworkAccessManagerFactory *);
    QQmlNetworkAccessManagerFactory *networkAccessManagerFactory() const;

    QNetworkAccessManager *networkAccessManager() const;

    void addImageProvider(const QString &id, QQmlImageProvider *);
    QQmlImageProvider *imageProvider(const QString &id) const;
    void removeImageProvider(const QString &id);

    void setIncubationController(QQmlIncubationController *);
    QQmlIncubationController *incubationController() const;

    void setOfflineStoragePath(const QString& dir);
    QString offlineStoragePath() const;

    QUrl baseUrl() const;
    void setBaseUrl(const QUrl &);

    bool outputWarningsToStandardError() const;
    void setOutputWarningsToStandardError(bool);

    void collectGarbage();
    
    static QQmlContext *contextForObject(const QObject *);
    static void setContextForObject(QObject *, QQmlContext *);

    enum ObjectOwnership { CppOwnership, JavaScriptOwnership };
    static void setObjectOwnership(QObject *, ObjectOwnership);
    static ObjectOwnership objectOwnership(QObject *);

protected:
    virtual bool event(QEvent *);

Q_SIGNALS:
    void quit();
    void warnings(const QList<QQmlError> &warnings);

private:
    Q_DISABLE_COPY(QQmlEngine)
    Q_DECLARE_PRIVATE(QQmlEngine)
};

QT_END_NAMESPACE

QT_END_HEADER

#endif // QQMLENGINE_H