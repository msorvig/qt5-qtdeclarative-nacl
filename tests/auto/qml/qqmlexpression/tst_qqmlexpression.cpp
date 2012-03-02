/****************************************************************************
**
** Copyright (C) 2012 Nokia Corporation and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/
**
** This file is part of the test suite of the Qt Toolkit.
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

#include <qtest.h>
#include <QtQml/qqmlengine.h>
#include <QtQml/qqmlcomponent.h>
#include <QtQml/qqmlexpression.h>
#include <QtQml/qqmlscriptstring.h>
#include "../../shared/util.h"

class tst_qqmlexpression : public QQmlDataTest
{
    Q_OBJECT
public:
    tst_qqmlexpression() {}

private slots:
    void scriptString();
    void syntaxError();
};

class TestObject : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QQmlScriptString scriptString READ scriptString WRITE setScriptString)
    Q_PROPERTY(QQmlScriptString scriptStringError READ scriptStringError WRITE setScriptStringError)
public:
    TestObject(QObject *parent = 0) : QObject(parent) {}

    QQmlScriptString scriptString() const { return m_scriptString; }
    void setScriptString(QQmlScriptString scriptString) { m_scriptString = scriptString; }

    QQmlScriptString scriptStringError() const { return m_scriptStringError; }
    void setScriptStringError(QQmlScriptString scriptString) { m_scriptStringError = scriptString; }

private:
    QQmlScriptString m_scriptString;
    QQmlScriptString m_scriptStringError;
};

QML_DECLARE_TYPE(TestObject)

void tst_qqmlexpression::scriptString()
{
    qmlRegisterType<TestObject>("Test", 1, 0, "TestObject");

    QQmlEngine engine;
    QQmlComponent c(&engine, testFileUrl("scriptString.qml"));
    TestObject *testObj = qobject_cast<TestObject*>(c.create());
    QVERIFY(testObj != 0);

    QQmlScriptString script = testObj->scriptString();
    QCOMPARE(script.script(), QLatin1String("value1 + value2"));

    QQmlExpression expression(script);
    QVariant value = expression.evaluate();
    QCOMPARE(value.toInt(), 15);

    QQmlScriptString scriptError = testObj->scriptStringError();
    QCOMPARE(scriptError.script(), QLatin1String("value3 * 5"));

    //verify that the expression has the correct error location information
    QQmlExpression expressionError(scriptError);
    QVariant valueError = expressionError.evaluate();
    QVERIFY(!valueError.isValid());
    QVERIFY(expressionError.hasError());
    QQmlError error = expressionError.error();
    QCOMPARE(error.url(), c.url());
    QCOMPARE(error.line(), 8);
}

// QTBUG-21310 - crash test
void tst_qqmlexpression::syntaxError()
{
    QQmlEngine engine;
    QQmlExpression expression(engine.rootContext(), 0, "asd asd");
    QVariant v = expression.evaluate();
    QCOMPARE(v, QVariant());
}

QTEST_MAIN(tst_qqmlexpression)

#include "tst_qqmlexpression.moc"