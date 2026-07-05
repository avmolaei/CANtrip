#pragma once

#include <QTimer>
#include <QWidget>

namespace cantrip {

// A small round status-bar indicator: solid red when idle, blinking
// green/gray while a capture is running.
class StatusLed : public QWidget {
    Q_OBJECT
public:
    explicit StatusLed(QWidget* parent = nullptr);
    QSize sizeHint() const override;

public slots:
    void setCapturing(bool capturing);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    bool capturing_ = false;
    bool blinkOn_ = false;
    QTimer blinkTimer_;
};

} // namespace cantrip
