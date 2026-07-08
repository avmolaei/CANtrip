// The Logging tab's "Other Options..." dialog: filename template editing
// (Tab 1) and logging-specific settings (Tab 2). Same QDialog-with-tabs
// pattern as CanControllerDialog/GraphAxisDialog elsewhere in app/.
#pragma once

#include <QDialog>

class QCheckBox;
class QLabel;
class QLineEdit;
class QRadioButton;
class QSpinBox;

namespace cantrip {

class LoggingOptionsDialog : public QDialog {
    Q_OBJECT
public:
    enum class ExistingFilePolicy { Overwrite, AutoIncrement, Prompt };

    explicit LoggingOptionsDialog(QWidget* parent = nullptr);

    void setFilenameTemplate(const QString& tmpl);
    QString filenameTemplate() const;

    // Needed only to expand [bus] correctly in the live preview - not part
    // of the saved settings themselves.
    void setPreviewBusName(const QString& busName);

    void setMaxFileSizeMb(int mb); // 0 = unlimited
    int maxFileSizeMb() const;

    void setExistingFilePolicy(ExistingFilePolicy policy);
    ExistingFilePolicy existingFilePolicy() const;

    void setAutoStartWithCapture(bool enabled);
    bool autoStartWithCapture() const;

private slots:
    void insertToken(const QString& token);
    void updatePreview();

private:
    QWidget* buildTemplateTab();
    QWidget* buildOptionsTab();

    QLineEdit* templateEdit_;
    QLabel* previewLabel_;
    QString previewBusName_;

    QSpinBox* maxFileSizeSpin_;
    QRadioButton* overwriteRadio_;
    QRadioButton* autoIncrementRadio_;
    QRadioButton* promptRadio_;
    QCheckBox* autoStartCheck_;
};

} // namespace cantrip
