/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#include "qmlprofilerstatisticsview.h"
#include "qmlprofilerviewmanager.h"
#include "qmlprofilertool.h"

#include <coreplugin/minisplitter.h>
#include <utils/qtcassert.h>
#include <timeline/timelineformattime.h>

#include <QUrl>
#include <QHash>
#include <QStandardItem>
#include <QHeaderView>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMenu>

#include <functional>

namespace QmlProfiler {
namespace Internal {

const int DEFAULT_SORT_COLUMN = MainTimeInPercent;

struct SortPreserver {
    SortPreserver(Utils::TreeView *view) : view(view)
    {
        const QHeaderView *header = view->header();
        column = header->sortIndicatorSection();
        order = header->sortIndicatorOrder();
        view->setSortingEnabled(false);
    }

    ~SortPreserver()
    {
        view->setSortingEnabled(true);
        view->sortByColumn(column, order);
    }

    int column;
    Qt::SortOrder order;
    Utils::TreeView *view;
};

struct Colors {
    Colors () : noteBackground(QColor("orange")), defaultBackground(QColor("white")) {}
    QColor noteBackground;
    QColor defaultBackground;
};

struct RootEventType : public QmlEventType {
    RootEventType() : QmlEventType(MaximumMessage, MaximumRangeType, -1,
                                   QmlEventLocation("<program>", 1, 1),
                                   QmlProfilerStatisticsMainView::tr("Main Program"),
                                   QmlProfilerStatisticsMainView::tr("<program>"))
    {
    }
};

Q_GLOBAL_STATIC(Colors, colors)
Q_GLOBAL_STATIC(RootEventType, rootEventType)

class StatisticsViewItem : public QStandardItem
{
public:
    StatisticsViewItem(const QString &text, const QVariant &sort) : QStandardItem(text)
    {
        setData(sort, SortRole);
    }

    virtual bool operator<(const QStandardItem &other) const
    {
        if (data(SortRole).type() == QVariant::String) {
            // Strings should be case-insensitive compared
            return data(SortRole).toString().compare(other.data(SortRole).toString(),
                                                      Qt::CaseInsensitive) < 0;
        } else {
            // For everything else the standard comparison should be OK
            return QStandardItem::operator<(other);
        }
    }
};

class QmlProfilerStatisticsView::QmlProfilerStatisticsViewPrivate
{
public:
    QmlProfilerStatisticsMainView *m_statsTree;
    QmlProfilerStatisticsRelativesView *m_statsChildren;
    QmlProfilerStatisticsRelativesView *m_statsParents;

    QmlProfilerStatisticsModel *model;
};

static void setViewDefaults(Utils::TreeView *view)
{
    view->setFrameStyle(QFrame::NoFrame);
    QHeaderView *header = view->header();
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setDefaultSectionSize(100);
    header->setMinimumSectionSize(50);
}

static QString displayHeader(MainField header)
{
    switch (header) {
    case MainCallCount:
        return QmlProfilerStatisticsMainView::tr("Calls");
    case MainDetails:
        return QmlProfilerStatisticsMainView::tr("Details");
    case MainLocation:
        return QmlProfilerStatisticsMainView::tr("Location");
    case MainMaxTime:
        return QmlProfilerStatisticsMainView::tr("Longest Time");
    case MainTimePerCall:
        return QmlProfilerStatisticsMainView::tr("Mean Time");
    case MainSelfTime:
        return QmlProfilerStatisticsMainView::tr("Self Time");
    case MainSelfTimeInPercent:
        return QmlProfilerStatisticsMainView::tr("Self Time in Percent");
    case MainMinTime:
        return QmlProfilerStatisticsMainView::tr("Shortest Time");
    case MainTimeInPercent:
        return QmlProfilerStatisticsMainView::tr("Time in Percent");
    case MainTotalTime:
        return QmlProfilerStatisticsMainView::tr("Total Time");
    case MainType:
        return QmlProfilerStatisticsMainView::tr("Type");
    case MainMedianTime:
        return QmlProfilerStatisticsMainView::tr("Median Time");
    case MaxMainField:
        QTC_ASSERT(false, break);
    }
    return QString();
}

static QString displayHeader(RelativeField header, QmlProfilerStatisticsRelation relation)
{
    switch (header) {
    case RelativeLocation:
        return relation == QmlProfilerStatisticsChilden
                ? QmlProfilerStatisticsMainView::tr("Callee")
                : QmlProfilerStatisticsMainView::tr("Caller");
    case RelativeType:
        return displayHeader(MainType);
    case RelativeTotalTime:
        return displayHeader(MainTotalTime);
    case RelativeCallCount:
        return displayHeader(MainCallCount);
    case RelativeDetails:
        return relation == QmlProfilerStatisticsChilden
                ? QmlProfilerStatisticsMainView::tr("Callee Description")
                : QmlProfilerStatisticsMainView::tr("Caller Description");
    case MaxRelativeField:
        QTC_ASSERT(false, break);
    }
    return QString();
}

static void getSourceLocation(QStandardItem *infoItem,
                              std::function<void (const QString &, int, int)> receiver)
{
    int line = infoItem->data(LineRole).toInt();
    int column = infoItem->data(ColumnRole).toInt();
    QString fileName = infoItem->data(FilenameRole).toString();
    if (line != -1 && !fileName.isEmpty())
        receiver(fileName, line, column);
}

QmlProfilerStatisticsView::QmlProfilerStatisticsView(QmlProfilerModelManager *profilerModelManager,
                                                     QWidget *parent)
    : QmlProfilerEventsView(parent), d(new QmlProfilerStatisticsViewPrivate)
{
    setObjectName(QLatin1String("QmlProfiler.Statistics.Dock"));
    setWindowTitle(tr("Statistics"));

    d->model = new QmlProfilerStatisticsModel(profilerModelManager, this);

    d->m_statsTree = new QmlProfilerStatisticsMainView(this, d->model);
    connect(d->m_statsTree, &QmlProfilerStatisticsMainView::gotoSourceLocation,
            this, &QmlProfilerStatisticsView::gotoSourceLocation);
    connect(d->m_statsTree, &QmlProfilerStatisticsMainView::typeSelected,
            this, &QmlProfilerStatisticsView::typeSelected);

    d->m_statsChildren = new QmlProfilerStatisticsRelativesView(
                new QmlProfilerStatisticsRelativesModel(profilerModelManager, d->model,
                                                        QmlProfilerStatisticsChilden, this),
                this);
    d->m_statsParents = new QmlProfilerStatisticsRelativesView(
                new QmlProfilerStatisticsRelativesModel(profilerModelManager, d->model,
                                                        QmlProfilerStatisticsParents, this),
                this);
    connect(d->m_statsTree, &QmlProfilerStatisticsMainView::typeSelected,
            d->m_statsChildren, &QmlProfilerStatisticsRelativesView::displayType);
    connect(d->m_statsTree, &QmlProfilerStatisticsMainView::typeSelected,
            d->m_statsParents, &QmlProfilerStatisticsRelativesView::displayType);
    connect(d->m_statsChildren, &QmlProfilerStatisticsRelativesView::typeClicked,
            d->m_statsTree, &QmlProfilerStatisticsMainView::selectType);
    connect(d->m_statsParents, &QmlProfilerStatisticsRelativesView::typeClicked,
            d->m_statsTree, &QmlProfilerStatisticsMainView::selectType);
    connect(d->m_statsChildren, &QmlProfilerStatisticsRelativesView::gotoSourceLocation,
            this, &QmlProfilerStatisticsView::gotoSourceLocation);
    connect(d->m_statsParents, &QmlProfilerStatisticsRelativesView::gotoSourceLocation,
            this, &QmlProfilerStatisticsView::gotoSourceLocation);

    // widget arrangement
    QVBoxLayout *groupLayout = new QVBoxLayout;
    groupLayout->setContentsMargins(0,0,0,0);
    groupLayout->setSpacing(0);

    Core::MiniSplitter *splitterVertical = new Core::MiniSplitter;
    splitterVertical->addWidget(d->m_statsTree);
    Core::MiniSplitter *splitterHorizontal = new Core::MiniSplitter;
    splitterHorizontal->addWidget(d->m_statsParents);
    splitterHorizontal->addWidget(d->m_statsChildren);
    splitterHorizontal->setOrientation(Qt::Horizontal);
    splitterVertical->addWidget(splitterHorizontal);
    splitterVertical->setOrientation(Qt::Vertical);
    splitterVertical->setStretchFactor(0,5);
    splitterVertical->setStretchFactor(1,2);
    groupLayout->addWidget(splitterVertical);
    setLayout(groupLayout);
}

QmlProfilerStatisticsView::~QmlProfilerStatisticsView()
{
    delete d->model;
    delete d;
}

void QmlProfilerStatisticsView::clear()
{
    d->m_statsTree->clear();
    d->m_statsChildren->clear();
    d->m_statsParents->clear();
}

QString QmlProfilerStatisticsView::summary(const QVector<int> &typeIds) const
{
    const double cutoff = 0.1;
    const double round = 0.05;
    double maximum = 0;
    double sum = 0;

    for (int typeId : typeIds) {
        const double percentage = d->model->durationPercent(typeId);
        if (percentage > maximum)
            maximum = percentage;
        sum += percentage;
    }

    const QLatin1Char percent('%');

    if (sum < cutoff)
        return QLatin1Char('<') + QString::number(cutoff, 'f', 1) + percent;

    if (typeIds.length() == 1)
        return QLatin1Char('~') + QString::number(maximum, 'f', 1) + percent;

    // add/subtract 0.05 to avoid problematic rounding
    if (maximum < cutoff)
        return QChar(0x2264) + QString::number(sum + round, 'f', 1) + percent;

    return QChar(0x2265) + QString::number(qMax(maximum - round, cutoff), 'f', 1) + percent;
}

QStringList QmlProfilerStatisticsView::details(int typeId) const
{
    const QmlEventType &type = d->model->getTypes()[typeId];

    const QChar ellipsisChar(0x2026);
    const int maxColumnWidth = 32;

    QString data = type.data();
    if (data.length() > maxColumnWidth)
        data = data.left(maxColumnWidth - 1) + ellipsisChar;

    return QStringList({
        QmlProfilerStatisticsMainView::nameForType(type.rangeType()),
        data,
        QString::number(d->model->durationPercent(typeId), 'f', 2) + QLatin1Char('%')
    });
}

QModelIndex QmlProfilerStatisticsView::selectedModelIndex() const
{
    return d->m_statsTree->selectedModelIndex();
}

void QmlProfilerStatisticsView::contextMenuEvent(QContextMenuEvent *ev)
{
    QMenu menu;
    QAction *copyRowAction = 0;
    QAction *copyTableAction = 0;
    QAction *showExtendedStatsAction = 0;
    QAction *getGlobalStatsAction = 0;

    QPoint position = ev->globalPos();

    QList <QAction *> commonActions = QmlProfilerTool::profilerContextMenuActions();
    foreach (QAction *act, commonActions)
        menu.addAction(act);

    if (mouseOnTable(position)) {
        menu.addSeparator();
        if (selectedModelIndex().isValid())
            copyRowAction = menu.addAction(tr("Copy Row"));
        copyTableAction = menu.addAction(tr("Copy Table"));

        showExtendedStatsAction = menu.addAction(tr("Extended Event Statistics"));
        showExtendedStatsAction->setCheckable(true);
        showExtendedStatsAction->setChecked(showExtendedStatistics());
    }

    menu.addSeparator();
    getGlobalStatsAction = menu.addAction(tr("Show Full Range"));
    if (!d->model->modelManager()->isRestrictedToRange())
        getGlobalStatsAction->setEnabled(false);

    QAction *selectedAction = menu.exec(position);

    if (selectedAction) {
        if (selectedAction == copyRowAction)
            copyRowToClipboard();
        if (selectedAction == copyTableAction)
            copyTableToClipboard();
        if (selectedAction == getGlobalStatsAction)
            emit showFullRange();
        if (selectedAction == showExtendedStatsAction)
            setShowExtendedStatistics(!showExtendedStatistics());
    }
}

bool QmlProfilerStatisticsView::mouseOnTable(const QPoint &position) const
{
    QPoint tableTopLeft = d->m_statsTree->mapToGlobal(QPoint(0,0));
    QPoint tableBottomRight = d->m_statsTree->mapToGlobal(QPoint(d->m_statsTree->width(), d->m_statsTree->height()));
    return (position.x() >= tableTopLeft.x() && position.x() <= tableBottomRight.x() && position.y() >= tableTopLeft.y() && position.y() <= tableBottomRight.y());
}

void QmlProfilerStatisticsView::copyTableToClipboard() const
{
    d->m_statsTree->copyTableToClipboard();
}

void QmlProfilerStatisticsView::copyRowToClipboard() const
{
    d->m_statsTree->copyRowToClipboard();
}

void QmlProfilerStatisticsView::selectByTypeId(int typeIndex)
{
    if (d->m_statsTree->selectedTypeId() != typeIndex)
        d->m_statsTree->selectType(typeIndex);
}

void QmlProfilerStatisticsView::onVisibleFeaturesChanged(quint64 features)
{
    d->model->restrictToFeatures(features);
}

void QmlProfilerStatisticsView::setShowExtendedStatistics(bool show)
{
    d->m_statsTree->setShowExtendedStatistics(show);
}

bool QmlProfilerStatisticsView::showExtendedStatistics() const
{
    return d->m_statsTree->showExtendedStatistics();
}

class QmlProfilerStatisticsMainView::QmlProfilerStatisticsMainViewPrivate
{
public:
    QmlProfilerStatisticsMainViewPrivate(QmlProfilerStatisticsMainView *qq) : q(qq) {}

    QString textForItem(QStandardItem *item) const;

    QmlProfilerStatisticsMainView *q;

    QmlProfilerStatisticsModel *model;
    QStandardItemModel *m_model;
    bool m_showExtendedStatistics;
};

QmlProfilerStatisticsMainView::QmlProfilerStatisticsMainView(
        QWidget *parent, QmlProfilerStatisticsModel *model) :
    Utils::TreeView(parent), d(new QmlProfilerStatisticsMainViewPrivate(this))
{
    setViewDefaults(this);
    setObjectName(QLatin1String("QmlProfilerEventsTable"));

    d->m_model = new QStandardItemModel(this);
    d->m_model->setSortRole(SortRole);
    setModel(d->m_model);
    connect(this, &QAbstractItemView::activated, this, &QmlProfilerStatisticsMainView::jumpToItem);

    d->model = model;
    connect(d->model, &QmlProfilerStatisticsModel::dataAvailable,
            this, &QmlProfilerStatisticsMainView::buildModel);
    connect(d->model, &QmlProfilerStatisticsModel::notesAvailable,
            this, &QmlProfilerStatisticsMainView::updateNotes);
    d->m_showExtendedStatistics = false;

    setSortingEnabled(true);
    sortByColumn(DEFAULT_SORT_COLUMN, Qt::DescendingOrder);

    buildModel();
}

QmlProfilerStatisticsMainView::~QmlProfilerStatisticsMainView()
{
    clear();
    delete d->m_model;
    delete d;
}

void QmlProfilerStatisticsMainView::setHeaderLabels()
{
    for (int i = 0; i < MaxMainField; ++i)
        d->m_model->setHeaderData(i, Qt::Horizontal, displayHeader(static_cast<MainField>(i)));
}

void QmlProfilerStatisticsMainView::setShowExtendedStatistics(bool show)
{
    // Not checking if already set because we don't want the first call to skip
    d->m_showExtendedStatistics = show;
    if (show) {
        showColumn(MainMedianTime);
        showColumn(MainMaxTime);
        showColumn(MainMinTime);
    } else {
        hideColumn(MainMedianTime);
        hideColumn(MainMaxTime);
        hideColumn(MainMinTime);
    }
}

bool QmlProfilerStatisticsMainView::showExtendedStatistics() const
{
    return d->m_showExtendedStatistics;
}

void QmlProfilerStatisticsMainView::clear()
{
    SortPreserver sorter(this);
    d->m_model->clear();
    d->m_model->setColumnCount(MaxMainField);
    setHeaderLabels();
}

void QmlProfilerStatisticsMainView::buildModel()
{
    clear();

    {
        SortPreserver sorter(this);
        parseModel();
        setShowExtendedStatistics(d->m_showExtendedStatistics);
        setRootIsDecorated(false);
    }

    resizeColumnToContents(MainLocation);
    resizeColumnToContents(MainType);
}

void QmlProfilerStatisticsMainView::updateNotes(int typeIndex)
{
    const QHash<int, QmlProfilerStatisticsModel::QmlEventStats> &eventList = d->model->getData();
    const QHash<int, QString> &noteList = d->model->getNotes();
    QStandardItem *parentItem = d->m_model->invisibleRootItem();

    for (int rowIndex = 0; rowIndex < parentItem->rowCount(); ++rowIndex) {
        int rowType = parentItem->child(rowIndex)->data(TypeIdRole).toInt();
        if (rowType != typeIndex && typeIndex != -1)
            continue;
        const QmlProfilerStatisticsModel::QmlEventStats &stats = eventList[rowType];

        for (int columnIndex = 0; columnIndex < parentItem->columnCount(); ++columnIndex) {
            QStandardItem *item = parentItem->child(rowIndex, columnIndex);
            QHash<int, QString>::ConstIterator it = noteList.find(rowType);
            if (it != noteList.end()) {
                item->setBackground(colors()->noteBackground);
                item->setToolTip(it.value());
            } else if (stats.durationRecursive > 0) {
                item->setBackground(colors()->noteBackground);
                item->setToolTip(tr("+%1 in recursive calls")
                                 .arg(Timeline::formatTime(stats.durationRecursive)));
            } else if (!item->toolTip().isEmpty()){
                item->setBackground(colors()->defaultBackground);
                item->setToolTip(QString());
            }
        }
    }
}

void QmlProfilerStatisticsMainView::parseModel()
{
    const QHash<int, QmlProfilerStatisticsModel::QmlEventStats> &eventList = d->model->getData();
    const QVector<QmlEventType> &typeList = d->model->getTypes();

    QHash<int, QmlProfilerStatisticsModel::QmlEventStats>::ConstIterator it;
    for (it = eventList.constBegin(); it != eventList.constEnd(); ++it) {
        int typeIndex = it.key();
        const QmlProfilerStatisticsModel::QmlEventStats &stats = it.value();
        const QmlEventType &type = (typeIndex != -1 ? typeList[typeIndex] : *rootEventType());
        QStandardItem *parentItem = d->m_model->invisibleRootItem();
        QList<QStandardItem *> newRow;

        newRow << new StatisticsViewItem(
                      type.displayName().isEmpty() ? tr("<bytecode>") : type.displayName(),
                      type.displayName());

        QString typeString = QmlProfilerStatisticsMainView::nameForType(type.rangeType());
        newRow << new StatisticsViewItem(typeString, typeString);

        const double percent = d->model->durationPercent(typeIndex);
        newRow << new StatisticsViewItem(QString::number(percent, 'f', 2)
                                         + QLatin1String(" %"), percent);

        newRow << new StatisticsViewItem(
                      Timeline::formatTime(stats.duration - stats.durationRecursive),
                      stats.duration - stats.durationRecursive);

        const double percentSelf = d->model->durationSelfPercent(typeIndex);
        newRow << new StatisticsViewItem(QString::number(percentSelf, 'f', 2)
                                         + QLatin1String(" %"), percentSelf);

        newRow << new StatisticsViewItem(Timeline::formatTime(stats.durationSelf),
                                         stats.durationSelf);

        newRow << new StatisticsViewItem(QString::number(stats.calls), stats.calls);

        const qint64 timePerCall = stats.calls > 0 ? stats.duration / stats.calls : 0;
        newRow << new StatisticsViewItem(Timeline::formatTime(timePerCall),
                                         timePerCall);

        newRow << new StatisticsViewItem(Timeline::formatTime(stats.medianTime),
                                         stats.medianTime);

        newRow << new StatisticsViewItem(Timeline::formatTime(stats.maxTime),
                                         stats.maxTime);

        newRow << new StatisticsViewItem(Timeline::formatTime(stats.minTime),
                                         stats.minTime);

        newRow << new StatisticsViewItem(type.data().isEmpty() ? tr("Source code not available")
                                                               : type.data(), type.data());

        // no edit
        foreach (QStandardItem *item, newRow)
            item->setEditable(false);

        // metadata
        QStandardItem *first = newRow.at(MainLocation);
        first->setData(typeIndex, TypeIdRole);
        const QmlEventLocation location(type.location());
        first->setData(location.filename(), FilenameRole);
        first->setData(location.line(), LineRole);
        first->setData(location.column(), ColumnRole);

        // append
        parentItem->appendRow(newRow);
    }
}

QStandardItem *QmlProfilerStatisticsMainView::itemFromIndex(const QModelIndex &index) const
{
    QStandardItem *indexItem = d->m_model->itemFromIndex(index);
    if (indexItem->parent())
        return indexItem->parent()->child(indexItem->row());
    else
        return d->m_model->item(index.row());

}

QString QmlProfilerStatisticsMainView::nameForType(RangeType typeNumber)
{
    switch (typeNumber) {
    case Painting: return QmlProfilerStatisticsMainView::tr("Painting");
    case Compiling: return QmlProfilerStatisticsMainView::tr("Compiling");
    case Creating: return QmlProfilerStatisticsMainView::tr("Creating");
    case Binding: return QmlProfilerStatisticsMainView::tr("Binding");
    case HandlingSignal: return QmlProfilerStatisticsMainView::tr("Handling Signal");
    case Javascript: return QmlProfilerStatisticsMainView::tr("JavaScript");
    default: return QString();
    }
}

int QmlProfilerStatisticsMainView::selectedTypeId() const
{
    QModelIndex index = selectedModelIndex();
    if (!index.isValid())
        return -1;
    QStandardItem *item = d->m_model->item(index.row());
    return item->data(TypeIdRole).toInt();
}

void QmlProfilerStatisticsMainView::jumpToItem(const QModelIndex &index)
{
    QStandardItem *infoItem = itemFromIndex(index);

    // show in editor
    getSourceLocation(infoItem, [this](const QString &fileName, int line, int column) {
        emit gotoSourceLocation(fileName, line, column);
    });

    // show in callers/callees subwindow
    emit typeSelected(infoItem->data(TypeIdRole).toInt());
}

void QmlProfilerStatisticsMainView::selectItem(const QStandardItem *item)
{
    // If the same item is already selected, don't reselect it.
    QModelIndex index = d->m_model->indexFromItem(item);
    if (index != currentIndex()) {
        setCurrentIndex(index);

        // show in callers/callees subwindow
        emit typeSelected(itemFromIndex(index)->data(TypeIdRole).toInt());
    }
}

void QmlProfilerStatisticsMainView::selectType(int typeIndex)
{
    for (int i=0; i<d->m_model->rowCount(); i++) {
        QStandardItem *infoItem = d->m_model->item(i);
        if (infoItem->data(TypeIdRole).toInt() == typeIndex) {
            selectItem(infoItem);
            return;
        }
    }
}

QModelIndex QmlProfilerStatisticsMainView::selectedModelIndex() const
{
    QModelIndexList sel = selectedIndexes();
    if (sel.isEmpty())
        return QModelIndex();
    else
        return sel.first();
}

QString QmlProfilerStatisticsMainView::QmlProfilerStatisticsMainViewPrivate::textForItem(
        QStandardItem *item) const
{
    QString str;

    // item's data
    int colCount = m_model->columnCount();
    for (int j = 0; j < colCount; ++j) {
        QStandardItem *colItem = item->parent() ? item->parent()->child(item->row(),j) :
                                                  m_model->item(item->row(),j);
        str += colItem->data(Qt::DisplayRole).toString();
        if (j < colCount-1) str += QLatin1Char('\t');
    }
    str += QLatin1Char('\n');

    return str;
}

void QmlProfilerStatisticsMainView::copyTableToClipboard() const
{
    QString str;
    // headers
    int columnCount = d->m_model->columnCount();
    for (int i = 0; i < columnCount; ++i) {
        str += d->m_model->headerData(i, Qt::Horizontal, Qt::DisplayRole).toString();
        if (i < columnCount - 1)
            str += QLatin1Char('\t');
        else
            str += QLatin1Char('\n');
    }
    // data
    int rowCount = d->m_model->rowCount();
    for (int i = 0; i != rowCount; ++i) {
        str += d->textForItem(d->m_model->item(i));
    }
    QClipboard *clipboard = QApplication::clipboard();
    clipboard->setText(str, QClipboard::Selection);
    clipboard->setText(str, QClipboard::Clipboard);
}

void QmlProfilerStatisticsMainView::copyRowToClipboard() const
{
    QString str = d->textForItem(d->m_model->itemFromIndex(selectedModelIndex()));
    QClipboard *clipboard = QApplication::clipboard();
    clipboard->setText(str, QClipboard::Selection);
    clipboard->setText(str, QClipboard::Clipboard);
}

class QmlProfilerStatisticsRelativesView::QmlProfilerStatisticsRelativesViewPrivate
{
public:
    QmlProfilerStatisticsRelativesViewPrivate(QmlProfilerStatisticsRelativesView *qq):q(qq) {}
    ~QmlProfilerStatisticsRelativesViewPrivate() {}

    QmlProfilerStatisticsRelativesModel *model;

    QmlProfilerStatisticsRelativesView *q;
};

QmlProfilerStatisticsRelativesView::QmlProfilerStatisticsRelativesView(
        QmlProfilerStatisticsRelativesModel *model, QWidget *parent) :
    Utils::TreeView(parent), d(new QmlProfilerStatisticsRelativesViewPrivate(this))
{
    setViewDefaults(this);
    d->model = model;
    QStandardItemModel *itemModel = new QStandardItemModel(this);
    itemModel->setSortRole(SortRole);
    setModel(itemModel);
    setRootIsDecorated(false);
    updateHeader();

    setSortingEnabled(true);
    sortByColumn(DEFAULT_SORT_COLUMN, Qt::DescendingOrder);

    connect(this, &QAbstractItemView::activated,
            this, &QmlProfilerStatisticsRelativesView::jumpToItem);

    // Clear when new data available as the selection may be invalid now.
    connect(d->model, &QmlProfilerStatisticsRelativesModel::dataAvailable,
            this, &QmlProfilerStatisticsRelativesView::clear);
}

QmlProfilerStatisticsRelativesView::~QmlProfilerStatisticsRelativesView()
{
    delete d;
}

void QmlProfilerStatisticsRelativesView::displayType(int typeIndex)
{
    SortPreserver sorter(this);
    rebuildTree(d->model->getData(typeIndex));

    updateHeader();
    resizeColumnToContents(RelativeLocation);
}

void QmlProfilerStatisticsRelativesView::rebuildTree(
        const QmlProfilerStatisticsRelativesModel::QmlStatisticsRelativesMap &map)
{
    Q_ASSERT(treeModel());
    treeModel()->clear();

    QStandardItem *topLevelItem = treeModel()->invisibleRootItem();
    const QVector<QmlEventType> &typeList = d->model->getTypes();

    QmlProfilerStatisticsRelativesModel::QmlStatisticsRelativesMap::const_iterator it;
    for (it = map.constBegin(); it != map.constEnd(); ++it) {
        const QmlProfilerStatisticsRelativesModel::QmlStatisticsRelativesData &stats = it.value();
        int typeIndex = it.key();
        const QmlEventType &type = (typeIndex != -1 ? typeList[typeIndex] : *rootEventType());
        QList<QStandardItem *> newRow;

        // ToDo: here we were going to search for the data in the other model
        // maybe we should store the data in this model and get it here
        // no indirections at this level of abstraction!
        newRow << new StatisticsViewItem(
                      type.displayName().isEmpty() ? tr("<bytecode>") : type.displayName(),
                      type.displayName());
        const QString typeName = QmlProfilerStatisticsMainView::nameForType(type.rangeType());
        newRow << new StatisticsViewItem(typeName, typeName);
        newRow << new StatisticsViewItem(Timeline::formatTime(stats.duration),
                                         stats.duration);
        newRow << new StatisticsViewItem(QString::number(stats.calls), stats.calls);
        newRow << new StatisticsViewItem(type.data().isEmpty() ? tr("Source code not available") :
                                                                 type.data(), type.data());

        QStandardItem *first = newRow.at(RelativeLocation);
        first->setData(typeIndex, TypeIdRole);
        const QmlEventLocation location(type.location());
        first->setData(location.filename(), FilenameRole);
        first->setData(location.line(), LineRole);
        first->setData(location.column(), ColumnRole);

        if (stats.isRecursive) {
            foreach (QStandardItem *item, newRow) {
                item->setBackground(colors()->noteBackground);
                item->setToolTip(tr("called recursively"));
            }
        }

        foreach (QStandardItem *item, newRow)
            item->setEditable(false);

        topLevelItem->appendRow(newRow);
    }
}

void QmlProfilerStatisticsRelativesView::clear()
{
    if (treeModel()) {
        SortPreserver sorter(this);
        treeModel()->clear();
        updateHeader();
    }
}

void QmlProfilerStatisticsRelativesView::updateHeader()
{
    const QmlProfilerStatisticsRelation relation = d->model->relation();
    if (QStandardItemModel *model = treeModel()) {
        model->setColumnCount(MaxRelativeField);
        for (int i = 0; i < MaxRelativeField; ++i) {
            model->setHeaderData(i, Qt::Horizontal,
                                 displayHeader(static_cast<RelativeField>(i), relation));
        }
    }
}

QStandardItemModel *QmlProfilerStatisticsRelativesView::treeModel()
{
    return qobject_cast<QStandardItemModel *>(model());
}

void QmlProfilerStatisticsRelativesView::jumpToItem(const QModelIndex &index)
{
    if (treeModel()) {
        QStandardItem *infoItem = treeModel()->item(index.row());
        // show in editor
        getSourceLocation(infoItem, [this](const QString &fileName, int line, int column) {
            emit gotoSourceLocation(fileName, line, column);
        });

        emit typeClicked(infoItem->data(TypeIdRole).toInt());
    }
}

} // namespace Internal
} // namespace QmlProfiler
