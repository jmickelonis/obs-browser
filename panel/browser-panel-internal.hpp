#pragma once

#include <QConicalGradient>
#include <QTimer>
#include <QPainterPath>
#include <QPointer>
#include <QPropertyAnimation>
#include "browser-panel.hpp"
#include "cef-headers.hpp"
#include "include/views/cef_window.h"

#include <vector>
#include <mutex>

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

	virtual void resizeEvent(QResizeEvent *event) override;
	virtual void showEvent(QShowEvent *event) override;
	// virtual QPaintEngine *paintEngine() const override;
	virtual bool event(QEvent *) override;

	virtual void setURL(const std::string &url) override;
	virtual void setStartupScript(const std::string &script) override;
	virtual void allowAllPopups(bool allow) override;
	virtual void closeBrowser() override;
	virtual void reloadPage() override;

	void Resize();
	void onLoadEnd();

private:
	volatile bool loading = false;
	QPointer<QWindow> cefWindow;
	QPointer<QWidget> cefContainer;
#ifndef _WIN32
	CefRefPtr<CefWindow> nativeWindow;
#endif
	void removeChildren();
	void showContainer();
	void updateMargins();
#ifdef __linux__
	bool needsDeleteXdndProxy = true;
	void unsetToplevelXdndProxy();
#endif

public slots:
	void Init();
};
