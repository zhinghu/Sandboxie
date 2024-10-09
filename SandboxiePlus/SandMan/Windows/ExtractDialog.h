#pragma once

#include <QtWidgets/QMainWindow>
#include "ui_ExtractDialog.h"
#include "SbiePlusAPI.h"

class CExtractDialog : public QDialog
{
	Q_OBJECT

public:
	CExtractDialog(const QString& Name, QWidget *parent = Q_NULLPTR);
	~CExtractDialog();

	QString GetName() const { return ui.txtName->text(); }
	QString GetRoot() const;
	void ShowNoCrypt() const { ui.chkNoCrypt->setVisible(true); }
	bool IsNoCrypt() const { return ui.chkNoCrypt->isChecked(); }

private slots:
	void OnAccept();

private:
	Ui::ExtractDialog ui;
};
