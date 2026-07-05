#include "StatusLed.h"

#include <algorithm>

#include <QColor>
#include <QPainter>

namespace cantrip {

StatusLed::StatusLed(QWidget* parent) : QWidget(parent) {
    setToolTip("Capture status");
    blinkTimer_.setInterval(500);
    connect(&blinkTimer_, &QTimer::timeout, this, [this] {
        blinkOn_ = !blinkOn_;
        update();
    });
}

QSize StatusLed::sizeHint() const {
    return QSize(16, 16);
}

void StatusLed::setCapturing(bool capturing) {
    capturing_ = capturing;
    if (capturing_) {
        blinkOn_ = true;
        blinkTimer_.start();
    } else {
        blinkTimer_.stop();
    }
    update();
}

void StatusLed::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    QColor color;
    if (!capturing_) {
        color = QColor(200, 40, 40); // idle: solid red
    } else {
        color = blinkOn_ ? QColor(50, 200, 60) : QColor(110, 110, 110); // blinking green/gray
    }

    const int diameter = std::min(width(), height()) - 4;
    const QRect circleRect((width() - diameter) / 2, (height() - diameter) / 2, diameter, diameter);
    painter.setBrush(color);
    painter.setPen(QColor(25, 25, 25));
    painter.drawEllipse(circleRect);
}

} // namespace cantrip
