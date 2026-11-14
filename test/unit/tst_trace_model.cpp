#include <QtTest>
#include <QLineEdit>
#include <QTreeView>

#include "DbcDecoder.h"
#include "TestFrames.h"
#include "TraceHeaderView.h"
#include "TraceModel.h"

using namespace cantrip;
using namespace cantrip::testing;

// The Trace view's model and header, wired together the way MainWindow
// wires them, and driven with real mouse and key events.
class TraceModelTest : public QObject {
    Q_OBJECT

    struct Rig {
        DbcDecoder decoder;
        TraceModel model{&decoder};
        QTreeView view;
        TraceHeaderView* header;

        explicit Rig(bool periodic = false) {
            QString error;
            if (!decoder.loadFile(sampleDbcPath(), &error)) qFatal("%s", qPrintable(error));
            model.decoderChanged();
            header = new TraceHeaderView(&view);
            view.setHeader(header);
            view.setModel(&model);
            header->applyBehavior();
            view.setUniformRowHeights(true);
            connect(header, &TraceHeaderView::sortRequested, &model, &TraceModel::sort);
            connect(header, &TraceHeaderView::filterEdited, &model,
                    [this](int column, const QString& text) { model.setColumnFilter(column, text); });
            if (periodic) sortBy(TraceModel::Id, Qt::AscendingOrder);
            else sortBy(TraceModel::Time, Qt::DescendingOrder);
            view.resize(1280, 640);
            view.show();
            (void)QTest::qWaitForWindowExposed(&view);
        }

        void sortBy(int column, Qt::SortOrder order) {
            header->setSortIndicator(column, order);
            model.sort(column, order);
        }
        QString cell(int row, int column) const { return model.index(row, column).data().toString(); }
        double time(int row) const { return cell(row, TraceModel::Time).toDouble(); }
        QStringList column(int column) const {
            QStringList out;
            for (int row = 0; row < model.rowCount(); ++row) out << cell(row, column).toUpper();
            return out;
        }
        QLineEdit* editor() const { return header->viewport()->findChild<QLineEdit*>(); }
        void clickName(int column) {
            QTest::mouseClick(header->viewport(), Qt::LeftButton, {},
                              QPoint(header->sectionViewportPosition(column) + 12, header->height() / 2));
        }
        void clickArrow(int column) {
            const int right = header->sectionViewportPosition(column) + header->sectionSize(column);
            QTest::mouseClick(header->viewport(), Qt::LeftButton, {}, QPoint(right - 9, header->height() / 2));
        }
    };

    static std::vector<DecodedCanFrame> mixedFrames() {
        DecodedCanFrame ext = frame(0x18DAF110, 5);
        ext.extended = true;
        return {frame(0x100, 1), frame(0x7E8, 2), frame(0x170, 3), frame(0x200, 4, QByteArray::fromHex("A0A1A2A3")),
                ext, errorFrame(6, "Stuff Error")};
    }

private slots:
    void waterfallIsNewestFirstPastTenSeconds() {
        // Time used to sort as text, which put 10.0 below 9.5.
        Rig r;
        std::vector<DecodedCanFrame> frames;
        for (double t = 8.0; t <= 12.01; t += 0.5) frames.push_back(frame(0x100, t));
        r.model.appendFrames(frames);
        QCOMPARE(r.time(0), 12.0);
        QCOMPARE(r.time(1), 11.5);
        QCOMPARE(r.time(r.model.rowCount() - 1), 8.0);
    }

    void filtersMatchByContaining_data() {
        QTest::addColumn<int>("column");
        QTest::addColumn<QString>("text");
        QTest::addColumn<int>("rows");
        QTest::newRow("ID prefix") << int(TraceModel::Id) << "0x7" << 1;
        QTest::newRow("ID digit anywhere") << int(TraceModel::Id) << "7" << 2;
        QTest::newRow("ID ignores case") << int(TraceModel::Id) << "7e8" << 1;
        QTest::newRow("ID hides bus errors") << int(TraceModel::Id) << "0x1" << 3;
        QTest::newRow("Data without spaces") << int(TraceModel::Data) << "a0a1" << 1;
        QTest::newRow("Data with spaces") << int(TraceModel::Data) << "A0 A1" << 1;
        QTest::newRow("Message name") << int(TraceModel::Message) << "engine" << 1;
        QTest::newRow("Signal name keeps its message") << int(TraceModel::Message) << "EngineTemp" << 1;
        QTest::newRow("Bus errors by name") << int(TraceModel::Message) << "BUS ERROR" << 1;
        QTest::newRow("Decoded value") << int(TraceModel::Value) << "degC" << 1;
        QTest::newRow("Flags") << int(TraceModel::Flags) << "EXT" << 1;
        QTest::newRow("Nothing matches") << int(TraceModel::Id) << "0xFFF" << 0;
        QTest::newRow("Only spaces is no filter") << int(TraceModel::Id) << "   " << 6;
    }
    void filtersMatchByContaining() {
        QFETCH(int, column);
        QFETCH(QString, text);
        QFETCH(int, rows);
        Rig r;
        r.model.appendFrames(mixedFrames());
        QCOMPARE(r.model.rowCount(), 6);
        r.model.setColumnFilter(column, text);
        QCOMPARE(r.model.rowCount(), rows);
        QCOMPARE(r.model.totalRows(), 6);
        r.model.setColumnFilter(column, QString());
        QCOMPARE(r.model.rowCount(), 6);
        QVERIFY(!r.model.hasFilter());
    }

    void filtersCombineAndApplyToLiveFrames() {
        Rig r;
        r.model.appendFrames(mixedFrames());
        r.model.setColumnFilter(TraceModel::Id, "0x2");
        r.model.setColumnFilter(TraceModel::Data, "A0");
        QCOMPARE(r.model.rowCount(), 1);
        r.model.setColumnFilter(TraceModel::Data, "FF");
        QCOMPARE(r.model.rowCount(), 0);
        r.model.setColumnFilter(TraceModel::Data, QString());

        r.model.appendFrames({frame(0x200, 7), frame(0x300, 8), frame(0x2FF, 9)});
        QCOMPARE(r.model.rowCount(), 3);
        QCOMPARE(r.time(0), 9.0);
        QCOMPARE(r.model.totalRows(), 9);
        QCOMPARE(r.model.headerData(TraceModel::Id, Qt::Horizontal).toString(), QString("ID: 0x2"));
        QCOMPARE(r.model.headerData(TraceModel::Id, Qt::Horizontal, Qt::UserRole).toString(), QString("0x2"));
    }

    void expandedRowsShowDecodedSignals() {
        Rig r;
        r.model.appendFrames(mixedFrames());
        const QModelIndex engine = r.model.index(5, 0); // oldest row, 0x100
        QCOMPARE(r.model.rowCount(engine), 2);
        QCOMPARE(r.model.index(0, TraceModel::Message, engine).data().toString(), QString("EngineSpeed"));
        QCOMPARE(r.model.index(0, TraceModel::Value, engine).data().toString(), QString("1418 rpm"));
        QCOMPARE(r.model.index(1, TraceModel::Value, engine).data().toString(), QString("90 degC"));
        QCOMPARE(r.model.index(0, 0, engine).parent(), engine);
        QCOMPARE(r.model.rowCount(r.model.index(0, 0)), 0); // a bus error has none
    }

    void liveRowsLandInSortedPlace() {
        Rig r;
        r.sortBy(TraceModel::Id, Qt::AscendingOrder);
        r.model.appendFrames({frame(0x300, 1), frame(0x100, 2), frame(0x7E8, 3)});
        r.model.appendFrames({frame(0x200, 4), frame(0x050, 5)});
        QCOMPARE(r.column(TraceModel::Id), (QStringList{"0X050", "0X100", "0X200", "0X300", "0X7E8"}));
        r.sortBy(TraceModel::Id, Qt::DescendingOrder);
        QCOMPARE(r.column(TraceModel::Id).first(), QString("0X7E8"));
    }

    void expansionSurvivesFilterAndSortChanges() {
        Rig r;
        r.model.appendFrames({frame(0x100, 1), frame(0x200, 2), frame(0x300, 3)});
        QPersistentModelIndex engine(r.model.index(2, 0));
        r.view.expand(engine);

        r.sortBy(TraceModel::Id, Qt::AscendingOrder);
        QVERIFY(engine.isValid());
        QCOMPARE(engine.row(), 0);
        QVERIFY(r.view.isExpanded(engine));

        r.model.setColumnFilter(TraceModel::Id, "0x1");
        QCOMPARE(r.model.rowCount(), 1);
        QVERIFY(r.view.isExpanded(r.model.index(0, 0)));

        r.model.setColumnFilter(TraceModel::Id, "0x2");
        QVERIFY(!engine.isValid());
    }

    void childrenFindTheirParentAmongEqualSortKeys() {
        Rig r;
        r.sortBy(TraceModel::Dlc, Qt::AscendingOrder);
        std::vector<DecodedCanFrame> same;
        for (int i = 0; i < 50; ++i) same.push_back(frame(0x100, i));
        r.model.appendFrames(same);
        const QModelIndex parent = r.model.index(25, 0);
        const QModelIndex child = r.model.index(1, TraceModel::Message, parent);
        QCOMPARE(child.parent(), parent);
        QCOMPARE(child.data().toString(), QString("EngineTemp"));
    }

    void waterfallKeepsOnlyTheNewestRows() {
        Rig r;
        std::vector<DecodedCanFrame> frames;
        for (int i = 0; i < TraceModel::kMaxRows; ++i) frames.push_back(frame(0x100 + i % 40, i * 0.001));
        r.model.appendFrames(frames);
        QPersistentModelIndex expanded(r.model.index(10, 0));
        r.view.expand(expanded);

        frames.clear();
        for (int i = 0; i < 500; ++i) frames.push_back(frame(0x100 + i % 40, 200 + i * 0.001));
        r.model.appendFrames(frames);
        QCOMPARE(r.model.rowCount(), int(TraceModel::kMaxRows));
        QCOMPARE(r.time(0), 200.499);
        QCOMPARE(r.time(r.model.rowCount() - 1), 0.5);

        // Row numbers shifted underneath it, the expanded row is still itself.
        QVERIFY(expanded.isValid());
        QCOMPARE(expanded.row(), 510);

        // A filter hiding old rows must not stop them from being dropped.
        r.model.setColumnFilter(TraceModel::Id, "0x105");
        const int shown = r.model.rowCount();
        frames.clear();
        for (int i = 0; i < 2000; ++i) frames.push_back(frame(0x100 + i % 40, 900 + i * 0.001));
        r.model.appendFrames(frames);
        QCOMPARE(r.model.rowCount(), shown);
        QCOMPARE(r.model.totalRows(), int(TraceModel::kMaxRows));

        r.model.clear();
        QCOMPARE(r.model.rowCount(), 0);
        QCOMPARE(r.model.totalRows(), 0);
    }

    void periodicKeepsOneRowPerIdAndMeasuresThePeriod() {
        Rig r(/*periodic=*/true);
        r.model.upsertPeriodic(frame(0x200, 1), 1000);
        r.model.upsertPeriodic(frame(0x100, 1), 1000);
        r.model.upsertPeriodic(errorFrame(1, "Form Error"), 1000);
        r.model.upsertPeriodic(frame(0x100, 2, QByteArray::fromHex("A000820000000000")), 1250);
        r.model.upsertPeriodic(errorFrame(2, "Form Error"), 1400);
        r.model.endPeriodicBatch();

        QCOMPARE(r.model.rowCount(), 3);
        const int row = static_cast<int>(r.column(TraceModel::Id).indexOf("0X100"));
        QVERIFY(row >= 0);
        QCOMPARE(r.cell(row, TraceModel::Time), QString("0.250 s"));
        QVERIFY(r.cell(row, TraceModel::Data).startsWith("A0"));
        QCOMPARE(r.cell(r.column(TraceModel::Id).indexOf("0X200"), TraceModel::Time), QString("--"));

        // An expanded row's signals follow the new data.
        const QModelIndex parent = r.model.index(row, 0);
        r.view.expand(parent);
        const QString before = r.model.index(0, TraceModel::Value, parent).data().toString();
        r.model.upsertPeriodic(frame(0x100, 3, QByteArray::fromHex("1000820000000000")), 1500);
        r.model.endPeriodicBatch();
        QVERIFY(r.model.index(0, TraceModel::Value, r.model.index(row, 0)).data().toString() != before);
    }

    void periodicRowsGoStaleAndComeBack() {
        Rig r(/*periodic=*/true);
        r.model.upsertPeriodic(frame(0x100, 1), 1000);
        r.model.endPeriodicBatch();
        QVERIFY(r.model.index(0, 1).data(Qt::ForegroundRole).isNull());
        r.model.markStaleRows(2500, 2000);
        QVERIFY(r.model.index(0, 1).data(Qt::ForegroundRole).isNull());
        r.model.markStaleRows(9000, 2000);
        QCOMPARE(r.model.index(0, 1).data(Qt::ForegroundRole).value<QBrush>().color(), QColor(Qt::gray));
        r.model.upsertPeriodic(frame(0x100, 4), 9100);
        r.model.endPeriodicBatch();
        QVERIFY(r.model.index(0, 1).data(Qt::ForegroundRole).isNull());
    }

    void periodicFollowsFiltersAndAChangingSortKey() {
        Rig r(/*periodic=*/true);
        r.sortBy(TraceModel::Time, Qt::DescendingOrder);
        for (uint32_t id : {0x100u, 0x200u, 0x300u}) r.model.upsertPeriodic(frame(id, 0), 1000);
        r.model.endPeriodicBatch();
        r.model.upsertPeriodic(frame(0x100, 1), 1010);
        r.model.upsertPeriodic(frame(0x200, 1), 1300);
        r.model.upsertPeriodic(frame(0x300, 1), 1100);
        r.model.endPeriodicBatch();
        QCOMPARE(r.column(TraceModel::Id), (QStringList{"0X200", "0X300", "0X100"}));

        r.model.setColumnFilter(TraceModel::Data, "AB");
        QCOMPARE(r.model.rowCount(), 0);
        r.model.upsertPeriodic(frame(0x200, 2, QByteArray::fromHex("ABAB")), 1400);
        r.model.endPeriodicBatch();
        QCOMPARE(r.column(TraceModel::Id), QStringList{"0X200"});
        r.model.upsertPeriodic(frame(0x200, 3), 1500);
        r.model.endPeriodicBatch();
        QCOMPARE(r.model.rowCount(), 0);
        r.model.setColumnFilter(TraceModel::Data, QString());
        QCOMPARE(r.model.rowCount(), 3);
    }

    void loadingADbcRenamesExistingRows() {
        DbcDecoder decoder;
        TraceModel model(&decoder);
        model.appendFrames({frame(0x100, 1)});
        QVERIFY(model.index(0, TraceModel::Message).data().toString().isEmpty());
        QCOMPARE(model.rowCount(model.index(0, 0)), 0);
        QVERIFY(decoder.loadFile(sampleDbcPath(), nullptr));
        model.decoderChanged();
        QCOMPARE(model.index(0, TraceModel::Message).data().toString(), QString("EngineData"));
        QCOMPARE(model.rowCount(model.index(0, 0)), 2);
    }

    void clickingAColumnNameFiltersIt() {
        Rig r;
        r.model.appendFrames(mixedFrames());
        QVERIFY(!r.editor()->isVisible());

        r.clickName(TraceModel::Id);
        QVERIFY(r.editor()->isVisible());
        QCOMPARE(r.header->sortIndicatorSection(), int(TraceModel::Time)); // a name click never sorts
        QTest::keyClicks(r.editor(), "0x7");
        QTRY_COMPARE(r.model.rowCount(), 1); // live, after the typing pause

        QTest::keyClick(r.editor(), Qt::Key_Return);
        QVERIFY(!r.editor()->isVisible());
        QCOMPARE(r.model.rowCount(), 1);

        r.clickName(TraceModel::Id);
        QCOMPARE(r.editor()->text(), QString("0x7"));
        QTest::keyClick(r.editor(), Qt::Key_Escape);
        QVERIFY(!r.editor()->isVisible());
        QCOMPARE(r.model.rowCount(), 6);
        QVERIFY(!r.model.hasFilter());
    }

    void theFilterBoxStaysUsableOnNarrowColumnsAndClosesOnFocusLoss() {
        Rig r;
        r.view.setColumnWidth(TraceModel::Dlc, 40);
        r.clickName(TraceModel::Dlc);
        QVERIFY(r.editor()->isVisible());
        QVERIFY(r.editor()->width() >= 140);
        r.editor()->clearFocus();
        QVERIFY(!r.editor()->isVisible());
    }

    void clickingTheArrowSorts() {
        Rig r;
        r.model.appendFrames(mixedFrames());
        r.clickArrow(TraceModel::Id);
        QVERIFY(!r.editor()->isVisible());
        QCOMPARE(r.header->sortIndicatorSection(), int(TraceModel::Id));
        QCOMPARE(r.header->sortIndicatorOrder(), Qt::AscendingOrder);
        // The bus error has no ID and sorts as 0, the extended ID sorts last.
        QCOMPARE(r.column(TraceModel::Id), (QStringList{"", "0X100", "0X170", "0X200", "0X7E8", "0X18DAF110"}));

        r.clickArrow(TraceModel::Id);
        QCOMPARE(r.header->sortIndicatorOrder(), Qt::DescendingOrder);
        QCOMPARE(r.column(TraceModel::Id).first(), QString("0X18DAF110"));

        r.clickArrow(TraceModel::Time);
        QCOMPARE(r.header->sortIndicatorOrder(), Qt::DescendingOrder); // Time starts newest first
        QCOMPARE(r.time(0), 6.0);
    }

    void savedHeaderStateKeepsTheHeaderBehaving() {
        Rig r;
        r.header->moveSection(2, 4);
        r.sortBy(TraceModel::Id, Qt::AscendingOrder);
        const QByteArray state = r.header->saveState();

        Rig restored;
        restored.header->restoreState(state);
        restored.header->applyBehavior();
        QCOMPARE(restored.header->visualIndex(2), 4);
        QCOMPARE(restored.header->sortIndicatorSection(), int(TraceModel::Id));
        QVERIFY(restored.header->sectionsClickable());
        QVERIFY(restored.header->sectionsMovable());
        QVERIFY(!restored.header->isSortIndicatorShown());
    }
};

QTEST_MAIN(TraceModelTest)
#include "tst_trace_model.moc"
