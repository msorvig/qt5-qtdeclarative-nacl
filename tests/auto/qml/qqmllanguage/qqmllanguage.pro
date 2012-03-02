CONFIG += testcase
TARGET = tst_qqmllanguage
macx:CONFIG -= app_bundle

SOURCES += tst_qqmllanguage.cpp \
           testtypes.cpp
HEADERS += testtypes.h

INCLUDEPATH += ../../shared/
HEADERS += ../../shared/testhttpserver.h
SOURCES += ../../shared/testhttpserver.cpp

importFiles.files = data
importFiles.path = .
DEPLOYMENT += importFiles

CONFIG += parallel_test
QT += core-private gui-private v8-private qml-private network testlib