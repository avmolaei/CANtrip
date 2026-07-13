// The "CAN Controller" dialog, opened from the ribbon's Hardware tab: picks
// classic vs. CAN FD, bitrate(s), and - for FD - either a target sample
// point (real bit timing computed live via CanBitTiming.h) or raw
// BRP/TSEG1/TSEG2/SJW register values directly.
#pragma once

#include <optional>

#include <QDialog>

#include "../common/AVlabsCanBackend.h"
#include "../common/CanBitTiming.h"

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QSpinBox;
class QStackedWidget;
class QWidget;

namespace cantrip {

class CanControllerDialog : public QDialog {
    Q_OBJECT
public:
    explicit CanControllerDialog(QWidget* parent = nullptr);

    // Pre-populates the dialog from a previously-accepted config, so
    // reopening it shows what's currently configured rather than defaults.
    void setConfig(const CanBitrateConfig& config);
    CanBitrateConfig config() const;

    // "Request bus configuration" checkbox - unchecked means join the
    // channel in listen-only mode (no exclusive configuration rights
    // requested), for coexisting with another app that's already configured
    // the bus. See IAvlabsCanBackend::initialize's requestOwnership
    // parameter, threaded through TsharkCapture::Config::listenOnly.
    bool listenOnly() const;
    void setListenOnly(bool listenOnly);

private slots:
    void onModeChanged();
    void updateIsoPreview();

private:
    enum class Mode { Classic, IsoFd, ExpertFd };
    Mode currentMode() const;

    QWidget* buildIsoPage();
    QWidget* buildExpertPage();

    QComboBox* modeCombo_;
    QComboBox* baudrateCombo_;

    QWidget* fdPanel_;
    QComboBox* dataRateCombo_;
    QStackedWidget* fdStack_;

    QSpinBox* nomSamplePointSpin_;
    QSpinBox* dataSamplePointSpin_;
    QLabel* achievedLabel_;

    QSpinBox* nomBrpSpin_;
    QSpinBox* nomTseg1Spin_;
    QSpinBox* nomTseg2Spin_;
    QSpinBox* nomSjwSpin_;
    QSpinBox* dataBrpSpin_;
    QSpinBox* dataTseg1Spin_;
    QSpinBox* dataTseg2Spin_;
    QSpinBox* dataSjwSpin_;

    QCheckBox* requestOwnershipCheck_;

    QDialogButtonBox* buttonBox_;

    // Cached so switching from ISO to Expert mode pre-fills the raw fields
    // with the last live-computed solution instead of leaving them blank.
    std::optional<CanFdTimingResult> lastFdResult_;
};

} // namespace cantrip
