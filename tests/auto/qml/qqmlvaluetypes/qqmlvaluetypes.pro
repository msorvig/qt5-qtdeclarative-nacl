CONFIG += testcase
TARGET = tst_qqmlvaluetypes
macx:CONFIG -= app_bundle

HEADERS += testtypes.h

SOURCES += tst_qqmlvaluetypes.cpp \
           testtypes.cpp

include (../../shared/util.pri)

testDataFiles.files = data
testDataFiles.path = .
DEPLOYMENT += testDataFiles

CONFIG += parallel_test

QT += core-private gui-private v8-private qml-private testlib