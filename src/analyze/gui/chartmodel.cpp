/*
    SPDX-FileCopyrightText: 2015-2017 Milian Wolff <mail@milianw.de>

    SPDX-License-Identifier: LGPL-2.1-or-later
*/

#include "chartmodel.h"

#include <KChartGlobal>
#include <KChartLineAttributes>
#include <KLocalizedString>

#include <QBrush>
#include <QDebug>
#include <QPen>

#include "util.h"

#include <algorithm>

namespace {
QColor colorForColumn(int column, int columnCount)
{
    // The total cost graph (column 0) is always red.
    if (column == 0) {
        return Qt::red;
    } else {
        return QColor::fromHsv((double(column + 1) / columnCount) * 255, 255, 255);
    }
}
}

ChartModel::ChartModel(Type type, QObject* parent)
    : QAbstractTableModel(parent)
    , m_type(type)
    , m_maxDatasetCount(11)
{
    qRegisterMetaType<ChartData>();
}

ChartModel::~ChartModel() = default;

ChartModel::Type ChartModel::type() const
{
    return m_type;
}

QString ChartModel::typeString() const
{
    switch (m_type) {
    case Allocations:
        return i18n("Memory Allocations");
    case Consumed:
        return i18n("Memory Consumed");
    case Temporary:
        return i18n("Temporary Allocations");
    default:
        return QString();
    }
}

QVariant ChartModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (section < 0 || section >= columnCount() || orientation != Qt::Horizontal) {
        return {};
    }

    if (role == KChart::DatasetPenRole) {
        return QVariant::fromValue(m_columnDataSetPens.at(section));
    } else if (role == KChart::DatasetBrushRole) {
        return QVariant::fromValue(m_columnDataSetBrushes.at(section));
    }

    if (role == Qt::ToolTipRole) {
        if (section == 0) {
            return i18n("Elapsed Time");
        }
        return typeString();
    } else if (role == Qt::DisplayRole) {
        if (section == 0) {
            switch (m_type) {
            case Allocations:
                return i18n("Total Memory Allocations");
            case Consumed:
                return i18n("Total Memory Consumption");
            case Temporary:
                return i18n("Total Temporary Allocations");
            }
        } else {
            auto id = m_data.labels.value(section / 2).functionId;
            QString label = m_data.resultData->string(id);

            // Experimental symbol name shortening, currently only truncating
            // and left-justified labels. The length is also fixed and should
            // be adjusted later on.
            //
            // The justified text is also a workaround to setTextAlignment as
            // this does not seem to work on KChartLegend objects. This might
            // be because it is not supported for these instances, as the re-
            // ference suggests: https://doc.qt.io/qt-6/stylesheet-reference.html
            // (see "text-align" entry).
            int symbolNameLength = 60;

            if (label.size() < symbolNameLength) {
                label = label.leftJustified(symbolNameLength);
            } else if (label.size() > symbolNameLength) {
                label.truncate(symbolNameLength - 3);
                label = label.leftJustified(symbolNameLength, QLatin1Char('.'));
            }

            label = label.rightJustified(symbolNameLength + 1);

            return i18n("%1", label);
        }
    }

    return {};
}

QVariant ChartModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid()) {
        return {};
    }
    Q_ASSERT(index.row() >= 0 && index.row() < rowCount(index.parent()));
    Q_ASSERT(index.column() >= 0 && index.column() < columnCount(index.parent()));
    Q_ASSERT(!index.parent().isValid());

    if (role == KChart::LineAttributesRole) {
        KChart::LineAttributes attributes;
        attributes.setDisplayArea(true);
        if (index.column() > 1) {
            attributes.setTransparency(127);
        } else {
            attributes.setTransparency(50);
        }
        return QVariant::fromValue(attributes);
    }

    if (role == KChart::DatasetPenRole) {
        return QVariant::fromValue(m_columnDataSetPens.at(index.column()));
    } else if (role == KChart::DatasetBrushRole) {
        return QVariant::fromValue(m_columnDataSetBrushes.at(index.column()));
    }

    if (role != Qt::DisplayRole && role != Qt::ToolTipRole) {
        return {};
    }

    const auto& data = m_data.rows.at(index.row());

    int column = index.column();
    if (role != Qt::ToolTipRole && column % 2 == 0) {
        return data.timeStamp;
    }
    column = column / 2;
    Q_ASSERT(column < ChartRows::MAX_NUM_COST);

    const auto cost = data.cost[column];
    if (role == Qt::ToolTipRole) {
        const QString time = Util::formatTime(data.timeStamp);
        auto byteCost = [cost]() -> QString {
            const auto formatted = Util::formatBytes(cost);
            if (cost > 1024) {
                return i18nc("%1: the formatted byte size, e.g. \"1.2KB\", %2: the raw byte size, e.g. \"1300\"",
                             "%1 (%2 bytes)", formatted, cost);
            } else {
                return formatted;
            }
        };
        if (column == 0) {
            switch (m_type) {
            case Allocations:
                return i18n("<qt>%1 allocations in total after %2</qt>", cost, time);
            case Temporary:
                return i18n("<qt>%1 temporary allocations in total after %2</qt>", cost, time);
            case Consumed:
                return i18n("<qt>%1 consumed in total after %2</qt>", byteCost(), time);
            }
        } else {
            auto label = Util::toString(m_data.labels.value(column), *m_data.resultData, Util::Long);
            switch (m_type) {
            case Allocations:
                return i18n("<qt>%2 allocations after %3 from:<p "
                            "style='margin-left:10px;'>%1</p></qt>",
                            label, cost, time);
            case Temporary:
                return i18n("<qt>%2 temporary allocations after %3 from:<p "
                            "style='margin-left:10px'>%1</p></qt>",
                            label, cost, time);
            case Consumed:
                return i18n("<qt>%2 consumed after %3 from:<p "
                            "style='margin-left:10px'>%1</p></qt>",
                            label, byteCost(), time);
            }
        }
        return {};
    }

    return cost;
}

int ChartModel::columnCount(const QModelIndex& /*parent*/) const
{
    return qMin(m_maxDatasetCount, m_data.labels.size()) * 2;
}

int ChartModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    } else {
        return m_data.rows.size();
    }
}

void ChartModel::setMaximumDatasetCount(int count)
{
    Q_ASSERT(count >= 0);

    int currentColumns = qMin(m_data.labels.size(), m_maxDatasetCount);
    int newColumnCount = qMin(m_data.labels.size(), count);

    if (newColumnCount == currentColumns) {
        return;
    } else if (newColumnCount < currentColumns) {
        beginRemoveColumns(QModelIndex(), newColumnCount * 2, currentColumns * 2 - 1);
    } else {
        beginInsertColumns(QModelIndex(), currentColumns * 2, newColumnCount * 2 - 1);
    }

    m_maxDatasetCount = count;
    resetColors();

    if (newColumnCount < currentColumns) {
        endRemoveColumns();
    } else {
        endInsertColumns();
    }
    Q_ASSERT(columnCount() == newColumnCount * 2);
}

void ChartModel::resetColors()
{
    m_columnDataSetBrushes.clear();
    m_columnDataSetPens.clear();
    const auto columns = columnCount();
    for (int i = 0; i < columns; ++i) {
        auto color = colorForColumn(i, columns);
        m_columnDataSetBrushes << QBrush(color);
        m_columnDataSetPens << QPen(color);
    }
}

void ChartModel::resetData(const ChartData& data)
{
    Q_ASSERT(data.resultData);
    Q_ASSERT(data.labels.size() < ChartRows::MAX_NUM_COST);
    beginResetModel();
    m_data = data;
    resetColors();
    endResetModel();
}

void ChartModel::clearData()
{
    beginResetModel();
    m_data = {};
    m_columnDataSetBrushes = {};
    m_columnDataSetPens = {};
    endResetModel();
}

struct CompareClosestToTime
{
    bool operator()(const qint64& lhs, const ChartRows& rhs) const
    {
        return lhs > rhs.timeStamp;
    }
    bool operator()(const ChartRows& lhs, const qint64& rhs) const
    {
        return lhs.timeStamp > rhs;
    }
};

qint64 ChartModel::totalCostAt(qint64 timeStamp) const
{
    auto it = std::lower_bound(m_data.rows.rbegin(), m_data.rows.rend(), timeStamp, CompareClosestToTime());
    if (it == m_data.rows.rend())
        return 0;
    return it->cost[0];
}

#include "moc_chartmodel.cpp"
