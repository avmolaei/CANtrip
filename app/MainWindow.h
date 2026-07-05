#pragma once

#include <memory>
#include <vector>

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPushButton>
#include <QTreeWidget>

#include <dbcppp/Network.h>

#include "TsharkCapture.h"

namespace cantrip {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void refreshChannels();
    void importDbc();
    void toggleCapture();
    void onFrameReceived(const DecodedCanFrame& frame);
    void onCaptureError(const QString& message);
    void onCaptureStopped();

private:
    struct ChannelEntry {
        QString interfaceId;
        QString displayName;
    };

    void updateFdControlsEnabled();
    static QString findTsharkExe();

    QComboBox* channelCombo_;
    QPushButton* refreshButton_;
    QComboBox* bitrateCombo_;
    QCheckBox* fdCheckBox_;
    QComboBox* dataBitrateCombo_;
    QLineEdit* expertStringEdit_;
    QPushButton* importDbcButton_;
    QLabel* dbcStatusLabel_;
    QPushButton* startStopButton_;
    QTreeWidget* frameTree_;

    std::vector<ChannelEntry> channels_;
    std::unique_ptr<dbcppp::INetwork> dbcNetwork_;
    TsharkCapture capture_;
    int frameCount_ = 0;
};

} // namespace cantrip
