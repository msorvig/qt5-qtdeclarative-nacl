/****************************************************************************
**
** Copyright (C) 2013 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of the QtQml module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qquickwindow.h"
#include "qquickwindow_p.h"

#include "qquickitem.h"
#include "qquickitem_p.h"
#include "qquickevents_p_p.h"

#include <QtQuick/private/qsgrenderer_p.h>
#include <QtQuick/private/qsgtexture_p.h>
#include <QtQuick/private/qsgflashnode_p.h>

#include <private/qsgrenderloop_p.h>

#include <private/qguiapplication_p.h>
#include <QtGui/QInputMethod>

#include <private/qabstractanimation_p.h>

#include <QtGui/qpainter.h>
#include <QtGui/qevent.h>
#include <QtGui/qmatrix4x4.h>
#include <QtGui/qstylehints.h>
#include <QtCore/qvarlengtharray.h>
#include <QtCore/qabstractanimation.h>
#include <QtQml/qqmlincubator.h>

#include <QtQuick/private/qquickpixmapcache_p.h>

#include <private/qqmlprofilerservice_p.h>
#include <private/qqmlmemoryprofiler_p.h>

QT_BEGIN_NAMESPACE

extern Q_GUI_EXPORT QImage qt_gl_read_framebuffer(const QSize &size, bool alpha_format, bool include_alpha);

bool QQuickWindowPrivate::defaultAlphaBuffer(0);

void QQuickWindowPrivate::updateFocusItemTransform()
{
    Q_Q(QQuickWindow);
#ifndef QT_NO_IM
    QQuickItem *focus = q->activeFocusItem();
    if (focus && qApp->focusObject() == focus) {
        QQuickItemPrivate *focusPrivate = QQuickItemPrivate::get(focus);
        qApp->inputMethod()->setInputItemTransform(focusPrivate->itemToWindowTransform());
        qApp->inputMethod()->setInputItemRectangle(QRectF(0, 0, focusPrivate->width, focusPrivate->height));
    }
#endif
}

class QQuickWindowIncubationController : public QObject, public QQmlIncubationController
{
    Q_OBJECT

public:
    QQuickWindowIncubationController(QSGRenderLoop *loop)
        : m_renderLoop(loop), m_timer(0)
    {
        // Allow incubation for 1/3 of a frame.
        m_incubation_time = qMax(1, int(1000 / QGuiApplication::primaryScreen()->refreshRate()) / 3);

        m_animation_driver = m_renderLoop->animationDriver();
        if (m_animation_driver) {
            connect(m_animation_driver, SIGNAL(stopped()), this, SLOT(animationStopped()));
            connect(m_renderLoop, SIGNAL(timeToIncubate()), this, SLOT(incubate()));
        }
    }

protected:
    void timerEvent(QTimerEvent *)
    {
        killTimer(m_timer);
        m_timer = 0;
        incubate();
    }

    void incubateAgain() {
        if (m_timer == 0) {
            // Wait for a while before processing the next batch. Using a
            // timer to avoid starvation of system events.
            m_timer = startTimer(m_incubation_time);
        }
    }

public slots:
    void incubate() {
        if (incubatingObjectCount()) {
            if (m_renderLoop->interleaveIncubation()) {
                incubateFor(m_incubation_time);
            } else {
                incubateFor(m_incubation_time * 2);
                if (incubatingObjectCount())
                    incubateAgain();
            }
        }
    }

    void animationStopped() { incubate(); }

protected:
    virtual void incubatingObjectCountChanged(int count)
    {
        if (count && !m_renderLoop->interleaveIncubation())
            incubateAgain();
    }

private:
    QSGRenderLoop *m_renderLoop;
    int m_incubation_time;
    QAnimationDriver *m_animation_driver;
    int m_timer;
};

#include "qquickwindow.moc"


#ifndef QT_NO_ACCESSIBILITY
/*!
    Returns an accessibility interface for this window, or 0 if such an
    interface cannot be created.
*/
QAccessibleInterface *QQuickWindow::accessibleRoot() const
{
    return QAccessible::queryAccessibleInterface(const_cast<QQuickWindow*>(this));
}
#endif


/*
Focus behavior
==============

Prior to being added to a valid window items can set and clear focus with no
effect.  Only once items are added to a window (by way of having a parent set that
already belongs to a window) do the focus rules apply.  Focus goes back to
having no effect if an item is removed from a window.

When an item is moved into a new focus scope (either being added to a window
for the first time, or having its parent changed), if the focus scope already has
a scope focused item that takes precedence over the item being added.  Otherwise,
the focus of the added tree is used.  In the case of of a tree of items being
added to a window for the first time, which may have a conflicted focus state (two
or more items in one scope having focus set), the same rule is applied item by item -
thus the first item that has focus will get it (assuming the scope doesn't already
have a scope focused item), and the other items will have their focus cleared.
*/


// #define FOCUS_DEBUG
// #define MOUSE_DEBUG
// #define TOUCH_DEBUG
// #define DIRTY_DEBUG

#ifdef FOCUS_DEBUG
void printFocusTree(QQuickItem *item, QQuickItem *scope = 0, int depth = 1);
#endif

QQuickItem::UpdatePaintNodeData::UpdatePaintNodeData()
: transformNode(0)
{
}

QQuickRootItem::QQuickRootItem()
{
}

/*! \reimp */
void QQuickWindow::exposeEvent(QExposeEvent *)
{
    Q_D(QQuickWindow);
    d->windowManager->exposureChanged(this);
}

/*! \reimp */
void QQuickWindow::resizeEvent(QResizeEvent *)
{
}

/*! \reimp */
void QQuickWindow::showEvent(QShowEvent *)
{
    d_func()->windowManager->show(this);
}

/*! \reimp */
void QQuickWindow::hideEvent(QHideEvent *)
{
    d_func()->windowManager->hide(this);
}

/*! \reimp */
void QQuickWindow::focusOutEvent(QFocusEvent *ev)
{
    Q_D(QQuickWindow);
    d->contentItem->setFocus(false, ev->reason());
}

/*! \reimp */
void QQuickWindow::focusInEvent(QFocusEvent *ev)
{
    Q_D(QQuickWindow);
    d->contentItem->setFocus(true, ev->reason());
    d->updateFocusItemTransform();
}


void QQuickWindowPrivate::polishItems()
{
    int maxPolishCycles = 100000;

    while (!itemsToPolish.isEmpty() && --maxPolishCycles > 0) {
        QSet<QQuickItem *> itms = itemsToPolish;
        itemsToPolish.clear();

        for (QSet<QQuickItem *>::iterator it = itms.begin(); it != itms.end(); ++it) {
            QQuickItem *item = *it;
            QQuickItemPrivate::get(item)->polishScheduled = false;
            item->updatePolish();
        }
    }

    if (maxPolishCycles == 0)
        qWarning("QQuickWindow: possible QQuickItem::polish() loop");

    updateFocusItemTransform();
}

/*!
 * Schedules the window to render another frame.
 *
 * Calling QQuickWindow::update() differs from QQuickItem::update() in that
 * it always triggers a repaint, regardless of changes in the underlying
 * scene graph or not.
 */
void QQuickWindow::update()
{
    Q_D(QQuickWindow);
    d->windowManager->update(this);
}

void forcePolishHelper(QQuickItem *item)
{
    if (item->flags() & QQuickItem::ItemHasContents) {
        item->polish();
    }

    QList <QQuickItem *> items = item->childItems();
    for (int i=0; i<items.size(); ++i)
        forcePolishHelper(items.at(i));
}

/*!
    Schedules polish events on all items in the scene.
*/
void QQuickWindow::forcePolish()
{
    Q_D(QQuickWindow);
    forcePolishHelper(d->contentItem);
}

void forceUpdate(QQuickItem *item)
{
    if (item->flags() & QQuickItem::ItemHasContents)
        item->update();
    QQuickItemPrivate::get(item)->dirty(QQuickItemPrivate::ChildrenUpdateMask);

    QList <QQuickItem *> items = item->childItems();
    for (int i=0; i<items.size(); ++i)
        forceUpdate(items.at(i));
}

void QQuickWindowPrivate::syncSceneGraph()
{
    QML_MEMORY_SCOPE_STRING("SceneGraph");
    Q_Q(QQuickWindow);

    emit q->beforeSynchronizing();
    if (!renderer) {
        forceUpdate(contentItem);

        QSGRootNode *rootNode = new QSGRootNode;
        rootNode->appendChildNode(QQuickItemPrivate::get(contentItem)->itemNode());
        renderer = context->createRenderer();
        renderer->setRootNode(rootNode);
    }

    updateDirtyNodes();

    // Copy the current state of clearing from window into renderer.
    renderer->setClearColor(clearColor);
    QSGRenderer::ClearMode mode = QSGRenderer::ClearStencilBuffer | QSGRenderer::ClearDepthBuffer;
    if (clearBeforeRendering)
        mode |= QSGRenderer::ClearColorBuffer;
    renderer->setClearMode(mode);
}


void QQuickWindowPrivate::renderSceneGraph(const QSize &size)
{
    QML_MEMORY_SCOPE_STRING("SceneGraph");
    Q_Q(QQuickWindow);
    emit q->beforeRendering();
    int fboId = 0;
    const qreal devicePixelRatio = q->devicePixelRatio();
    renderer->setDeviceRect(QRect(QPoint(0, 0), size * devicePixelRatio));
    if (renderTargetId) {
        fboId = renderTargetId;
        renderer->setViewportRect(QRect(QPoint(0, 0), renderTargetSize));
    } else {
        renderer->setViewportRect(QRect(QPoint(0, 0), size * devicePixelRatio));
    }
    renderer->setProjectionMatrixToRect(QRect(QPoint(0, 0), size));
    renderer->setDevicePixelRatio(q->devicePixelRatio());

    context->renderNextFrame(renderer, fboId);
    emit q->afterRendering();
}

QQuickWindowPrivate::QQuickWindowPrivate()
    : contentItem(0)
    , activeFocusItem(0)
    , mouseGrabberItem(0)
#ifndef QT_NO_CURSOR
    , cursorItem(0)
#endif
    , touchMouseId(-1)
    , touchMousePressTimestamp(0)
    , dirtyItemList(0)
    , context(0)
    , renderer(0)
    , windowManager(0)
    , clearColor(Qt::white)
    , clearBeforeRendering(true)
    , persistentGLContext(true)
    , persistentSceneGraph(true)
    , lastWheelEventAccepted(false)
    , componentCompleted(true)
    , renderTarget(0)
    , renderTargetId(0)
    , incubationController(0)
{
}

QQuickWindowPrivate::~QQuickWindowPrivate()
{
}

void QQuickWindowPrivate::init(QQuickWindow *c)
{
    q_ptr = c;

    Q_Q(QQuickWindow);

    contentItem = new QQuickRootItem;
    QQmlEngine::setObjectOwnership(contentItem, QQmlEngine::CppOwnership);
    QQuickItemPrivate *contentItemPrivate = QQuickItemPrivate::get(contentItem);
    contentItemPrivate->window = q;
    contentItemPrivate->windowRefCount = 1;
    contentItemPrivate->flags |= QQuickItem::ItemIsFocusScope;

    windowManager = QSGRenderLoop::instance();
    context = windowManager->sceneGraphContext();
    q->setSurfaceType(QWindow::OpenGLSurface);
    q->setFormat(context->defaultSurfaceFormat());

    QObject::connect(context, SIGNAL(initialized()), q, SIGNAL(sceneGraphInitialized()), Qt::DirectConnection);
    QObject::connect(context, SIGNAL(invalidated()), q, SIGNAL(sceneGraphInvalidated()), Qt::DirectConnection);
    QObject::connect(context, SIGNAL(invalidated()), q, SLOT(cleanupSceneGraph()), Qt::DirectConnection);

    QObject::connect(q, SIGNAL(focusObjectChanged(QObject*)), q, SIGNAL(activeFocusItemChanged()));
    QObject::connect(q, SIGNAL(screenChanged(QScreen*)), q, SLOT(forcePolish()));
}

/*!
    \property QQuickWindow::data
    \internal
*/

QQmlListProperty<QObject> QQuickWindowPrivate::data()
{
    initContentItem();
    return QQmlListProperty<QObject>(q_func(), 0, QQuickWindowPrivate::data_append,
                                             QQuickWindowPrivate::data_count,
                                             QQuickWindowPrivate::data_at,
                                             QQuickWindowPrivate::data_clear);
}

void QQuickWindowPrivate::initContentItem()
{
    Q_Q(QQuickWindow);
    q->connect(q, SIGNAL(widthChanged(int)),
            contentItem, SLOT(setWidth(int)));
    q->connect(q, SIGNAL(heightChanged(int)),
            contentItem, SLOT(setHeight(int)));
    contentItem->setWidth(q->width());
    contentItem->setHeight(q->height());
}

static QMouseEvent *touchToMouseEvent(QEvent::Type type, const QTouchEvent::TouchPoint &p, QTouchEvent *event, QQuickItem *item, bool transformNeeded = true)
{
    // The touch point local position and velocity are not yet transformed.
    QMouseEvent *me = new QMouseEvent(type, transformNeeded ? item->mapFromScene(p.scenePos()) : p.pos(), p.scenePos(), p.screenPos(),
                                      Qt::LeftButton, (type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton), event->modifiers());
    me->setAccepted(true);
    me->setTimestamp(event->timestamp());
    QVector2D transformedVelocity = p.velocity();
    if (transformNeeded) {
        QQuickItemPrivate *itemPrivate = QQuickItemPrivate::get(item);
        QMatrix4x4 transformMatrix(itemPrivate->windowToItemTransform());
        transformedVelocity = transformMatrix.mapVector(p.velocity()).toVector2D();
    }
    QGuiApplicationPrivate::setMouseEventCapsAndVelocity(me, event->device()->capabilities(), transformedVelocity);
    return me;
}

bool QQuickWindowPrivate::checkIfDoubleClicked(ulong newPressEventTimestamp)
{
    bool doubleClicked;

    if (touchMousePressTimestamp == 0) {
        // just initialize the variable
        touchMousePressTimestamp = newPressEventTimestamp;
        doubleClicked = false;
    } else {
        ulong timeBetweenPresses = newPressEventTimestamp - touchMousePressTimestamp;
        ulong doubleClickInterval = static_cast<ulong>(qApp->styleHints()->
                mouseDoubleClickInterval());
        doubleClicked = timeBetweenPresses < doubleClickInterval;
        if (doubleClicked) {
            touchMousePressTimestamp = 0;
        } else {
            touchMousePressTimestamp = newPressEventTimestamp;
        }
    }

    return doubleClicked;
}

bool QQuickWindowPrivate::translateTouchToMouse(QQuickItem *item, QTouchEvent *event)
{
    // For each point, check if it is accepted, if not, try the next point.
    // Any of the fingers can become the mouse one.
    // This can happen because a mouse area might not accept an event at some point but another.
    for (int i = 0; i < event->touchPoints().count(); ++i) {
        const QTouchEvent::TouchPoint &p = event->touchPoints().at(i);
        // A new touch point
        if (touchMouseId == -1 && p.state() & Qt::TouchPointPressed) {
            QPointF pos = item->mapFromScene(p.scenePos());

            // probably redundant, we check bounds in the calling function (matchingNewPoints)
            if (!item->contains(pos))
                break;

            // Store the id already here and restore it to -1 if the event does not get
            // accepted. Cannot defer setting the new value because otherwise if the event
            // handler spins the event loop all subsequent moves and releases get lost.
            touchMouseId = p.id();
            itemForTouchPointId[touchMouseId] = item;
            QScopedPointer<QMouseEvent> mousePress(touchToMouseEvent(QEvent::MouseButtonPress, p, event, item));

            // Send a single press and see if that's accepted
            if (!mouseGrabberItem)
                item->grabMouse();
            item->grabTouchPoints(QVector<int>() << touchMouseId);

            QQuickItemPrivate::get(item)->deliverMouseEvent(mousePress.data());
            event->setAccepted(mousePress->isAccepted());
            if (!mousePress->isAccepted()) {
                touchMouseId = -1;
                if (itemForTouchPointId.value(p.id()) == item)
                    itemForTouchPointId.remove(p.id());

                if (mouseGrabberItem == item)
                    item->ungrabMouse();
            }

            if (mousePress->isAccepted() && checkIfDoubleClicked(event->timestamp())) {
                QScopedPointer<QMouseEvent> mouseDoubleClick(touchToMouseEvent(QEvent::MouseButtonDblClick, p, event, item));
                QQuickItemPrivate::get(item)->deliverMouseEvent(mouseDoubleClick.data());
                event->setAccepted(mouseDoubleClick->isAccepted());
                if (mouseDoubleClick->isAccepted()) {
                    return true;
                } else {
                    touchMouseId = -1;
                }
            }
            // The event was accepted, we are done.
            if (mousePress->isAccepted())
                return true;
            // The event was not accepted but touchMouseId was set.
            if (touchMouseId != -1)
                return false;
            // try the next point

        // Touch point was there before and moved
        } else if (p.id() == touchMouseId) {
            if (p.state() & Qt::TouchPointMoved) {
                if (mouseGrabberItem) {
                    QScopedPointer<QMouseEvent> me(touchToMouseEvent(QEvent::MouseMove, p, event, mouseGrabberItem));
                    QQuickItemPrivate::get(item)->deliverMouseEvent(me.data());
                    event->setAccepted(me->isAccepted());
                    if (me->isAccepted()) {
                        itemForTouchPointId[p.id()] = mouseGrabberItem; // N.B. the mouseGrabberItem may be different after returning from sendEvent()
                        return true;
                    }
                } else {
                    // no grabber, check if we care about mouse hover
                    // FIXME: this should only happen once, not recursively... I'll ignore it just ignore hover now.
                    // hover for touch???
                    QScopedPointer<QMouseEvent> me(touchToMouseEvent(QEvent::MouseMove, p, event, item));
                    if (lastMousePosition.isNull())
                        lastMousePosition = me->windowPos();
                    QPointF last = lastMousePosition;
                    lastMousePosition = me->windowPos();

                    bool accepted = me->isAccepted();
                    bool delivered = deliverHoverEvent(contentItem, me->windowPos(), last, me->modifiers(), accepted);
                    if (!delivered) {
                        //take care of any exits
                        accepted = clearHover();
                    }
                    me->setAccepted(accepted);
                    break;
                }
            } else if (p.state() & Qt::TouchPointReleased) {
                // currently handled point was released
                touchMouseId = -1;
                if (mouseGrabberItem) {
                    QScopedPointer<QMouseEvent> me(touchToMouseEvent(QEvent::MouseButtonRelease, p, event, mouseGrabberItem));
                    QQuickItemPrivate::get(item)->deliverMouseEvent(me.data());
                    if (mouseGrabberItem) // might have ungrabbed due to event
                        mouseGrabberItem->ungrabMouse();
                    return me->isAccepted();
                }
            }
            break;
        }
    }
    return false;
}

void QQuickWindowPrivate::transformTouchPoints(QList<QTouchEvent::TouchPoint> &touchPoints, const QTransform &transform)
{
    QMatrix4x4 transformMatrix(transform);
    for (int i=0; i<touchPoints.count(); i++) {
        QTouchEvent::TouchPoint &touchPoint = touchPoints[i];
        touchPoint.setRect(transform.mapRect(touchPoint.sceneRect()));
        touchPoint.setStartPos(transform.map(touchPoint.startScenePos()));
        touchPoint.setLastPos(transform.map(touchPoint.lastScenePos()));
        touchPoint.setVelocity(transformMatrix.mapVector(touchPoint.velocity()).toVector2D());
    }
}


/*!
Translates the data in \a touchEvent to this window.  This method leaves the item local positions in
\a touchEvent untouched (these are filled in later).
*/
void QQuickWindowPrivate::translateTouchEvent(QTouchEvent *touchEvent)
{
    QList<QTouchEvent::TouchPoint> touchPoints = touchEvent->touchPoints();
    for (int i = 0; i < touchPoints.count(); ++i) {
        QTouchEvent::TouchPoint &touchPoint = touchPoints[i];

        touchPoint.setScreenRect(touchPoint.sceneRect());
        touchPoint.setStartScreenPos(touchPoint.startScenePos());
        touchPoint.setLastScreenPos(touchPoint.lastScenePos());

        touchPoint.setSceneRect(touchPoint.rect());
        touchPoint.setStartScenePos(touchPoint.startPos());
        touchPoint.setLastScenePos(touchPoint.lastPos());

        if (i == 0)
            lastMousePosition = touchPoint.pos().toPoint();
    }
    touchEvent->setTouchPoints(touchPoints);
}

/*!
Set the focus inside \a scope to be \a item.
If the scope contains the active focus item, it will be changed to \a item.
Calls notifyFocusChangesRecur for all changed items.
*/
void QQuickWindowPrivate::setFocusInScope(QQuickItem *scope, QQuickItem *item, Qt::FocusReason reason, FocusOptions options)
{
    Q_Q(QQuickWindow);

    Q_ASSERT(item);
    Q_ASSERT(scope || item == contentItem);

#ifdef FOCUS_DEBUG
    qWarning() << "QQuickWindowPrivate::setFocusInScope():";
    qWarning() << "    scope:" << (QObject *)scope;
    if (scope)
        qWarning() << "    scopeSubFocusItem:" << (QObject *)QQuickItemPrivate::get(scope)->subFocusItem;
    qWarning() << "    item:" << (QObject *)item;
    qWarning() << "    activeFocusItem:" << (QObject *)activeFocusItem;
#endif

    QQuickItemPrivate *scopePrivate = scope ? QQuickItemPrivate::get(scope) : 0;
    QQuickItemPrivate *itemPrivate = QQuickItemPrivate::get(item);

    QQuickItem *newActiveFocusItem = 0;

    QVarLengthArray<QQuickItem *, 20> changed;

    // Does this change the active focus?
    if (item == contentItem || (scopePrivate->activeFocus && item->isEnabled())) {
        QQuickItem *oldActiveFocusItem = 0;
        oldActiveFocusItem = activeFocusItem;
        newActiveFocusItem = item;
        while (newActiveFocusItem->isFocusScope()
               && newActiveFocusItem->scopedFocusItem()
               && newActiveFocusItem->scopedFocusItem()->isEnabled()) {
            newActiveFocusItem = newActiveFocusItem->scopedFocusItem();
        }

        if (oldActiveFocusItem) {
#ifndef QT_NO_IM
            qApp->inputMethod()->commit();
#endif

            activeFocusItem = 0;
            QFocusEvent event(QEvent::FocusOut, reason);
            q->sendEvent(oldActiveFocusItem, &event);

            QQuickItem *afi = oldActiveFocusItem;
            while (afi && afi != scope) {
                if (QQuickItemPrivate::get(afi)->activeFocus) {
                    QQuickItemPrivate::get(afi)->activeFocus = false;
                    changed << afi;
                }
                afi = afi->parentItem();
            }
        }
    }

    if (item != contentItem && !(options & DontChangeSubFocusItem)) {
        QQuickItem *oldSubFocusItem = scopePrivate->subFocusItem;
        if (oldSubFocusItem) {
            QQuickItemPrivate::get(oldSubFocusItem)->focus = false;
            changed << oldSubFocusItem;
        }

        QQuickItemPrivate::get(item)->updateSubFocusItem(scope, true);
    }

    if (!(options & DontChangeFocusProperty)) {
        if (item != contentItem || QGuiApplication::focusWindow() == q) {
            itemPrivate->focus = true;
            changed << item;
        }
    }

    if (newActiveFocusItem && contentItem->hasFocus()) {
        activeFocusItem = newActiveFocusItem;

        QQuickItemPrivate::get(newActiveFocusItem)->activeFocus = true;
        changed << newActiveFocusItem;

        QQuickItem *afi = newActiveFocusItem->parentItem();
        while (afi && afi != scope) {
            if (afi->isFocusScope()) {
                QQuickItemPrivate::get(afi)->activeFocus = true;
                changed << afi;
            }
            afi = afi->parentItem();
        }
        updateFocusItemTransform();

        QFocusEvent event(QEvent::FocusIn, reason);
        q->sendEvent(newActiveFocusItem, &event);
    }

    emit q->focusObjectChanged(activeFocusItem);

    if (!changed.isEmpty())
        notifyFocusChangesRecur(changed.data(), changed.count() - 1);
}

void QQuickWindowPrivate::clearFocusInScope(QQuickItem *scope, QQuickItem *item, Qt::FocusReason reason, FocusOptions options)
{
    Q_Q(QQuickWindow);

    Q_ASSERT(item);
    Q_ASSERT(scope || item == contentItem);

#ifdef FOCUS_DEBUG
    qWarning() << "QQuickWindowPrivate::clearFocusInScope():";
    qWarning() << "    scope:" << (QObject *)scope;
    qWarning() << "    item:" << (QObject *)item;
    qWarning() << "    activeFocusItem:" << (QObject *)activeFocusItem;
#endif

    QQuickItemPrivate *scopePrivate = 0;
    if (scope) {
        scopePrivate = QQuickItemPrivate::get(scope);
        if ( !scopePrivate->subFocusItem )
            return;//No focus, nothing to do.
    }

    QQuickItem *oldActiveFocusItem = 0;
    QQuickItem *newActiveFocusItem = 0;

    QVarLengthArray<QQuickItem *, 20> changed;

    Q_ASSERT(item == contentItem || item == scopePrivate->subFocusItem);

    // Does this change the active focus?
    if (item == contentItem || scopePrivate->activeFocus) {
        oldActiveFocusItem = activeFocusItem;
        newActiveFocusItem = scope;

        Q_ASSERT(oldActiveFocusItem);

#ifndef QT_NO_IM
        qApp->inputMethod()->commit();
#endif

        activeFocusItem = 0;
        QFocusEvent event(QEvent::FocusOut, reason);
        q->sendEvent(oldActiveFocusItem, &event);

        QQuickItem *afi = oldActiveFocusItem;
        while (afi && afi != scope) {
            if (QQuickItemPrivate::get(afi)->activeFocus) {
                QQuickItemPrivate::get(afi)->activeFocus = false;
                changed << afi;
            }
            afi = afi->parentItem();
        }
    }

    if (item != contentItem && !(options & DontChangeSubFocusItem)) {
        QQuickItem *oldSubFocusItem = scopePrivate->subFocusItem;
        if (oldSubFocusItem && !(options & DontChangeFocusProperty)) {
            QQuickItemPrivate::get(oldSubFocusItem)->focus = false;
            changed << oldSubFocusItem;
        }

        QQuickItemPrivate::get(item)->updateSubFocusItem(scope, false);

    } else if (!(options & DontChangeFocusProperty)) {
        QQuickItemPrivate::get(item)->focus = false;
        changed << item;
    }

    if (newActiveFocusItem) {
        Q_ASSERT(newActiveFocusItem == scope);
        activeFocusItem = scope;
        updateFocusItemTransform();

        QFocusEvent event(QEvent::FocusIn, reason);
        q->sendEvent(newActiveFocusItem, &event);
    }

    emit q->focusObjectChanged(activeFocusItem);

    if (!changed.isEmpty())
        notifyFocusChangesRecur(changed.data(), changed.count() - 1);
}

void QQuickWindowPrivate::notifyFocusChangesRecur(QQuickItem **items, int remaining)
{
    QPointer<QQuickItem> item(*items);

    if (remaining)
        notifyFocusChangesRecur(items + 1, remaining - 1);

    if (item) {
        QQuickItemPrivate *itemPrivate = QQuickItemPrivate::get(item);

        if (itemPrivate->notifiedFocus != itemPrivate->focus) {
            itemPrivate->notifiedFocus = itemPrivate->focus;
            emit item->focusChanged(itemPrivate->focus);
        }

        if (item && itemPrivate->notifiedActiveFocus != itemPrivate->activeFocus) {
            itemPrivate->notifiedActiveFocus = itemPrivate->activeFocus;
            itemPrivate->itemChange(QQuickItem::ItemActiveFocusHasChanged, itemPrivate->activeFocus);
            emit item->activeFocusChanged(itemPrivate->activeFocus);
        }
    }
}

void QQuickWindowPrivate::dirtyItem(QQuickItem *)
{
    Q_Q(QQuickWindow);
    q->maybeUpdate();
}

void QQuickWindowPrivate::cleanup(QSGNode *n)
{
    Q_Q(QQuickWindow);

    Q_ASSERT(!cleanupNodeList.contains(n));
    cleanupNodeList.append(n);
    q->maybeUpdate();
}

/*!
    \qmltype Window
    \instantiates QQuickWindow
    \inqmlmodule QtQuick.Window 2
    \ingroup qtquick-visual
    \brief Creates a new top-level window

    The Window object creates a new top-level window for a Qt Quick scene. It automatically sets up the
    window for use with \c {QtQuick 2.x} graphical types.

    To use this type, you will need to import the module with the following line:
    \code
    import QtQuick.Window 2.1
    \endcode

    Omitting this import will allow you to have a QML environment without
    access to window system features.

    A Window can be declared inside an Item or inside another Window; in that
    case the inner Window will automatically become "transient for" the outer
    Window: that is, most platforms will show it centered upon the outer window
    by default, and there may be other platform-dependent behaviors, depending
    also on the \l flags. If the nested window is intended to be a dialog in
    your application, you should also set \l flags to Qt.Dialog, because some
    window managers will not provide the centering behavior without that flag.
    You can also declare multiple windows inside a top-level \l QtObject, in which
    case the windows will have no transient relationship.

    Alternatively you can set or bind \l x and \l y to position the Window
    explicitly on the screen.

    When the user attempts to close a window, the \a closing signal will be
    emitted. You can force the window to stay open (for example to prompt the
    user to save changes) by writing an onClosing handler and setting
    close.accepted = false.
*/
/*!
    \class QQuickWindow
    \since QtQuick 2.0

    \inmodule QtQuick

    \brief The QQuickWindow class provides the window for displaying a graphical QML scene

    QQuickWindow provides the graphical scene management needed to interact with and display
    a scene of QQuickItems.

    A QQuickWindow always has a single invisible root item. To add items to this window,
    reparent the items to the root item or to an existing item in the scene.

    For easily displaying a scene from a QML file, see \l{QQuickView}.



    \section1 Rendering

    QQuickWindow uses a scene graph on top of OpenGL to
    render. This scene graph is disconnected from the QML scene and
    potentially lives in another thread, depending on the platform
    implementation. Since the rendering scene graph lives
    independently from the QML scene, it can also be completely
    released without affecting the state of the QML scene.

    The sceneGraphInitialized() signal is emitted on the rendering
    thread before the QML scene is rendered to the screen for the
    first time. If the rendering scene graph has been released, the
    signal will be emitted again before the next frame is rendered.


    \section2 Integration with OpenGL

    It is possible to integrate OpenGL calls directly into the
    QQuickWindow using the same OpenGL context as the Qt Quick Scene
    Graph. This is done by connecting to the
    QQuickWindow::beforeRendering() or QQuickWindow::afterRendering()
    signal.

    \note When using QQuickWindow::beforeRendering(), make sure to
    disable clearing before rendering with
    QQuickWindow::setClearBeforeRendering().


    \section2 Exposure and Visibility

    When a QQuickWindow instance is deliberately hidden with hide() or
    setVisible(false), it will stop rendering and its scene graph and
    OpenGL context might be released. The sceneGraphInvalidated()
    signal will be emitted when this happens.

    \warning It is crucial that OpenGL operations and interaction with
    the scene graph happens exclusively on the rendering thread,
    primarily during the updatePaintNode() phase.

    \warning As signals related to rendering might be emitted from the
    rendering thread, connections should be made using
    Qt::DirectConnection.


    \section2 Resource Management

    QML will try to cache images and scene graph nodes to
    improve performance, but in some low-memory scenarios it might be
    required to aggressively release these resources. The
    releaseResources() can be used to force the clean up of certain
    resources. Calling releaseResources() may result in the entire
    scene graph and its OpenGL context being deleted. The
    sceneGraphInvalidated() signal will be emitted when this happens.

    \sa {Scene Graph - OpenGL Under QML}

*/

/*!
    Constructs a window for displaying a QML scene with parent window \a parent.
*/
QQuickWindow::QQuickWindow(QWindow *parent)
    : QWindow(*(new QQuickWindowPrivate), parent)
{
    Q_D(QQuickWindow);
    d->init(this);
}



/*!
    \internal
*/
QQuickWindow::QQuickWindow(QQuickWindowPrivate &dd, QWindow *parent)
    : QWindow(dd, parent)
{
    Q_D(QQuickWindow);
    d->init(this);
}

/*!
    Destroys the window.
*/
QQuickWindow::~QQuickWindow()
{
    Q_D(QQuickWindow);

    d->windowManager->windowDestroyed(this);

    QCoreApplication::sendPostedEvents(0, QEvent::DeferredDelete);
    delete d->incubationController; d->incubationController = 0;

    delete d->contentItem; d->contentItem = 0;
}



/*!
    This function tries to release redundant resources currently held by the QML scene.

    Calling this function might result in the scene graph and the OpenGL context used
    for rendering being released to release graphics memory. If this happens, the
    sceneGraphInvalidated() signal will be called, allowing users to clean up their
    own graphics resources. The setPersistentOpenGLContext() and setPersistentSceneGraph()
    functions can be used to prevent this from happening, if handling the cleanup is
    not feasible in the application, at the cost of higher memory usage.

    \sa sceneGraphInvalidated(), setPersistentOpenGLContext(), setPersistentSceneGraph()
 */

void QQuickWindow::releaseResources()
{
    Q_D(QQuickWindow);
    d->windowManager->releaseResources(this);
    QQuickPixmap::purgeCache();
}



/*!
    Sets whether the OpenGL context can be released to \a
    persistent. The default value is true.

    The OpenGL context can be released to free up graphics resources
    when the window is obscured, hidden or not rendering. When this
    happens is implementation specific.

    The QOpenGLContext::aboutToBeDestroyed() signal is emitted from
    the QQuickWindow::openglContext() when the OpenGL context is about
    to be released.  The QQuickWindow::sceneGraphInitialized() signal
    is emitted when a new OpenGL context is created for this
    window. Make a Qt::DirectConnection to these signals to be
    notified.

    The OpenGL context is still released when the last QQuickWindow is
    deleted.

    \sa setPersistentSceneGraph(),
    QOpenGLContext::aboutToBeDestroyed(), sceneGraphInitialized()
 */

void QQuickWindow::setPersistentOpenGLContext(bool persistent)
{
    Q_D(QQuickWindow);
    d->persistentGLContext = persistent;
}



/*!
    Returns whether the OpenGL context can be released during the
    lifetime of the QQuickWindow.

    \note This is a hint. When and how this happens is implementation
    specific.
 */

bool QQuickWindow::isPersistentOpenGLContext() const
{
    Q_D(const QQuickWindow);
    return d->persistentGLContext;
}



/*!
    Sets whether the scene graph nodes and resources can be released
    to \a persistent.  The default value is true.

    The scene graph nodes and resources can be released to free up
    graphics resources when the window is obscured, hidden or not
    rendering. When this happens is implementation specific.

    The QQuickWindow::sceneGraphInvalidated() signal is emitted when
    cleanup occurs. The QQuickWindow::sceneGraphInitialized() signal
    is emitted when a new scene graph is recreated for this
    window. Make a Qt::DirectConnection to these signals to be
    notified.

    The scene graph nodes and resources are still released when the
    last QQuickWindow is deleted.

    \sa setPersistentOpenGLContext(),
    sceneGraphInvalidated(), sceneGraphInitialized()
 */

void QQuickWindow::setPersistentSceneGraph(bool persistent)
{
    Q_D(QQuickWindow);
    d->persistentSceneGraph = persistent;
}



/*!
    Returns whether the scene graph nodes and resources can be
    released during the lifetime of this QQuickWindow.

    \note This is a hint. When and how this happens is implementation
    specific.
 */

bool QQuickWindow::isPersistentSceneGraph() const
{
    Q_D(const QQuickWindow);
    return d->persistentSceneGraph;
}





/*!
    \property QQuickWindow::contentItem
    \brief The invisible root item of the scene.

  A QQuickWindow always has a single invisible root item containing all of its content.
  To add items to this window, reparent the items to the contentItem or to an existing
  item in the scene.
*/
QQuickItem *QQuickWindow::contentItem() const
{
    Q_D(const QQuickWindow);

    return d->contentItem;
}

/*!
    \property QQuickWindow::activeFocusItem

    \brief The item which currently has active focus or \c null if there is
    no item with active focus.
*/
QQuickItem *QQuickWindow::activeFocusItem() const
{
    Q_D(const QQuickWindow);

    return d->activeFocusItem;
}

/*!
  \internal
  \reimp
*/
QObject *QQuickWindow::focusObject() const
{
    Q_D(const QQuickWindow);

    if (d->activeFocusItem)
        return d->activeFocusItem;
    return const_cast<QQuickWindow*>(this);
}


/*!
  Returns the item which currently has the mouse grab.
*/
QQuickItem *QQuickWindow::mouseGrabberItem() const
{
    Q_D(const QQuickWindow);

    return d->mouseGrabberItem;
}


bool QQuickWindowPrivate::clearHover()
{
    Q_Q(QQuickWindow);
    if (hoverItems.isEmpty())
        return false;

    QPointF pos = q->mapFromGlobal(QGuiApplicationPrivate::lastCursorPosition.toPoint());

    bool accepted = false;
    foreach (QQuickItem* item, hoverItems)
        accepted = sendHoverEvent(QEvent::HoverLeave, item, pos, pos, QGuiApplication::keyboardModifiers(), true) || accepted;
    hoverItems.clear();
    return accepted;
}

/*! \reimp */
bool QQuickWindow::event(QEvent *e)
{
    Q_D(QQuickWindow);

    switch (e->type()) {

    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd: {
        QTouchEvent *touch = static_cast<QTouchEvent*>(e);
        d->translateTouchEvent(touch);
        // return in order to avoid the QWindow::event below
        return d->deliverTouchEvent(touch);
    }
        break;
    case QEvent::TouchCancel:
        // return in order to avoid the QWindow::event below
        return d->deliverTouchCancelEvent(static_cast<QTouchEvent*>(e));
        break;
    case QEvent::Leave:
        d->clearHover();
        d->lastMousePosition = QPoint();
        break;
#ifndef QT_NO_DRAGANDDROP
    case QEvent::DragEnter:
    case QEvent::DragLeave:
    case QEvent::DragMove:
    case QEvent::Drop:
        d->deliverDragEvent(&d->dragGrabber, e);
        break;
#endif
    case QEvent::WindowDeactivate:
        contentItem()->windowDeactivateEvent();
        break;
    case QEvent::Close: {
        // TOOD Qt 6 (binary incompatible)
        // closeEvent(static_cast<QCloseEvent *>(e));
        QQuickCloseEvent qev;
        qev.setAccepted(e->isAccepted());
        emit closing(&qev);
        e->setAccepted(qev.isAccepted());
        } break;
    case QEvent::FocusAboutToChange:
#ifndef QT_NO_IM
        if (d->activeFocusItem)
            qGuiApp->inputMethod()->commit();
#endif
        if (d->mouseGrabberItem)
            d->mouseGrabberItem->ungrabMouse();
        break;
    default:
        break;
    }

    return QWindow::event(e);
}

/*! \reimp */
void QQuickWindow::keyPressEvent(QKeyEvent *e)
{
    Q_D(QQuickWindow);

#ifndef QT_NO_SHORTCUT
    // Try looking for a Shortcut before sending key events
    if (QGuiApplicationPrivate::instance()->shortcutMap.tryShortcutEvent(focusObject(), e))
        return;
#endif

    if (d->activeFocusItem)
        sendEvent(d->activeFocusItem, e);
}

/*! \reimp */
void QQuickWindow::keyReleaseEvent(QKeyEvent *e)
{
    Q_D(QQuickWindow);

    if (d->activeFocusItem)
        sendEvent(d->activeFocusItem, e);
}

QMouseEvent *QQuickWindowPrivate::cloneMouseEvent(QMouseEvent *event, QPointF *transformedLocalPos)
{
    int caps = QGuiApplicationPrivate::mouseEventCaps(event);
    QVector2D velocity = QGuiApplicationPrivate::mouseEventVelocity(event);
    QMouseEvent *me = new QMouseEvent(event->type(),
                                      transformedLocalPos ? *transformedLocalPos : event->localPos(),
                                      event->windowPos(), event->screenPos(),
                                      event->button(), event->buttons(), event->modifiers());
    QGuiApplicationPrivate::setMouseEventCapsAndVelocity(me, caps, velocity);
    me->setTimestamp(event->timestamp());
    return me;
}

bool QQuickWindowPrivate::deliverInitialMousePressEvent(QQuickItem *item, QMouseEvent *event)
{
    Q_Q(QQuickWindow);

    QQuickItemPrivate *itemPrivate = QQuickItemPrivate::get(item);

    if (itemPrivate->flags & QQuickItem::ItemClipsChildrenToShape) {
        QPointF p = item->mapFromScene(event->windowPos());
        if (!item->contains(p))
            return false;
    }

    QList<QQuickItem *> children = itemPrivate->paintOrderChildItems();
    for (int ii = children.count() - 1; ii >= 0; --ii) {
        QQuickItem *child = children.at(ii);
        if (!child->isVisible() || !child->isEnabled() || QQuickItemPrivate::get(child)->culled)
            continue;
        if (deliverInitialMousePressEvent(child, event))
            return true;
    }

    if (itemPrivate->acceptedMouseButtons() & event->button()) {
        QPointF localPos = item->mapFromScene(event->windowPos());
        if (item->contains(localPos)) {
            QScopedPointer<QMouseEvent> me(cloneMouseEvent(event, &localPos));
            me->accept();
            item->grabMouse();
            q->sendEvent(item, me.data());
            event->setAccepted(me->isAccepted());
            if (me->isAccepted())
                return true;
            if (mouseGrabberItem)
                mouseGrabberItem->ungrabMouse();
        }
    }

    return false;
}

bool QQuickWindowPrivate::deliverMouseEvent(QMouseEvent *event)
{
    Q_Q(QQuickWindow);

    lastMousePosition = event->windowPos();

    if (!mouseGrabberItem &&
         event->type() == QEvent::MouseButtonPress &&
         (event->buttons() & event->button()) == event->buttons()) {
        if (deliverInitialMousePressEvent(contentItem, event))
            event->accept();
        else
            event->ignore();
        return event->isAccepted();
    }

    if (mouseGrabberItem) {
        QPointF localPos = mouseGrabberItem->mapFromScene(event->windowPos());
        QScopedPointer<QMouseEvent> me(cloneMouseEvent(event, &localPos));
        me->accept();
        q->sendEvent(mouseGrabberItem, me.data());
        event->setAccepted(me->isAccepted());
        if (me->isAccepted())
            return true;
    }

    return false;
}

/*! \reimp */
void QQuickWindow::mousePressEvent(QMouseEvent *event)
{
    Q_D(QQuickWindow);
#ifdef MOUSE_DEBUG
    qWarning() << "QQuickWindow::mousePressEvent()" << event->localPos() << event->button() << event->buttons();
#endif

    d->deliverMouseEvent(event);
}

/*! \reimp */
void QQuickWindow::mouseReleaseEvent(QMouseEvent *event)
{
    Q_D(QQuickWindow);
#ifdef MOUSE_DEBUG
    qWarning() << "QQuickWindow::mouseReleaseEvent()" << event->localPos() << event->button() << event->buttons();
#endif

    if (!d->mouseGrabberItem) {
        QWindow::mouseReleaseEvent(event);
        return;
    }

    d->deliverMouseEvent(event);
    if (d->mouseGrabberItem && !event->buttons())
        d->mouseGrabberItem->ungrabMouse();
}

/*! \reimp */
void QQuickWindow::mouseDoubleClickEvent(QMouseEvent *event)
{
    Q_D(QQuickWindow);
#ifdef MOUSE_DEBUG
    qWarning() << "QQuickWindow::mouseDoubleClickEvent()" << event->localPos() << event->button() << event->buttons();
#endif

    if (!d->mouseGrabberItem && (event->buttons() & event->button()) == event->buttons()) {
        if (d->deliverInitialMousePressEvent(d->contentItem, event))
            event->accept();
        else
            event->ignore();
        return;
    }

    d->deliverMouseEvent(event);
}

bool QQuickWindowPrivate::sendHoverEvent(QEvent::Type type, QQuickItem *item,
                                      const QPointF &scenePos, const QPointF &lastScenePos,
                                      Qt::KeyboardModifiers modifiers, bool accepted)
{
    Q_Q(QQuickWindow);
    const QTransform transform = QQuickItemPrivate::get(item)->windowToItemTransform();

    //create copy of event
    QHoverEvent hoverEvent(type, transform.map(scenePos), transform.map(lastScenePos), modifiers);
    hoverEvent.setAccepted(accepted);

    q->sendEvent(item, &hoverEvent);

    return hoverEvent.isAccepted();
}

/*! \reimp */
void QQuickWindow::mouseMoveEvent(QMouseEvent *event)
{
    Q_D(QQuickWindow);
#ifdef MOUSE_DEBUG
    qWarning() << "QQuickWindow::mouseMoveEvent()" << event->localPos() << event->button() << event->buttons();
#endif

#ifndef QT_NO_CURSOR
    d->updateCursor(event->windowPos());
#endif

    if (!d->mouseGrabberItem) {
        if (d->lastMousePosition.isNull())
            d->lastMousePosition = event->windowPos();
        QPointF last = d->lastMousePosition;
        d->lastMousePosition = event->windowPos();

        bool accepted = event->isAccepted();
        bool delivered = d->deliverHoverEvent(d->contentItem, event->windowPos(), last, event->modifiers(), accepted);
        if (!delivered) {
            //take care of any exits
            accepted = d->clearHover();
        }
        event->setAccepted(accepted);
        return;
    }

    d->deliverMouseEvent(event);
}

bool QQuickWindowPrivate::deliverHoverEvent(QQuickItem *item, const QPointF &scenePos, const QPointF &lastScenePos,
                                         Qt::KeyboardModifiers modifiers, bool &accepted)
{
    Q_Q(QQuickWindow);
    QQuickItemPrivate *itemPrivate = QQuickItemPrivate::get(item);

    if (itemPrivate->flags & QQuickItem::ItemClipsChildrenToShape) {
        QPointF p = item->mapFromScene(scenePos);
        if (!item->contains(p))
            return false;
    }

    QList<QQuickItem *> children = itemPrivate->paintOrderChildItems();
    for (int ii = children.count() - 1; ii >= 0; --ii) {
        QQuickItem *child = children.at(ii);
        if (!child->isVisible() || !child->isEnabled() || QQuickItemPrivate::get(child)->culled)
            continue;
        if (deliverHoverEvent(child, scenePos, lastScenePos, modifiers, accepted))
            return true;
    }

    if (itemPrivate->hoverEnabled) {
        QPointF p = item->mapFromScene(scenePos);
        if (item->contains(p)) {
            if (!hoverItems.isEmpty() && hoverItems[0] == item) {
                //move
                accepted = sendHoverEvent(QEvent::HoverMove, item, scenePos, lastScenePos, modifiers, accepted);
            } else {
                QList<QQuickItem *> itemsToHover;
                QQuickItem* parent = item;
                itemsToHover << item;
                while ((parent = parent->parentItem()))
                    itemsToHover << parent;

                // Leaving from previous hovered items until we reach the item or one of its ancestors.
                while (!hoverItems.isEmpty() && !itemsToHover.contains(hoverItems[0])) {
                    sendHoverEvent(QEvent::HoverLeave, hoverItems[0], scenePos, lastScenePos, modifiers, accepted);
                    hoverItems.removeFirst();
                }

                if (!hoverItems.isEmpty() && hoverItems[0] == item){//Not entering a new Item
                    // ### Shouldn't we send moves for the parent items as well?
                    accepted = sendHoverEvent(QEvent::HoverMove, item, scenePos, lastScenePos, modifiers, accepted);
                } else {
                    // Enter items that are not entered yet.
                    int startIdx = -1;
                    if (!hoverItems.isEmpty())
                        startIdx = itemsToHover.indexOf(hoverItems[0]) - 1;
                    if (startIdx == -1)
                        startIdx = itemsToHover.count() - 1;

                    for (int i = startIdx; i >= 0; i--) {
                        QQuickItem *itemToHover = itemsToHover[i];
                        QQuickItemPrivate *itemToHoverPrivate = QQuickItemPrivate::get(itemToHover);
                        // The item may be about to be deleted or reparented to another window
                        // due to another hover event delivered in this function. If that is the
                        // case, sending a hover event here will cause a crash or other bad
                        // behavior when the leave event is generated. Checking
                        // itemToHoverPrivate->window here prevents that case.
                        if (itemToHoverPrivate->window == q && itemToHoverPrivate->hoverEnabled) {
                            hoverItems.prepend(itemToHover);
                            sendHoverEvent(QEvent::HoverEnter, itemToHover, scenePos, lastScenePos, modifiers, accepted);
                        }
                    }
                }
            }
            return true;
        }
    }

    return false;
}

#ifndef QT_NO_WHEELEVENT
bool QQuickWindowPrivate::deliverWheelEvent(QQuickItem *item, QWheelEvent *event)
{
    Q_Q(QQuickWindow);
    QQuickItemPrivate *itemPrivate = QQuickItemPrivate::get(item);

    if (itemPrivate->flags & QQuickItem::ItemClipsChildrenToShape) {
        QPointF p = item->mapFromScene(event->posF());
        if (!item->contains(p))
            return false;
    }

    QList<QQuickItem *> children = itemPrivate->paintOrderChildItems();
    for (int ii = children.count() - 1; ii >= 0; --ii) {
        QQuickItem *child = children.at(ii);
        if (!child->isVisible() || !child->isEnabled() || QQuickItemPrivate::get(child)->culled)
            continue;
        if (deliverWheelEvent(child, event))
            return true;
    }

    QPointF p = item->mapFromScene(event->posF());

    if (item->contains(p)) {
        QWheelEvent wheel(p, p, event->pixelDelta(), event->angleDelta(), event->delta(),
                          event->orientation(), event->buttons(), event->modifiers(), event->phase());
        wheel.accept();
        q->sendEvent(item, &wheel);
        if (wheel.isAccepted()) {
            event->accept();
            return true;
        }
    }

    return false;
}

/*! \reimp */
void QQuickWindow::wheelEvent(QWheelEvent *event)
{
    Q_D(QQuickWindow);
#ifdef MOUSE_DEBUG
    qWarning() << "QQuickWindow::wheelEvent()" << event->pixelDelta() << event->angleDelta();
#endif

    //if the actual wheel event was accepted, accept the compatibility wheel event and return early
    if (d->lastWheelEventAccepted && event->angleDelta().isNull() && event->phase() == Qt::ScrollUpdate)
        return;

    event->ignore();
    d->deliverWheelEvent(d->contentItem, event);
    d->lastWheelEventAccepted = event->isAccepted();
}
#endif // QT_NO_WHEELEVENT


bool QQuickWindowPrivate::deliverTouchCancelEvent(QTouchEvent *event)
{
#ifdef TOUCH_DEBUG
    qWarning("touchCancelEvent");
#endif
    Q_Q(QQuickWindow);
    // A TouchCancel event will typically not contain any points.
    // Deliver it to all items that have active touches.
    QSet<QQuickItem *> cancelDelivered;
    foreach (QQuickItem *item, itemForTouchPointId) {
        if (cancelDelivered.contains(item))
            continue;
        cancelDelivered.insert(item);
        q->sendEvent(item, event);
    }
    touchMouseId = -1;
    if (mouseGrabberItem)
        mouseGrabberItem->ungrabMouse();
    // The next touch event can only be a TouchBegin so clean up.
    itemForTouchPointId.clear();
    return true;
}

// check what kind of touch we have (begin/update) and
// call deliverTouchPoints to actually dispatch the points
bool QQuickWindowPrivate::deliverTouchEvent(QTouchEvent *event)
{
#ifdef TOUCH_DEBUG
    if (event->type() == QEvent::TouchBegin)
        qWarning() << "touchBeginEvent";
    else if (event->type() == QEvent::TouchUpdate)
        qWarning() << "touchUpdateEvent points";
    else if (event->type() == QEvent::TouchEnd)
        qWarning("touchEndEvent");
#endif

    // List of all items that received an event before
    // When we have TouchBegin this is and will stay empty
    QHash<QQuickItem *, QList<QTouchEvent::TouchPoint> > updatedPoints;

    // Figure out who accepted a touch point last and put it in updatedPoints
    // Add additional item to newPoints
    const QList<QTouchEvent::TouchPoint> &touchPoints = event->touchPoints();
    QList<QTouchEvent::TouchPoint> newPoints;
    for (int i=0; i<touchPoints.count(); i++) {
        const QTouchEvent::TouchPoint &touchPoint = touchPoints.at(i);
        if (touchPoint.state() == Qt::TouchPointPressed) {
            newPoints << touchPoint;
        } else {
            // TouchPointStationary is relevant only to items which
            // are also receiving touch points with some other state.
            // But we have not yet decided which points go to which item,
            // so for now we must include all non-new points in updatedPoints.
            if (itemForTouchPointId.contains(touchPoint.id())) {
                QQuickItem *item = itemForTouchPointId.value(touchPoint.id());
                if (item)
                    updatedPoints[item].append(touchPoint);
            }
        }
    }

    // Deliver the event, but only if there is at least one new point
    // or some item accepted a point and should receive an update
    if (newPoints.count() > 0 || updatedPoints.count() > 0) {
        QSet<int> acceptedNewPoints;
        event->setAccepted(deliverTouchPoints(contentItem, event, newPoints, &acceptedNewPoints, &updatedPoints));
    } else
        event->ignore();

    // Remove released points from itemForTouchPointId
    if (event->touchPointStates() & Qt::TouchPointReleased) {
        for (int i=0; i<touchPoints.count(); i++) {
            if (touchPoints[i].state() == Qt::TouchPointReleased) {
                itemForTouchPointId.remove(touchPoints[i].id());
                if (touchPoints[i].id() == touchMouseId)
                    touchMouseId = -1;
            }
        }
    }

    if (event->type() == QEvent::TouchEnd) {
        Q_ASSERT(itemForTouchPointId.isEmpty());
    }

    return event->isAccepted();
}

// This function recurses and sends the events to the individual items
bool QQuickWindowPrivate::deliverTouchPoints(QQuickItem *item, QTouchEvent *event, const QList<QTouchEvent::TouchPoint> &newPoints, QSet<int> *acceptedNewPoints, QHash<QQuickItem *, QList<QTouchEvent::TouchPoint> > *updatedPoints)
{
    QQuickItemPrivate *itemPrivate = QQuickItemPrivate::get(item);

    if (itemPrivate->flags & QQuickItem::ItemClipsChildrenToShape) {
        for (int i=0; i<newPoints.count(); i++) {
            QPointF p = item->mapFromScene(newPoints[i].scenePos());
            if (!item->contains(p))
                return false;
        }
    }

    // Check if our children want the event (or parts of it)
    // This is the only point where touch event delivery recurses!
    QList<QQuickItem *> children = itemPrivate->paintOrderChildItems();
    for (int ii = children.count() - 1; ii >= 0; --ii) {
        QQuickItem *child = children.at(ii);
        if (!child->isEnabled() || !child->isVisible() || QQuickItemPrivate::get(child)->culled)
            continue;
        if (deliverTouchPoints(child, event, newPoints, acceptedNewPoints, updatedPoints))
            return true;
    }

    // None of the children accepted the event, so check the given item itself.
    // First, construct matchingPoints as a list of TouchPoints which the
    // given item might be interested in.  Any newly-pressed point which is
    // inside the item's bounds will be interesting, and also any updated point
    // which was already accepted by that item when it was first pressed.
    // (A point which was already accepted is effectively "grabbed" by the item.)

    // set of IDs of "interesting" new points
    QSet<int> matchingNewPoints;
    // set of points which this item has previously accepted, for starters
    QList<QTouchEvent::TouchPoint> matchingPoints = (*updatedPoints)[item];
    // now add the new points which are inside this item's bounds
    if (newPoints.count() > 0 && acceptedNewPoints->count() < newPoints.count()) {
        for (int i = 0; i < newPoints.count(); i++) {
            if (acceptedNewPoints->contains(newPoints[i].id()))
                continue;
            QPointF p = item->mapFromScene(newPoints[i].scenePos());
            if (item->contains(p)) {
                matchingNewPoints.insert(newPoints[i].id());
                matchingPoints << newPoints[i];
            }
        }
    }
    // If there are no matching new points, and the existing points are all stationary,
    // there's no need to send an event to this item.  This is required by a test in
    // tst_qquickwindow::touchEvent_basic:
    // a single stationary press on an item shouldn't cause an event
    if (matchingNewPoints.isEmpty()) {
        bool stationaryOnly = true;
        Q_FOREACH (QTouchEvent::TouchPoint tp, matchingPoints)
            if (tp.state() != Qt::TouchPointStationary)
                stationaryOnly = false;
        if (stationaryOnly)
            matchingPoints.clear();
    }

    if (!matchingPoints.isEmpty()) {
        // Now we know this item might be interested in the event. Copy and send it, but
        // with only the subset of TouchPoints which are relevant to that item: that's matchingPoints.
        QQuickItemPrivate *itemPrivate = QQuickItemPrivate::get(item);
        transformTouchPoints(matchingPoints, itemPrivate->windowToItemTransform());
        deliverMatchingPointsToItem(item, event, acceptedNewPoints, matchingNewPoints, matchingPoints);
    }

    // record the fact that this item has been visited already
    updatedPoints->remove(item);

    // recursion is done only if ALL touch points have been delivered
    return (acceptedNewPoints->count() == newPoints.count() && updatedPoints->isEmpty());
}

// touchEventForItemBounds has no means to generate a touch event that contains
// only the points that are relevant for this item.  Thus the need for
// matchingPoints to already be that set of interesting points.
// They are all pre-transformed, too.
bool QQuickWindowPrivate::deliverMatchingPointsToItem(QQuickItem *item, QTouchEvent *event, QSet<int> *acceptedNewPoints, const QSet<int> &matchingNewPoints, const QList<QTouchEvent::TouchPoint> &matchingPoints)
{
    QScopedPointer<QTouchEvent> touchEvent(touchEventWithPoints(*event, matchingPoints));
    touchEvent.data()->setTarget(item);
    bool touchEventAccepted = false;

    // First check whether the parent wants to be a filter,
    // and if the parent accepts the event we are done.
    if (sendFilteredTouchEvent(item->parentItem(), item, event)) {
        event->accept();
        return true;
    }

    // Since it can change in sendEvent, update itemForTouchPointId now
    foreach (int id, matchingNewPoints)
        itemForTouchPointId[id] = item;

    // Deliver the touch event to the given item
    QQuickItemPrivate *itemPrivate = QQuickItemPrivate::get(item);
    itemPrivate->deliverTouchEvent(touchEvent.data());
    touchEventAccepted = touchEvent->isAccepted();

    // If the touch event wasn't accepted, synthesize a mouse event and see if the item wants it.
    if (!touchEventAccepted && (itemPrivate->acceptedMouseButtons() & Qt::LeftButton)) {
        //  send mouse event
        event->setAccepted(translateTouchToMouse(item, event));
        if (event->isAccepted()) {
            touchEventAccepted = true;
        }
    }

    if (touchEventAccepted) {
        // If the touch was accepted (regardless by whom or in what form),
        // update acceptedNewPoints.
        foreach (int id, matchingNewPoints)
            acceptedNewPoints->insert(id);
    } else {
        // But if the event was not accepted then we know this item
        // will not be interested in further updates for those touchpoint IDs either.
        foreach (int id, matchingNewPoints)
            if (itemForTouchPointId[id] == item)
                itemForTouchPointId.remove(id);
    }

    return touchEventAccepted;
}

QTouchEvent *QQuickWindowPrivate::touchEventForItemBounds(QQuickItem *target, const QTouchEvent &originalEvent)
{
    const QList<QTouchEvent::TouchPoint> &touchPoints = originalEvent.touchPoints();
    QList<QTouchEvent::TouchPoint> pointsInBounds;
    // if all points are stationary, the list of points should be empty to signal a no-op
    if (originalEvent.touchPointStates() != Qt::TouchPointStationary) {
        for (int i = 0; i < touchPoints.count(); ++i) {
            const QTouchEvent::TouchPoint &tp = touchPoints.at(i);
            if (tp.state() == Qt::TouchPointPressed) {
                QPointF p = target->mapFromScene(tp.scenePos());
                if (target->contains(p))
                    pointsInBounds.append(tp);
            } else {
                pointsInBounds.append(tp);
            }
        }
        transformTouchPoints(pointsInBounds, QQuickItemPrivate::get(target)->windowToItemTransform());
    }

    QTouchEvent* touchEvent = touchEventWithPoints(originalEvent, pointsInBounds);
    touchEvent->setTarget(target);
    return touchEvent;
}

QTouchEvent *QQuickWindowPrivate::touchEventWithPoints(const QTouchEvent &event, const QList<QTouchEvent::TouchPoint> &newPoints)
{
    Qt::TouchPointStates eventStates;
    for (int i=0; i<newPoints.count(); i++)
        eventStates |= newPoints[i].state();
    // if all points have the same state, set the event type accordingly
    QEvent::Type eventType = event.type();
    switch (eventStates) {
        case Qt::TouchPointPressed:
            eventType = QEvent::TouchBegin;
            break;
        case Qt::TouchPointReleased:
            eventType = QEvent::TouchEnd;
            break;
        default:
            eventType = QEvent::TouchUpdate;
            break;
    }

    QTouchEvent *touchEvent = new QTouchEvent(eventType);
    touchEvent->setWindow(event.window());
    touchEvent->setTarget(event.target());
    touchEvent->setDevice(event.device());
    touchEvent->setModifiers(event.modifiers());
    touchEvent->setTouchPoints(newPoints);
    touchEvent->setTouchPointStates(eventStates);
    touchEvent->setTimestamp(event.timestamp());
    touchEvent->accept();
    return touchEvent;
}

#ifndef QT_NO_DRAGANDDROP
void QQuickWindowPrivate::deliverDragEvent(QQuickDragGrabber *grabber, QEvent *event)
{
    Q_Q(QQuickWindow);
    grabber->resetTarget();
    QQuickDragGrabber::iterator grabItem = grabber->begin();
    if (grabItem != grabber->end()) {
        Q_ASSERT(event->type() != QEvent::DragEnter);
        if (event->type() == QEvent::Drop) {
            QDropEvent *e = static_cast<QDropEvent *>(event);
            for (e->setAccepted(false); !e->isAccepted() && grabItem != grabber->end(); grabItem = grabber->release(grabItem)) {
                QPointF p = (**grabItem)->mapFromScene(e->pos());
                QDropEvent translatedEvent(
                        p.toPoint(),
                        e->possibleActions(),
                        e->mimeData(),
                        e->mouseButtons(),
                        e->keyboardModifiers());
                QQuickDropEventEx::copyActions(&translatedEvent, *e);
                q->sendEvent(**grabItem, &translatedEvent);
                e->setAccepted(translatedEvent.isAccepted());
                e->setDropAction(translatedEvent.dropAction());
                grabber->setTarget(**grabItem);
            }
        }
        if (event->type() != QEvent::DragMove) {    // Either an accepted drop or a leave.
            QDragLeaveEvent leaveEvent;
            for (; grabItem != grabber->end(); grabItem = grabber->release(grabItem))
                q->sendEvent(**grabItem, &leaveEvent);
            return;
        } else for (; grabItem != grabber->end(); grabItem = grabber->release(grabItem)) {
            QDragMoveEvent *moveEvent = static_cast<QDragMoveEvent *>(event);
            if (deliverDragEvent(grabber, **grabItem, moveEvent)) {
                moveEvent->setAccepted(true);
                for (++grabItem; grabItem != grabber->end();) {
                    QPointF p = (**grabItem)->mapFromScene(moveEvent->pos());
                    if ((**grabItem)->contains(p)) {
                        QDragMoveEvent translatedEvent(
                                p.toPoint(),
                                moveEvent->possibleActions(),
                                moveEvent->mimeData(),
                                moveEvent->mouseButtons(),
                                moveEvent->keyboardModifiers());
                        QQuickDropEventEx::copyActions(&translatedEvent, *moveEvent);
                        q->sendEvent(**grabItem, &translatedEvent);
                        ++grabItem;
                    } else {
                        QDragLeaveEvent leaveEvent;
                        q->sendEvent(**grabItem, &leaveEvent);
                        grabItem = grabber->release(grabItem);
                    }
                }
                return;
            } else {
                QDragLeaveEvent leaveEvent;
                q->sendEvent(**grabItem, &leaveEvent);
            }
        }
    }
    if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
        QDragMoveEvent *e = static_cast<QDragMoveEvent *>(event);
        QDragEnterEvent enterEvent(
                e->pos(),
                e->possibleActions(),
                e->mimeData(),
                e->mouseButtons(),
                e->keyboardModifiers());
        QQuickDropEventEx::copyActions(&enterEvent, *e);
        event->setAccepted(deliverDragEvent(grabber, contentItem, &enterEvent));
    }
}

bool QQuickWindowPrivate::deliverDragEvent(QQuickDragGrabber *grabber, QQuickItem *item, QDragMoveEvent *event)
{
    Q_Q(QQuickWindow);
    bool accepted = false;
    QQuickItemPrivate *itemPrivate = QQuickItemPrivate::get(item);
    if (!item->isVisible() || !item->isEnabled() || QQuickItemPrivate::get(item)->culled)
        return false;

    QPointF p = item->mapFromScene(event->pos());
    if (item->contains(p)) {
        if (event->type() == QEvent::DragMove || itemPrivate->flags & QQuickItem::ItemAcceptsDrops) {
            QDragMoveEvent translatedEvent(
                    p.toPoint(),
                    event->possibleActions(),
                    event->mimeData(),
                    event->mouseButtons(),
                    event->keyboardModifiers(),
                    event->type());
            QQuickDropEventEx::copyActions(&translatedEvent, *event);
            q->sendEvent(item, &translatedEvent);
            if (event->type() == QEvent::DragEnter) {
                if (translatedEvent.isAccepted()) {
                    grabber->grab(item);
                    accepted = true;
                }
            } else {
                accepted = true;
            }
        }
    } else if (itemPrivate->flags & QQuickItem::ItemClipsChildrenToShape) {
        return false;
    }

    QDragEnterEvent enterEvent(
            event->pos(),
            event->possibleActions(),
            event->mimeData(),
            event->mouseButtons(),
            event->keyboardModifiers());
    QQuickDropEventEx::copyActions(&enterEvent, *event);
    QList<QQuickItem *> children = itemPrivate->paintOrderChildItems();
    for (int ii = children.count() - 1; ii >= 0; --ii) {
        if (deliverDragEvent(grabber, children.at(ii), &enterEvent))
            return true;
    }

    return accepted;
}
#endif // QT_NO_DRAGANDDROP

#ifndef QT_NO_CURSOR
void QQuickWindowPrivate::updateCursor(const QPointF &scenePos)
{
    Q_Q(QQuickWindow);

    QQuickItem *oldCursorItem = cursorItem;
    cursorItem = findCursorItem(contentItem, scenePos);

    if (cursorItem != oldCursorItem) {
        if (cursorItem)
            q->setCursor(cursorItem->cursor());
        else
            q->unsetCursor();
    }
}

QQuickItem *QQuickWindowPrivate::findCursorItem(QQuickItem *item, const QPointF &scenePos)
{
    QQuickItemPrivate *itemPrivate = QQuickItemPrivate::get(item);
    if (itemPrivate->flags & QQuickItem::ItemClipsChildrenToShape) {
        QPointF p = item->mapFromScene(scenePos);
        if (!item->contains(p))
            return 0;
    }

    const int numCursorsInHierarchy = itemPrivate->extra.isAllocated() ? itemPrivate->extra.value().numItemsWithCursor : 0;
    const int numChildrenWithCursor = itemPrivate->hasCursor ? numCursorsInHierarchy-1 : numCursorsInHierarchy;

    if (numChildrenWithCursor > 0) {
        QList<QQuickItem *> children = itemPrivate->paintOrderChildItems();
        for (int ii = children.count() - 1; ii >= 0; --ii) {
            QQuickItem *child = children.at(ii);
            if (!child->isVisible() || !child->isEnabled() || QQuickItemPrivate::get(child)->culled)
                continue;
            if (QQuickItem *cursorItem = findCursorItem(child, scenePos))
                return cursorItem;
        }
    }

    if (itemPrivate->hasCursor) {
        QPointF p = item->mapFromScene(scenePos);
        if (item->contains(p))
            return item;
    }
    return 0;
}
#endif

bool QQuickWindowPrivate::sendFilteredTouchEvent(QQuickItem *target, QQuickItem *item, QTouchEvent *event)
{
    if (!target)
        return false;

    QQuickItemPrivate *targetPrivate = QQuickItemPrivate::get(target);
    if (targetPrivate->filtersChildMouseEvents) {
        QScopedPointer<QTouchEvent> targetEvent(touchEventForItemBounds(target, *event));
        if (!targetEvent->touchPoints().isEmpty()) {
            QVector<int> touchIds;
            for (int i = 0; i < event->touchPoints().size(); ++i)
                touchIds.append(event->touchPoints().at(i).id());
            if (target->childMouseEventFilter(item, targetEvent.data())) {
                target->grabTouchPoints(touchIds);
                if (mouseGrabberItem) {
                    mouseGrabberItem->ungrabMouse();
                    touchMouseId = -1;
                }
                return true;
            }

            // Only offer a mouse event to the filter if we have one point
            if (targetEvent->touchPoints().count() == 1) {
                QEvent::Type t;
                const QTouchEvent::TouchPoint &tp = targetEvent->touchPoints().first();
                switch (tp.state()) {
                case Qt::TouchPointPressed:
                    t = QEvent::MouseButtonPress;
                    break;
                case Qt::TouchPointReleased:
                    t = QEvent::MouseButtonRelease;
                    break;
                default:
                    // move or stationary
                    t = QEvent::MouseMove;
                    break;
                }

                // targetEvent is already transformed wrt local position, velocity, etc.
                QScopedPointer<QMouseEvent> mouseEvent(touchToMouseEvent(t, targetEvent->touchPoints().first(), event, item, false));
                if (target->childMouseEventFilter(item, mouseEvent.data())) {
                    itemForTouchPointId[tp.id()] = target;
                    touchMouseId = tp.id();
                    target->grabMouse();
                    return true;
                }
            }
        }
    }

    return sendFilteredTouchEvent(target->parentItem(), item, event);
}

bool QQuickWindowPrivate::sendFilteredMouseEvent(QQuickItem *target, QQuickItem *item, QEvent *event)
{
    if (!target)
        return false;

    QQuickItemPrivate *targetPrivate = QQuickItemPrivate::get(target);
    if (targetPrivate->filtersChildMouseEvents)
        if (target->childMouseEventFilter(item, event))
            return true;

    if (sendFilteredMouseEvent(target->parentItem(), item, event))
        return true;

    return false;
}

bool QQuickWindowPrivate::dragOverThreshold(qreal d, Qt::Axis axis, QMouseEvent *event)
{
    QStyleHints *styleHints = qApp->styleHints();
    int caps = QGuiApplicationPrivate::mouseEventCaps(event);
    bool dragVelocityLimitAvailable = (caps & QTouchDevice::Velocity)
        && styleHints->startDragVelocity();
    bool overThreshold = qAbs(d) > styleHints->startDragDistance();
    if (dragVelocityLimitAvailable) {
        QVector2D velocityVec = QGuiApplicationPrivate::mouseEventVelocity(event);
        qreal velocity = axis == Qt::XAxis ? velocityVec.x() : velocityVec.y();
        overThreshold |= qAbs(velocity) > styleHints->startDragVelocity();
    }
    return overThreshold;
}

/*!
    \qmlproperty list<Object> QtQuick.Window2::Window::data
    \default

    The data property allows you to freely mix visual children, resources
    and other Windows in a Window.

    If you assign another Window to the data list, the nested window will
    become "transient for" the outer Window.

    If you assign an \l Item to the data list, it becomes a child of the
    Window's \l contentItem, so that it appears inside the window. The item's
    parent will be the window's contentItem, which is the root of the Item
    ownership tree within that Window.

    If you assign any other object type, it is added as a resource.

    It should not generally be necessary to refer to the \c data property,
    as it is the default property for Window and thus all child items are
    automatically assigned to this property.

    \sa QWindow::transientParent()
 */

void QQuickWindowPrivate::data_append(QQmlListProperty<QObject> *property, QObject *o)
{
    if (!o)
        return;
    QQuickWindow *that = static_cast<QQuickWindow *>(property->object);
    if (QQuickWindow *window = qmlobject_cast<QQuickWindow *>(o))
        window->setTransientParent(that);
    QQmlListProperty<QObject> itemProperty = QQuickItemPrivate::get(that->contentItem())->data();
    itemProperty.append(&itemProperty, o);
}

int QQuickWindowPrivate::data_count(QQmlListProperty<QObject> *property)
{
    QQuickWindow *win = static_cast<QQuickWindow*>(property->object);
    if (!win || !win->contentItem() || !QQuickItemPrivate::get(win->contentItem())->data().count)
        return 0;
    QQmlListProperty<QObject> itemProperty = QQuickItemPrivate::get(win->contentItem())->data();
    return itemProperty.count(&itemProperty);
}

QObject *QQuickWindowPrivate::data_at(QQmlListProperty<QObject> *property, int i)
{
    QQuickWindow *win = static_cast<QQuickWindow*>(property->object);
    QQmlListProperty<QObject> itemProperty = QQuickItemPrivate::get(win->contentItem())->data();
    return itemProperty.at(&itemProperty, i);
}

void QQuickWindowPrivate::data_clear(QQmlListProperty<QObject> *property)
{
    QQuickWindow *win = static_cast<QQuickWindow*>(property->object);
    QQmlListProperty<QObject> itemProperty = QQuickItemPrivate::get(win->contentItem())->data();
    itemProperty.clear(&itemProperty);
}

bool QQuickWindowPrivate::isRenderable() const
{
    Q_Q(const QQuickWindow);
    return q->isExposed() && q->isVisible() && q->geometry().isValid();
}

/*!
    Propagates an event \a e to a QQuickItem \a item on the window.

    The return value is currently not used.
*/
bool QQuickWindow::sendEvent(QQuickItem *item, QEvent *e)
{
    Q_D(QQuickWindow);

    if (!item) {
        qWarning("QQuickWindow::sendEvent: Cannot send event to a null item");
        return false;
    }

    Q_ASSERT(e);

    switch (e->type()) {
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
        e->accept();
        QQuickItemPrivate::get(item)->deliverKeyEvent(static_cast<QKeyEvent *>(e));
        while (!e->isAccepted() && (item = item->parentItem())) {
            e->accept();
            QQuickItemPrivate::get(item)->deliverKeyEvent(static_cast<QKeyEvent *>(e));
        }
        break;
    case QEvent::FocusIn:
    case QEvent::FocusOut:
        QQuickItemPrivate::get(item)->deliverFocusEvent(static_cast<QFocusEvent *>(e));
        break;
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove:
        // XXX todo - should sendEvent be doing this?  how does it relate to forwarded events?
        if (!d->sendFilteredMouseEvent(item->parentItem(), item, e)) {
            // accept because qml items by default accept and have to explicitly opt out of accepting
            e->accept();
            QQuickItemPrivate::get(item)->deliverMouseEvent(static_cast<QMouseEvent *>(e));
        }
        break;
    case QEvent::UngrabMouse:
        if (!d->sendFilteredMouseEvent(item->parentItem(), item, e)) {
            e->accept();
            item->mouseUngrabEvent();
        }
        break;
#ifndef QT_NO_WHEELEVENT
    case QEvent::Wheel:
        QQuickItemPrivate::get(item)->deliverWheelEvent(static_cast<QWheelEvent *>(e));
        break;
#endif
    case QEvent::HoverEnter:
    case QEvent::HoverLeave:
    case QEvent::HoverMove:
        QQuickItemPrivate::get(item)->deliverHoverEvent(static_cast<QHoverEvent *>(e));
        break;
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
        d->sendFilteredTouchEvent(item->parentItem(), item, static_cast<QTouchEvent *>(e));
        break;
    case QEvent::TouchCancel:
        QQuickItemPrivate::get(item)->deliverTouchEvent(static_cast<QTouchEvent *>(e));
        break;
#ifndef QT_NO_DRAGANDDROP
    case QEvent::DragEnter:
    case QEvent::DragMove:
    case QEvent::DragLeave:
    case QEvent::Drop:
        QQuickItemPrivate::get(item)->deliverDragEvent(e);
        break;
#endif
    default:
        break;
    }

    return false;
}

void QQuickWindowPrivate::cleanupNodes()
{
    for (int ii = 0; ii < cleanupNodeList.count(); ++ii)
        delete cleanupNodeList.at(ii);
    cleanupNodeList.clear();
}

void QQuickWindowPrivate::cleanupNodesOnShutdown(QQuickItem *item)
{
    QQuickItemPrivate *p = QQuickItemPrivate::get(item);
    if (p->itemNodeInstance) {
        delete p->itemNodeInstance;
        p->itemNodeInstance = 0;

        if (p->extra.isAllocated()) {
            p->extra->opacityNode = 0;
            p->extra->clipNode = 0;
            p->extra->rootNode = 0;
        }

        p->groupNode = 0;
        p->paintNode = 0;
    }

    for (int ii = 0; ii < p->childItems.count(); ++ii)
        cleanupNodesOnShutdown(p->childItems.at(ii));
}

// This must be called from the render thread, with the main thread frozen
void QQuickWindowPrivate::cleanupNodesOnShutdown()
{
    Q_Q(QQuickWindow);
    cleanupNodes();
    cleanupNodesOnShutdown(contentItem);
    QSet<QQuickItem *>::const_iterator it = parentlessItems.begin();
    for (; it != parentlessItems.end(); ++it)
        cleanupNodesOnShutdown(*it);
    q->cleanupSceneGraph();
}

void QQuickWindowPrivate::updateDirtyNodes()
{
#ifdef DIRTY_DEBUG
    qWarning() << "QQuickWindowPrivate::updateDirtyNodes():";
#endif

    cleanupNodes();

    QQuickItem *updateList = dirtyItemList;
    dirtyItemList = 0;
    if (updateList) QQuickItemPrivate::get(updateList)->prevDirtyItem = &updateList;

    while (updateList) {
        QQuickItem *item = updateList;
        QQuickItemPrivate *itemPriv = QQuickItemPrivate::get(item);
        itemPriv->removeFromDirtyList();

#ifdef DIRTY_DEBUG
        qWarning() << "   QSGNode:" << item << qPrintable(itemPriv->dirtyToString());
#endif
        updateDirtyNode(item);
    }
}

void QQuickWindowPrivate::updateDirtyNode(QQuickItem *item)
{
#ifdef QML_RUNTIME_TESTING
    bool didFlash = false;
#endif

    QQuickItemPrivate *itemPriv = QQuickItemPrivate::get(item);
    quint32 dirty = itemPriv->dirtyAttributes;
    itemPriv->dirtyAttributes = 0;

    if ((dirty & QQuickItemPrivate::TransformUpdateMask) ||
        (dirty & QQuickItemPrivate::Size && itemPriv->origin() != QQuickItem::TopLeft &&
         (itemPriv->scale() != 1. || itemPriv->rotation() != 0.))) {

        QMatrix4x4 matrix;

        if (itemPriv->x != 0. || itemPriv->y != 0.)
            matrix.translate(itemPriv->x, itemPriv->y);

        for (int ii = itemPriv->transforms.count() - 1; ii >= 0; --ii)
            itemPriv->transforms.at(ii)->applyTo(&matrix);

        if (itemPriv->scale() != 1. || itemPriv->rotation() != 0.) {
            QPointF origin = item->transformOriginPoint();
            matrix.translate(origin.x(), origin.y());
            if (itemPriv->scale() != 1.)
                matrix.scale(itemPriv->scale(), itemPriv->scale());
            if (itemPriv->rotation() != 0.)
                matrix.rotate(itemPriv->rotation(), 0, 0, 1);
            matrix.translate(-origin.x(), -origin.y());
        }

        itemPriv->itemNode()->setMatrix(matrix);
    }

    bool clipEffectivelyChanged = (dirty & (QQuickItemPrivate::Clip | QQuickItemPrivate::Window)) &&
                                  ((item->clip() == false) != (itemPriv->clipNode() == 0));
    int effectRefCount = itemPriv->extra.isAllocated()?itemPriv->extra->effectRefCount:0;
    bool effectRefEffectivelyChanged = (dirty & (QQuickItemPrivate::EffectReference | QQuickItemPrivate::Window)) &&
                                  ((effectRefCount == 0) != (itemPriv->rootNode() == 0));

    if (clipEffectivelyChanged) {
        QSGNode *parent = itemPriv->opacityNode() ? (QSGNode *) itemPriv->opacityNode() :
                                                    (QSGNode *)itemPriv->itemNode();
        QSGNode *child = itemPriv->rootNode() ? (QSGNode *)itemPriv->rootNode() :
                                                (QSGNode *)itemPriv->groupNode;

        if (item->clip()) {
            Q_ASSERT(itemPriv->clipNode() == 0);
            itemPriv->extra.value().clipNode = new QQuickDefaultClipNode(item->clipRect());
            itemPriv->clipNode()->update();

            if (child)
                parent->removeChildNode(child);
            parent->appendChildNode(itemPriv->clipNode());
            if (child)
                itemPriv->clipNode()->appendChildNode(child);

        } else {
            Q_ASSERT(itemPriv->clipNode() != 0);
            parent->removeChildNode(itemPriv->clipNode());
            if (child)
                itemPriv->clipNode()->removeChildNode(child);
            delete itemPriv->clipNode();
            itemPriv->extra->clipNode = 0;
            if (child)
                parent->appendChildNode(child);
        }
    }

    if (dirty & QQuickItemPrivate::ChildrenUpdateMask)
        itemPriv->childContainerNode()->removeAllChildNodes();

    if (effectRefEffectivelyChanged) {
        QSGNode *parent = itemPriv->clipNode();
        if (!parent)
            parent = itemPriv->opacityNode();
        if (!parent)
            parent = itemPriv->itemNode();
        QSGNode *child = itemPriv->groupNode;

        if (itemPriv->extra.isAllocated() && itemPriv->extra->effectRefCount) {
            Q_ASSERT(itemPriv->rootNode() == 0);
            itemPriv->extra->rootNode = new QSGRootNode;

            if (child)
                parent->removeChildNode(child);
            parent->appendChildNode(itemPriv->rootNode());
            if (child)
                itemPriv->rootNode()->appendChildNode(child);
        } else {
            Q_ASSERT(itemPriv->rootNode() != 0);
            parent->removeChildNode(itemPriv->rootNode());
            if (child)
                itemPriv->rootNode()->removeChildNode(child);
            delete itemPriv->rootNode();
            itemPriv->extra->rootNode = 0;
            if (child)
                parent->appendChildNode(child);
        }
    }

    if (dirty & QQuickItemPrivate::ChildrenUpdateMask) {
        QSGNode *groupNode = itemPriv->groupNode;
        if (groupNode)
            groupNode->removeAllChildNodes();

        QList<QQuickItem *> orderedChildren = itemPriv->paintOrderChildItems();
        int ii = 0;

        for (; ii < orderedChildren.count() && orderedChildren.at(ii)->z() < 0; ++ii) {
            QQuickItemPrivate *childPrivate = QQuickItemPrivate::get(orderedChildren.at(ii));
            if (!childPrivate->explicitVisible &&
                (!childPrivate->extra.isAllocated() || !childPrivate->extra->effectRefCount))
                continue;
            if (childPrivate->itemNode()->parent())
                childPrivate->itemNode()->parent()->removeChildNode(childPrivate->itemNode());

            itemPriv->childContainerNode()->appendChildNode(childPrivate->itemNode());
        }

        QSGNode *beforePaintNode = itemPriv->groupNode ? itemPriv->groupNode->lastChild() : 0;
        if (beforePaintNode || itemPriv->extra.isAllocated())
            itemPriv->extra.value().beforePaintNode = beforePaintNode;

        if (itemPriv->paintNode)
            itemPriv->childContainerNode()->appendChildNode(itemPriv->paintNode);

        for (; ii < orderedChildren.count(); ++ii) {
            QQuickItemPrivate *childPrivate = QQuickItemPrivate::get(orderedChildren.at(ii));
            if (!childPrivate->explicitVisible &&
                (!childPrivate->extra.isAllocated() || !childPrivate->extra->effectRefCount))
                continue;
            if (childPrivate->itemNode()->parent())
                childPrivate->itemNode()->parent()->removeChildNode(childPrivate->itemNode());

            itemPriv->childContainerNode()->appendChildNode(childPrivate->itemNode());
        }
    }

    if ((dirty & QQuickItemPrivate::Size) && itemPriv->clipNode()) {
        itemPriv->clipNode()->setRect(item->clipRect());
        itemPriv->clipNode()->update();
    }

    if (dirty & (QQuickItemPrivate::OpacityValue | QQuickItemPrivate::Visible
                 | QQuickItemPrivate::HideReference | QQuickItemPrivate::Window))
    {
        qreal opacity = itemPriv->explicitVisible && (!itemPriv->extra.isAllocated() || itemPriv->extra->hideRefCount == 0)
                      ? itemPriv->opacity() : qreal(0);

        if (opacity != 1 && !itemPriv->opacityNode()) {
            itemPriv->extra.value().opacityNode = new QSGOpacityNode;

            QSGNode *parent = itemPriv->itemNode();
            QSGNode *child = itemPriv->clipNode();
            if (!child)
                child = itemPriv->rootNode();
            if (!child)
                child = itemPriv->groupNode;

            if (child)
                parent->removeChildNode(child);
            parent->appendChildNode(itemPriv->opacityNode());
            if (child)
                itemPriv->opacityNode()->appendChildNode(child);
        }
        if (itemPriv->opacityNode())
            itemPriv->opacityNode()->setOpacity(opacity);
    }

    if (dirty & QQuickItemPrivate::ContentUpdateMask) {

        if (itemPriv->flags & QQuickItem::ItemHasContents) {
            updatePaintNodeData.transformNode = itemPriv->itemNode();
            itemPriv->paintNode = item->updatePaintNode(itemPriv->paintNode, &updatePaintNodeData);

            Q_ASSERT(itemPriv->paintNode == 0 ||
                     itemPriv->paintNode->parent() == 0 ||
                     itemPriv->paintNode->parent() == itemPriv->childContainerNode());

            if (itemPriv->paintNode && itemPriv->paintNode->parent() == 0) {
                if (itemPriv->extra.isAllocated() && itemPriv->extra->beforePaintNode)
                    itemPriv->childContainerNode()->insertChildNodeAfter(itemPriv->paintNode, itemPriv->extra->beforePaintNode);
                else
                    itemPriv->childContainerNode()->prependChildNode(itemPriv->paintNode);
            }
        } else if (itemPriv->paintNode) {
            delete itemPriv->paintNode;
            itemPriv->paintNode = 0;
        }
    }

#ifndef QT_NO_DEBUG
    // Check consistency.
    const QSGNode *nodeChain[] = {
        itemPriv->itemNodeInstance,
        itemPriv->opacityNode(),
        itemPriv->clipNode(),
        itemPriv->rootNode(),
        itemPriv->groupNode,
        itemPriv->paintNode,
    };

    int ip = 0;
    for (;;) {
        while (ip < 5 && nodeChain[ip] == 0)
            ++ip;
        if (ip == 5)
            break;
        int ic = ip + 1;
        while (ic < 5 && nodeChain[ic] == 0)
            ++ic;
        const QSGNode *parent = nodeChain[ip];
        const QSGNode *child = nodeChain[ic];
        if (child == 0) {
            Q_ASSERT(parent == itemPriv->groupNode || parent->childCount() == 0);
        } else {
            Q_ASSERT(parent == itemPriv->groupNode || parent->childCount() == 1);
            Q_ASSERT(child->parent() == parent);
            bool containsChild = false;
            for (QSGNode *n = parent->firstChild(); n; n = n->nextSibling())
                containsChild |= (n == child);
            Q_ASSERT(containsChild);
        }
        ip = ic;
    }
#endif

#ifdef QML_RUNTIME_TESTING
    if (itemPriv->sceneGraphContext()->isFlashModeEnabled()) {
        QSGFlashNode *flash = new QSGFlashNode();
        flash->setRect(item->boundingRect());
        itemPriv->childContainerNode()->appendChildNode(flash);
        didFlash = true;
    }
    Q_Q(QQuickWindow);
    if (didFlash) {
        q->maybeUpdate();
    }
#endif

}

void QQuickWindow::maybeUpdate()
{
    Q_D(QQuickWindow);
    d->windowManager->maybeUpdate(this);
}

void QQuickWindow::cleanupSceneGraph()
{
    Q_D(QQuickWindow);

    if (!d->renderer)
        return;

    delete d->renderer->rootNode();
    delete d->renderer;

    d->renderer = 0;
}

void QQuickWindow::setTransientParent_helper(QQuickWindow *window)
{
    setTransientParent(window);
    disconnect(sender(), SIGNAL(windowChanged(QQuickWindow*)),
               this, SLOT(setTransientParent_helper(QQuickWindow*)));
}

/*!
    Returns the opengl context used for rendering.

    If the scene graph is not ready, this function will return 0.

    \sa sceneGraphInitialized(), sceneGraphInvalidated()
 */

QOpenGLContext *QQuickWindow::openglContext() const
{
    Q_D(const QQuickWindow);
    if (d->context->isReady())
        return d->context->glContext();
    return 0;
}

/*!
    \fn void QQuickWindow::frameSwapped()

    This signal is emitted when the frame buffers have been swapped.

    This signal will be emitted from the scene graph rendering thread.
*/


/*!
    \fn void QQuickWindow::sceneGraphInitialized()

    This signal is emitted when the scene graph has been initialized.

    This signal will be emitted from the scene graph rendering thread.

 */


/*!
    \fn void QQuickWindow::sceneGraphInvalidated()

    This signal is emitted when the scene graph has been invalidated.

    This signal implies that the opengl rendering context used
    has been invalidated and all user resources tied to that context
    should be released.

    This signal will be emitted from the scene graph rendering thread.
 */

/*!
    \class QQuickCloseEvent
    \internal
    \since QtQuick 2.1

    \inmodule QtQuick.Window

    \brief Notification that a \l QQuickWindow is about to be closed
*/
/*!
    \qmltype CloseEvent
    \instantiates QQuickCloseEvent
    \inqmlmodule QtQuick.Window 2
    \ingroup qtquick-visual
    \brief Notification that a \l Window is about to be closed
    \since Qt 5.1

    Notification that a window is about to be closed by the windowing system
    (e.g. the user clicked the titlebar close button). The CloseEvent contains
    an accepted property which can be set to false to abort closing the window.

    \sa Window.closing()
*/

/*!
    \qmlproperty bool QtQuick.Window2::CloseEvent::accepted

    This property indicates whether the application will allow the user to
    close the window.  It is true by default.
*/

/*!
    \fn void QQuickWindow::closing()
    \since QtQuick 2.1

    This signal is emitted when the window receives a QCloseEvent from the
    windowing system.
*/

/*!
    \qmlsignal QtQuick.Window2::closing(CloseEvent close)
    \since Qt 5.1

    This signal is emitted when the user tries to close the window.

    This signal includes a closeEvent parameter. The \a close \l accepted
    property is true by default so that the window is allowed to close; but you
    can implement an onClosing() handler and set close.accepted = false if
    you need to do something else before the window can be closed.
 */


/*!
    Sets the render target for this window to be \a fbo.

    The specified fbo must be created in the context of the window
    or one that shares with it.

    \warning
    This function can only be called from the thread doing
    the rendering.
 */

void QQuickWindow::setRenderTarget(QOpenGLFramebufferObject *fbo)
{
    Q_D(QQuickWindow);
    if (d->context && QThread::currentThread() != d->context->thread()) {
        qWarning("QQuickWindow::setRenderThread: Cannot set render target from outside the rendering thread");
        return;
    }

    d->renderTarget = fbo;
    if (fbo) {
        d->renderTargetId = fbo->handle();
        d->renderTargetSize = fbo->size();
    } else {
        d->renderTargetId = 0;
        d->renderTargetSize = QSize();
    }
}

/*!
    \overload

    Sets the render target for this window to be an FBO with
    \a fboId and \a size.

    The specified FBO must be created in the context of the window
    or one that shares with it.

    \warning
    This function can only be called from the thread doing
    the rendering.
*/

void QQuickWindow::setRenderTarget(uint fboId, const QSize &size)
{
    Q_D(QQuickWindow);
    if (d->context && QThread::currentThread() != d->context->thread()) {
        qWarning("QQuickWindow::setRenderThread: Cannot set render target from outside the rendering thread");
        return;
    }

    d->renderTargetId = fboId;
    d->renderTargetSize = size;

    // Unset any previously set instance...
    d->renderTarget = 0;
}


/*!
    Returns the FBO id of the render target when set; otherwise returns 0.
 */
uint QQuickWindow::renderTargetId() const
{
    Q_D(const QQuickWindow);
    return d->renderTargetId;
}

/*!
    Returns the size of the currently set render target; otherwise returns an empty size.
 */
QSize QQuickWindow::renderTargetSize() const
{
    Q_D(const QQuickWindow);
    return d->renderTargetSize;
}




/*!
    Returns the render target for this window.

    The default is to render to the surface of the window, in which
    case the render target is 0.
 */
QOpenGLFramebufferObject *QQuickWindow::renderTarget() const
{
    Q_D(const QQuickWindow);
    return d->renderTarget;
}


/*!
    Grabs the contents of the window and returns it as an image.

    It is possible to call the grabWindow() function when the window is not
    visible. This requires that the window is \l{QWindow::create} {created}
    and has a valid size and that no other QQuickWindow instances are rendering
    in the same process.

    \warning Calling this function will cause performance problems.

    \warning This function can only be called from the GUI thread.
 */
QImage QQuickWindow::grabWindow()
{
    Q_D(QQuickWindow);
    if (!isVisible()) {

        if (d->context->isReady()) {
            qWarning("QQuickWindow::grabWindow: scene graph already in use");
            return QImage();
        }

        if (!handle() || !size().isValid()) {
            qWarning("QQuickWindow::grabWindow: window must be created and have a valid size");
            return QImage();
        }

        QOpenGLContext context;
        context.setFormat(requestedFormat());
        context.create();
        context.makeCurrent(this);
        d->context->initialize(&context);

        d->polishItems();
        d->syncSceneGraph();
        d->renderSceneGraph(size());

        QImage image = qt_gl_read_framebuffer(size(), false, false);
        d->cleanupNodesOnShutdown();
        d->context->invalidate();
        context.doneCurrent();

        return image;
    }

    return d->windowManager->grab(this);
}

/*!
    Returns an incubation controller that splices incubation between frames
    for this window. QQuickView automatically installs this controller for you,
    otherwise you will need to install it yourself using \l{QQmlEngine::setIncubationController()}.

    The controller is owned by the window and will be destroyed when the window
    is deleted.
*/
QQmlIncubationController *QQuickWindow::incubationController() const
{
    Q_D(const QQuickWindow);

    if (!d->incubationController)
        d->incubationController = new QQuickWindowIncubationController(d->windowManager);
    return d->incubationController;
}



/*!
    \enum QQuickWindow::CreateTextureOption

    The CreateTextureOption enums are used to customize a texture is wrapped.

    \value TextureHasAlphaChannel The texture has an alpha channel and should
    be drawn using blending.

    \value TextureHasMipmaps The texture has mipmaps and can be drawn with
    mipmapping enabled.

    \value TextureOwnsGLTexture The texture object owns the texture id and
    will delete the GL texture when the texture object is deleted.

    \value TextureCanUseAtlas The image can be uploaded into a texture atlas.
 */

/*!
    \fn void QQuickWindow::beforeSynchronizing()

    This signal is emitted before the scene graph is synchronized with the QML state.

    This signal can be used to do any preparation required before calls to
    QQuickItem::updatePaintNode().

    The GL context used for rendering the scene graph will be bound at this point.

    \warning This signal is emitted from the scene graph rendering thread. If your
    slot function needs to finish before execution continues, you must make sure that
    the connection is direct (see Qt::ConnectionType).

    \warning Make very sure that a signal handler for beforeSynchronizing leaves the GL
    context in the same state as it was when the signal handler was entered. Failing to
    do so can result in the scene not rendering properly.
*/

/*!
    \fn void QQuickWindow::beforeRendering()

    This signal is emitted before the scene starts rendering.

    Combined with the modes for clearing the background, this option
    can be used to paint using raw GL under QML content.

    The GL context used for rendering the scene graph will be bound
    at this point.

    \warning This signal is emitted from the scene graph rendering thread. If your
    slot function needs to finish before execution continues, you must make sure that
    the connection is direct (see Qt::ConnectionType).

    \warning Make very sure that a signal handler for beforeRendering leaves the GL
    context in the same state as it was when the signal handler was entered. Failing to
    do so can result in the scene not rendering properly.
*/

/*!
    \fn void QQuickWindow::afterRendering()

    This signal is emitted after the scene has completed rendering, before swapbuffers is called.

    This signal can be used to paint using raw GL on top of QML content,
    or to do screen scraping of the current frame buffer.

    The GL context used for rendering the scene graph will be bound at this point.

    \warning This signal is emitted from the scene graph rendering thread. If your
    slot function needs to finish before execution continues, you must make sure that
    the connection is direct (see Qt::ConnectionType).

    \warning Make very sure that a signal handler for afterRendering() leaves the GL
    context in the same state as it was when the signal handler was entered. Failing to
    do so can result in the scene not rendering properly.
 */



/*!
    Sets whether the scene graph rendering of QML should clear the color buffer
    before it starts rendering to \a enabled.

    By disabling clearing of the color buffer, it is possible to do GL painting
    under the scene graph.

    The color buffer is cleared by default.

    \sa beforeRendering()
 */

void QQuickWindow::setClearBeforeRendering(bool enabled)
{
    Q_D(QQuickWindow);
    d->clearBeforeRendering = enabled;
}



/*!
    Returns whether clearing of the color buffer is done before rendering or not.
 */

bool QQuickWindow::clearBeforeRendering() const
{
    Q_D(const QQuickWindow);
    return d->clearBeforeRendering;
}

/*!
    \overload
 */

QSGTexture *QQuickWindow::createTextureFromImage(const QImage &image) const
{
    return createTextureFromImage(image, 0);
}


/*!
    Creates a new QSGTexture from the supplied \a image. If the image has an
    alpha channel, the corresponding texture will have an alpha channel.

    The caller of the function is responsible for deleting the returned texture.
    The actual GL texture will be deleted when the texture object is deleted.

    When \a options contains TextureCanUseAtlas the engine may put the image
    into a texture atlas. Textures in an atlas need to rely on
    QSGTexture::normalizedTextureSubRect() for their geometry and will not
    support QSGTexture::Repeat. Other values from CreateTextureOption are
    ignored.

    \warning This function will return 0 if the scene graph has not yet been
    initialized.

    \warning The returned texture is not memory managed by the scene graph and
    must be explicitly deleted by the caller on the rendering thread.
    This is acheived by deleting the texture from a QSGNode destructor
    or by using deleteLater() in the case where the texture already has affinity
    to the rendering thread.

    This function can be called from any thread.

    \sa sceneGraphInitialized()
 */

QSGTexture *QQuickWindow::createTextureFromImage(const QImage &image, CreateTextureOptions options) const
{
    Q_D(const QQuickWindow);
    if (d->context && d->context->isReady()) {
        if (options & TextureCanUseAtlas)
            return d->context->createTexture(image);
        else
            return d->context->createTextureNoAtlas(image);
    }
    else
        return 0;
}



/*!
    Creates a new QSGTexture object from an existing GL texture \a id and \a size.

    The caller of the function is responsible for deleting the returned texture.

    Use \a options to customize the texture attributes. The TextureUsesAtlas
    option is ignored.

    \warning This function will return 0 if the scenegraph has not yet been
    initialized.

    \sa sceneGraphInitialized()
 */
QSGTexture *QQuickWindow::createTextureFromId(uint id, const QSize &size, CreateTextureOptions options) const
{
    Q_D(const QQuickWindow);
    if (d->context && d->context->isReady()) {
        QSGPlainTexture *texture = new QSGPlainTexture();
        texture->setTextureId(id);
        texture->setHasAlphaChannel(options & TextureHasAlphaChannel);
        texture->setHasMipmaps(options & TextureHasMipmaps);
        texture->setOwnsTexture(options & TextureOwnsGLTexture);
        texture->setTextureSize(size);
        return texture;
    }
    return 0;
}

/*!
    \qmlproperty color QtQuick.Window2::Window::color

    The background color for the window.

    Setting this property is more efficient than using a separate Rectangle.
*/

/*!
    \property QQuickWindow::color
    \brief The color used to clear the OpenGL context.

    Setting the clear color has no effect when clearing is disabled.
    By default, the clear color is white.

    \sa setClearBeforeRendering(), setDefaultAlphaBuffer()
 */

void QQuickWindow::setColor(const QColor &color)
{
    Q_D(QQuickWindow);
    if (color == d->clearColor)
        return;

    if (color.alpha() != d->clearColor.alpha()) {
        QSurfaceFormat fmt = requestedFormat();
        if (color.alpha() < 255)
            fmt.setAlphaBufferSize(8);
        else
            fmt.setAlphaBufferSize(-1);
        setFormat(fmt);
    }
    d->clearColor = color;
    emit colorChanged(color);
    d->dirtyItem(contentItem());
}

QColor QQuickWindow::color() const
{
    return d_func()->clearColor;
}

/*!
    \brief Returns whether to use alpha transparency on newly created windows.

    \since Qt 5.1
    \sa setDefaultAlphaBuffer()
 */
bool QQuickWindow::hasDefaultAlphaBuffer()
{
    return QQuickWindowPrivate::defaultAlphaBuffer;
}

/*!
    \brief \a useAlpha specifies whether to use alpha transparency on newly created windows.
    \since Qt 5.1

    In any application which expects to create translucent windows, it's
    necessary to set this to true before creating the first QQuickWindow,
    because all windows will share the same \l QOpenGLContext. The default
    value is false.
 */
void QQuickWindow::setDefaultAlphaBuffer(bool useAlpha)
{
    QQuickWindowPrivate::defaultAlphaBuffer = useAlpha;
}

/*!
    \qmlproperty string QtQuick.Window2::Window::title

    The window's title in the windowing system.

    The window title might appear in the title area of the window decorations,
    depending on the windowing system and the window flags. It might also
    be used by the windowing system to identify the window in other contexts,
    such as in the task switcher.
 */

/*!
    \qmlproperty Qt::WindowModality QtQuick.Window2::Window::modality

    The modality of the window.

    A modal window prevents other windows from receiving input events.
    Possible values are Qt.NonModal (the default), Qt.WindowModal,
    and Qt.ApplicationModal.
 */

/*!
    \qmlproperty Qt::WindowFlags QtQuick.Window2::Window::flags

    The window flags of the window.

    The window flags control the window's appearance in the windowing system,
    whether it's a dialog, popup, or a regular window, and whether it should
    have a title bar, etc.

    The flags which you read from this property might differ from the ones
    that you set if the requested flags could not be fulfilled.
 */

/*!
    \qmlproperty int QtQuick.Window2::Window::x
    \qmlproperty int QtQuick.Window2::Window::y
    \qmlproperty int QtQuick.Window2::Window::width
    \qmlproperty int QtQuick.Window2::Window::height

    Defines the window's position and size.

    The (x,y) position is relative to the \l Screen if there is only one,
    or to the virtual desktop (arrangement of multiple screens).

    \qml
    Window { x: 100; y: 100; width: 100; height: 100 }
    \endqml

    \image screen-and-window-dimensions.jpg
 */

/*!
    \qmlproperty int QtQuick.Window2::Window::minimumWidth
    \qmlproperty int QtQuick.Window2::Window::minimumHeight
    \since Qt 5.1

    Defines the window's minimum size.

    This is a hint to the window manager to prevent resizing below the specified
    width and height.
 */

/*!
    \qmlproperty int QtQuick.Window2::Window::maximumWidth
    \qmlproperty int QtQuick.Window2::Window::maximumHeight
    \since Qt 5.1

    Defines the window's maximum size.

    This is a hint to the window manager to prevent resizing above the specified
    width and height.
 */

/*!
    \qmlproperty bool QtQuick.Window2::Window::visible

    Whether the window is visible on the screen.

    Setting visible to false is the same as setting \l visibility to Hidden.

    \sa visibility
 */

/*!
    \qmlproperty QWindow::Visibility QtQuick.Window2::Window::visibility

    The screen-occupation state of the window.

    Visibility is whether the window should appear in the windowing system as
    normal, minimized, maximized, fullscreen or hidden.

    To set the visibility to AutomaticVisibility means to give the window a
    default visible state, which might be fullscreen or windowed depending on
    the platform. However when reading the visibility property you will always
    get the actual state, never AutomaticVisibility.

    When a window is not visible its visibility is Hidden, and setting
    visibility to Hidden is the same as setting \l visible to false.

    \sa visible
    \since Qt 5.1
 */

/*!
    \qmlproperty Qt::ScreenOrientation QtQuick.Window2::Window::contentOrientation

    This is a hint to the window manager in case it needs to display
    additional content like popups, dialogs, status bars, or similar
    in relation to the window.

    The recommended orientation is \l Screen.orientation, but
    an application doesn't have to support all possible orientations,
    and thus can opt to ignore the current screen orientation.

    The difference between the window and the content orientation
    determines how much to rotate the content by.

    The default value is Qt::PrimaryOrientation.

    \sa Screen

    \since Qt 5.1
 */

/*!
    \qmlproperty real QtQuick.Window2::Window::opacity

    The opacity of the window.

    If the windowing system supports window opacity, this can be used to fade the
    window in and out, or to make it semitransparent.

    A value of 1.0 or above is treated as fully opaque, whereas a value of 0.0 or below
    is treated as fully transparent. Values inbetween represent varying levels of
    translucency between the two extremes.

    The default value is 1.0.

    \since Qt 5.1
 */

/*!
    \qmlproperty Item QtQuick.Window2::Window::activeFocusItem
    \since Qt 5.1

    The item which currently has active focus or \c null if there is
    no item with active focus.
 */

/*!
    \qmlproperty QtQuick.Window2::Window::active
    \since Qt 5.1

    The active status of the window.

    \sa requestActivate()
 */

/*!
    \qmlmethod QtQuick2::Window::requestActivate()
    \since QtQuick 2.1

    Requests the window to be activated, i.e. receive keyboard focus.
 */

/*!
    \qmlmethod QtQuick2::Window::alert(int msec)
    \since QtQuick 2.1

    Causes an alert to be shown for \a msec miliseconds. If \a msec is \c 0 (the
    default), then the alert is shown indefinitely until the window becomes
    active again.

    In alert state, the window indicates that it demands attention, for example by
    flashing or bouncing the taskbar entry.
*/

#include "moc_qquickwindow.cpp"

QT_END_NAMESPACE
