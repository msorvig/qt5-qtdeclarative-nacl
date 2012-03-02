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

#ifndef QQMLINCUBATOR_P_H
#define QQMLINCUBATOR_P_H

#include <private/qintrusivelist_p.h>
#include <private/qqmlvme_p.h>
#include <private/qrecursionwatcher_p.h>
#include <private/qqmlengine_p.h>
#include <private/qqmlcontext_p.h>

//
//  W A R N I N G
//  -------------
//
// This file is not part of the Qt API.  It exists purely as an
// implementation detail.  This header file may change from version to
// version without notice, or even be removed.
//
// We mean it.
//

QT_BEGIN_NAMESPACE

class QQmlCompiledData;
class QQmlIncubator;
class QQmlIncubatorPrivate : public QQmlEnginePrivate::Incubator
{
public:
    QQmlIncubatorPrivate(QQmlIncubator *q, QQmlIncubator::IncubationMode m);
    ~QQmlIncubatorPrivate();

    QQmlIncubator *q;

    QQmlIncubator::Status calculateStatus() const;
    void changeStatus(QQmlIncubator::Status);
    QQmlIncubator::Status status;

    QQmlIncubator::IncubationMode mode;
    bool isAsynchronous;

    QList<QQmlError> errors;

    enum Progress { Execute, Completing, Completed };
    Progress progress;

    QQmlGuard<QObject> result;
    QQmlGuardedContextData rootContext;
    QQmlCompiledData *component;
    QQmlVME vme;
    QQmlVMEGuard vmeGuard;

    QQmlIncubatorPrivate *waitingOnMe;
    typedef QQmlEnginePrivate::Incubator QIPBase;
    QIntrusiveList<QIPBase, &QIPBase::nextWaitingFor> waitingFor;

    QRecursionNode recursion;

    void clear();

    void incubate(QQmlVME::Interrupt &i);
};

QT_END_NAMESPACE

#endif // QQMLINCUBATOR_P_H
