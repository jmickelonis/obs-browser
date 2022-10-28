#pragma once

#include <QTimer>
#include <QPointer>
#include "browser-panel.hpp"
#include "cef-headers.hpp"

#include <vector>
#include <mutex>

#include <QPainterPath>
#include <QPropertyAnimation>
#include "include/views/cef_window.h"

struct PopupWhitelistInfo {
	std::string url;
	QPointer<QObject> obj;

	inline PopupWhitelistInfo(const std::string &url_, QObject *obj_)
		: url(url_), obj(obj_)
	{
	}
};

extern std::mutex popup_whitelist_mutex;
extern std::vector<PopupWhitelistInfo> popup_whitelist;
extern std::vector<PopupWhitelistInfo> forced_popups;

/* ------------------------------------------------------------------------- */

/* Shows that a browser panel is loading, and covers up any graphical blips
   until the content is completely ready. */
class ProgressWidget : public QWidget
{
	Q_OBJECT
    Q_PROPERTY(qreal angle READ getAngle WRITE setAngle)

public:

	static const int thickness = 10;

	static const int w = 50;
	static const int h = 50;

	ProgressWidget(QWidget *parent = nullptr);
	~ProgressWidget();

	qreal getAngle();
	void setAngle(qreal angle);

	virtual QSize sizeHint() const override;

protected:

	virtual bool event(QEvent *) override;
	virtual void paintEvent(QPaintEvent *) override;

private:

	QPropertyAnimation *animation;
	QConicalGradient gradient;
	QPainterPath path;

};

class QCefWidgetInternal : public QCefWidget {
	Q_OBJECT

public:
	QCefWidgetInternal(QWidget *parent, const std::string &url,
			   CefRefPtr<CefRequestContext> rqc);
	~QCefWidgetInternal();

	CefRefPtr<CefBrowser> cefBrowser;
	std::string url;
	std::string script;
	CefRefPtr<CefRequestContext> rqc;
	QTimer timer;
	bool allowAllPopups_ = false;

	virtual bool event(QEvent *) override;
	virtual void showEvent(QShowEvent *event) override;

	virtual void setURL(const std::string &url) override;
	virtual void setStartupScript(const std::string &script) override;
	virtual void allowAllPopups(bool allow) override;
	virtual void closeBrowser() override;
	virtual void reloadPage() override;

	void onLoadEnd();

public slots:
	void Init();

private:
	volatile bool loading = false;
	QPointer<QWidget> cefContainer;
	QPointer<QWindow> cefWindow;
#ifndef _WIN32
	CefRefPtr<CefWindow> nativeWindow;
#endif
	void removeChildren();
	void showContainer();
	void updateMargins();
};
