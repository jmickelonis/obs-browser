#pragma once

#include <QTimer>
#include <QPointer>
#include "browser-panel.hpp"
#include "cef-headers.hpp"

#include <vector>
#include <mutex>
#include <condition_variable>

#include <QPainterPath>
#include <QPropertyAnimation>
#include <QResizeEvent>
#include "include/views/cef_window.h"

struct PopupWhitelistInfo {
	std::string url;
	QPointer<QObject> obj;

	inline PopupWhitelistInfo(const std::string &url_, QObject *obj_) : url(url_), obj(obj_) {}
};

extern std::mutex popup_whitelist_mutex;
extern std::vector<PopupWhitelistInfo> popup_whitelist;
extern std::vector<PopupWhitelistInfo> forced_popups;

/* ------------------------------------------------------------------------- */

/* Shows that a browser panel is loading, and covers up any graphical blips
   until the content is completely ready. */
class ProgressWidget : public QWidget {
	Q_OBJECT
	Q_PROPERTY(qreal angle READ getAngle WRITE setAngle)

public:
	static const int w = 50;
	static const int h = 50;
	static const int thickness = 5;

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

/* Whether to use the newer Views API, or the older way.
   Still trying to work around focus issues on Windows... */
#define _CEF_USE_VIEWS 1

class QCefWidgetInternal : public QCefWidget {
	Q_OBJECT
	friend class QCefBrowserClient;

public:
	QCefWidgetInternal(QWidget *parent, const std::string &url, CefRefPtr<CefRequestContext> rqc);
	~QCefWidgetInternal();

	CefRefPtr<CefBrowser> cefBrowser;
#ifdef _CEF_USE_VIEWS
	CefRefPtr<CefWindow> cefWindow;
#endif
	std::string url;
	std::string script;
	CefRefPtr<CefRequestContext> rqc;
	QPointer<QWindow> window;
	QPointer<QWidget> container;
	bool allowAllPopups_ = false;

	virtual bool event(QEvent *) override;
	virtual void showEvent(QShowEvent *event) override;
	virtual void resizeEvent(QResizeEvent *event) override;

	virtual void setURL(const std::string &url) override;
	virtual void setStartupScript(const std::string &script) override;
	virtual void allowAllPopups(bool allow) override;
	virtual void closeBrowser() override;
	virtual void reloadPage() override;
	virtual bool zoomPage(int direction) override;
	virtual void executeJavaScript(const std::string &script) override;

private:
	enum State { Closing = -1, Initial, CreatingBrowser, Loading, Loaded };
	volatile State state = State::Initial;
	CefWindowHandle cefWindowHandle;
	QTimer *showTimer = nullptr;

	std::mutex m;
	std::condition_variable cv;
	bool cefReady = false;

	void removeChildren();
	void showContainer();
	void updateMargins();
	void onBrowserClose();
	void onLoadEnd();
};
