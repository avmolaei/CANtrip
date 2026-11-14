#include <QtTest>
#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QValueAxis>
#include <QtCharts/QXYSeries>

#include <cmath>

#include "GraphView.h"
#include "SignalHistory.h"

using namespace cantrip;

// Drives a real GraphView against a real store, on the offscreen platform.
class GraphViewTest : public QObject {
    Q_OBJECT

    struct Rig {
        SignalHistoryStore store;
        GraphView view{&store};

        Rig() {
            view.resize(1200, 600);
            view.show();
            (void)QTest::qWaitForWindowExposed(&view);
        }
        QChart* chart() const { return view.findChild<QChartView*>()->chart(); }
        QXYSeries* series(int i = 0) const { return qobject_cast<QXYSeries*>(chart()->series().at(i)); }
        QValueAxis* timeAxis() const { return qobject_cast<QValueAxis*>(chart()->axes(Qt::Horizontal).first()); }
        QValueAxis* valueAxis() const { return qobject_cast<QValueAxis*>(chart()->axes(Qt::Vertical).first()); }

        void plot(const QStringList& names) {
            GraphView::AxisLayout axis;
            for (const QString& name : names) axis.plottedSignals.push_back({name, QColor(Qt::red), GraphLineStyle::Solid});
            view.importLayout({axis});
        }
        void record(const QString& name, int from, int to) {
            for (int i = from; i < to; ++i) store.recordSample(name, "u", i * 0.001, std::sin(i * 0.001) * 50.0);
        }
        void wheelZoomIn(int steps) {
            QWidget* viewport = view.findChild<QChartView*>()->viewport();
            const QPointF pos = viewport->rect().center();
            for (int i = 0; i < steps; ++i) {
                QWheelEvent wheel(pos, viewport->mapToGlobal(pos), QPoint(), QPoint(0, 120), Qt::NoButton,
                                  Qt::NoModifier, Qt::NoScrollPhase, false);
                QApplication::sendEvent(viewport, &wheel);
            }
        }
    };

private slots:
    void aLongCaptureIsDrawnWithFewPoints() {
        Rig r;
        r.record("M.a", 0, 300000);
        r.plot({"M.a"}); // plotted after the data, with nothing new arriving
        QTRY_VERIFY(r.series()->count() > 0);

        // At most two points per pixel column. Waited for, because the plot
        // area settles over a couple of refreshes as axis labels take shape.
        auto plotWidth = [&r]() { return static_cast<int>(r.chart()->plotArea().width()); };
        QVERIFY(plotWidth() > 200);
        QTRY_VERIFY2(r.series()->count() <= 2 * plotWidth(), qPrintable(QString("%1 points, plot %2 px").arg(r.series()->count()).arg(r.chart()->plotArea().width())));
        QVERIFY(r.series()->count() > plotWidth() / 2);

        // The whole capture is on screen, at its full height.
        QTRY_COMPARE(r.timeAxis()->max(), 299999 * 0.001);
        QCOMPARE(r.timeAxis()->min(), 0.0);
        const SignalSeries& samples = *r.store.series("M.a");
        QCOMPARE(r.valueAxis()->min(), samples.valueMin());
        QCOMPARE(r.valueAxis()->max(), samples.valueMax());
        QCOMPARE(r.series()->at(r.series()->count() - 1).x(), 299999 * 0.001);
    }

    void newSamplesShowUpWithoutBeingAskedFor() {
        Rig r;
        r.plot({"M.a"});
        r.record("M.a", 0, 1000);
        QTRY_COMPARE(r.series()->count(), 1000);
        r.record("M.a", 1000, 1500);
        QTRY_COMPARE(r.series()->count(), 1500);
        QCOMPARE(r.series()->at(1499), QPointF(1499 * 0.001, std::sin(1499 * 0.001) * 50.0));
        QTRY_COMPARE(r.timeAxis()->max(), 1499 * 0.001);
    }

    void zoomingInShowsTheRawSamples() {
        Rig r;
        r.record("M.a", 0, 300000);
        r.plot({"M.a"});
        QTRY_VERIFY(r.series()->count() > 0);

        r.wheelZoomIn(80); // 0.9^80: from 300 s down to well under a second
        const double span = r.timeAxis()->max() - r.timeAxis()->min();
        QVERIFY2(span < 0.2, qPrintable(QString::number(span)));

        const SignalSeries& samples = *r.store.series("M.a");
        const size_t first = samples.lowerBound(r.timeAxis()->min());
        const size_t last = samples.lowerBound(r.timeAxis()->max());
        QTRY_VERIFY(r.series()->count() <= static_cast<int>(last - first) + 3);
        QVERIFY(r.series()->count() >= static_cast<int>(last - first));

        // Every drawn point is a real sample, and none in view is skipped.
        const QList<QPointF> points = r.series()->points();
        const size_t start = samples.lowerBound(points.first().x());
        for (qsizetype i = 0; i < points.size(); ++i) {
            QCOMPARE(points[i], QPointF(samples.at(start + i).timeSec, samples.at(start + i).value));
        }

        // Still zoomed while new data arrives: the view must not jump back.
        const double heldMax = r.timeAxis()->max();
        r.record("M.a", 300000, 301000);
        QTest::qWait(150);
        QCOMPARE(r.timeAxis()->max(), heldMax);
    }

    void aLongViewStillPicksUpNewSamples() {
        // Redraws for new samples are spaced out when a lot of time is on
        // screen, but they must still happen.
        Rig r;
        r.record("M.a", 0, 3600000); // an hour at 1 kHz
        r.plot({"M.a"});
        QTRY_VERIFY(r.series()->count() > 0);
        QTRY_COMPARE(r.series()->at(r.series()->count() - 1).x(), 3599999 * 0.001);
        r.record("M.a", 3600000, 3600100);
        QTRY_COMPARE(r.series()->at(r.series()->count() - 1).x(), 3600099 * 0.001);
        QTRY_COMPARE(r.timeAxis()->max(), 3600099 * 0.001);
    }

    void clearingStartsOver() {
        Rig r;
        r.plot({"M.a", "M.b"});
        r.record("M.a", 0, 5000);
        r.record("M.b", 0, 5000);
        QTRY_VERIFY(r.series(0)->count() > 0 && r.series(1)->count() > 0);

        r.store.reset();
        r.view.clearData();
        QCOMPARE(r.series(0)->count(), 0);
        QTest::qWait(150);
        QCOMPARE(r.series(0)->count(), 0);

        // The same number of samples as before the clear, with other values:
        // must be redrawn, not mistaken for what was already drawn.
        for (int i = 0; i < 5000; ++i) r.store.recordSample("M.a", "u", i * 0.001, 7.0);
        QTRY_COMPARE(r.series(0)->count() > 0, true);
        QCOMPARE(r.series(0)->at(0).y(), 7.0);
        QCOMPARE(r.series(1)->count(), 0);
    }

    void aHiddenWindowDoesNoWorkAndCatchesUp() {
        Rig r;
        r.plot({"M.a"});
        r.view.hide();
        r.record("M.a", 0, 2000);
        QTest::qWait(150);
        QCOMPARE(r.series()->count(), 0);
        r.view.show();
        QTRY_VERIFY(r.series()->count() > 0);
    }
};

QTEST_MAIN(GraphViewTest)
#include "tst_graph_view.moc"
