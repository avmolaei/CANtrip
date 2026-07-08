#include "LoggingOptionsDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

#include "LogFilenameTemplate.h"

namespace cantrip {

LoggingOptionsDialog::LoggingOptionsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Logging Options");

    auto* root = new QVBoxLayout(this);
    auto* tabs = new QTabWidget(this);
    tabs->addTab(buildTemplateTab(), "Filename Template");
    tabs->addTab(buildOptionsTab(), "Logging Options");
    root->addWidget(tabs);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    updatePreview();
}

QWidget* LoggingOptionsDialog::buildTemplateTab() {
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    layout->addWidget(new QLabel("Filename template (extension is added automatically):", page));

    templateEdit_ = new QLineEdit("[user]_[bus]_[date]_[time]", page);
    layout->addWidget(templateEdit_);

    auto* tokenRow = new QHBoxLayout();
    for (const QString& token : {"[date]", "[time]", "[user]", "[bus]"}) {
        auto* btn = new QPushButton(token, page);
        connect(btn, &QPushButton::clicked, this, [this, token]() { insertToken(token); });
        tokenRow->addWidget(btn);
    }
    tokenRow->addStretch(1);
    layout->addLayout(tokenRow);

    layout->addWidget(new QLabel("Preview:", page));
    previewLabel_ = new QLabel(page);
    previewLabel_->setStyleSheet("font-family: monospace;");
    layout->addWidget(previewLabel_);
    layout->addStretch(1);

    connect(templateEdit_, &QLineEdit::textChanged, this, &LoggingOptionsDialog::updatePreview);

    return page;
}

QWidget* LoggingOptionsDialog::buildOptionsTab() {
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);

    maxFileSizeSpin_ = new QSpinBox(page);
    maxFileSizeSpin_->setRange(0, 100000);
    maxFileSizeSpin_->setSuffix(" MB");
    maxFileSizeSpin_->setSpecialValueText("Unlimited");
    form->addRow("Max file size before splitting:", maxFileSizeSpin_);

    overwriteRadio_ = new QRadioButton("Overwrite", page);
    autoIncrementRadio_ = new QRadioButton("Auto-increment suffix", page);
    promptRadio_ = new QRadioButton("Prompt", page);
    autoIncrementRadio_->setChecked(true);
    auto* existingLayout = new QVBoxLayout();
    existingLayout->addWidget(overwriteRadio_);
    existingLayout->addWidget(autoIncrementRadio_);
    existingLayout->addWidget(promptRadio_);
    form->addRow("If output file already exists:", existingLayout);

    autoStartCheck_ = new QCheckBox("Automatically start logging when Start Capture is pressed", page);
    form->addRow(autoStartCheck_);

    return page;
}

void LoggingOptionsDialog::insertToken(const QString& token) {
    templateEdit_->insert(token);
}

void LoggingOptionsDialog::updatePreview() {
    previewLabel_->setText(expandLogFilenameTemplate(templateEdit_->text(), previewBusName_) + ".asc");
}

void LoggingOptionsDialog::setFilenameTemplate(const QString& tmpl) {
    templateEdit_->setText(tmpl);
}

QString LoggingOptionsDialog::filenameTemplate() const {
    return templateEdit_->text();
}

void LoggingOptionsDialog::setPreviewBusName(const QString& busName) {
    previewBusName_ = busName;
    updatePreview();
}

void LoggingOptionsDialog::setMaxFileSizeMb(int mb) { maxFileSizeSpin_->setValue(mb); }
int LoggingOptionsDialog::maxFileSizeMb() const { return maxFileSizeSpin_->value(); }

void LoggingOptionsDialog::setExistingFilePolicy(ExistingFilePolicy policy) {
    switch (policy) {
        case ExistingFilePolicy::Overwrite: overwriteRadio_->setChecked(true); break;
        case ExistingFilePolicy::AutoIncrement: autoIncrementRadio_->setChecked(true); break;
        case ExistingFilePolicy::Prompt: promptRadio_->setChecked(true); break;
    }
}

LoggingOptionsDialog::ExistingFilePolicy LoggingOptionsDialog::existingFilePolicy() const {
    if (overwriteRadio_->isChecked()) return ExistingFilePolicy::Overwrite;
    if (promptRadio_->isChecked()) return ExistingFilePolicy::Prompt;
    return ExistingFilePolicy::AutoIncrement;
}

void LoggingOptionsDialog::setAutoStartWithCapture(bool enabled) { autoStartCheck_->setChecked(enabled); }
bool LoggingOptionsDialog::autoStartWithCapture() const { return autoStartCheck_->isChecked(); }

} // namespace cantrip
