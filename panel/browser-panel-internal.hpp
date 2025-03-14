#pragma once

#include <QTimer>
#include <QPointer>
#include "browser-panel.hpp"
#include "cef-headers.hpp"

#include <vector>
#include <mutex>
#include <condition_variable>

struct PopupWhitelistInfo {
	std::string url;
	QPointer<QObject> obj;

	inline PopupWhitelistInfo(const std::string &url_, QObject *obj_) : url(url_), obj(obj_) {}
};

extern std::mutex popup_whitelist_mutex;
extern std::vector<PopupWhitelistInfo> popup_whitelist;
extern std::vector<PopupWhitelistInfo> forced_popups;

/* ------------------------------------------------------------------------- */

class QCefWidgetInternal : public QCefWidget {
	Q_OBJECT

public:
	QCefWidgetInternal(QWidget *parent, const std::string &url, CefRefPtr<CefRequestContext> rqc);
	~QCefWidgetInternal();

	CefRefPtr<CefBrowser> cefBrowser;
	std::string url;
	std::string script;
	CefRefPtr<CefRequestContext> rqc;
	bool allowAllPopups_ = false;

	virtual void resizeEvent(QResizeEvent *event) override;
	virtual void showEvent(QShowEvent *event) override;
	virtual bool event(QEvent *event) override;

	virtual void setURL(const std::string &url) override;
	virtual void setStartupScript(const std::string &script) override;
	virtual void allowAllPopups(bool allow) override;
	virtual void closeBrowser() override;
	virtual void reloadPage() override;
	virtual bool zoomPage(int direction) override;
	virtual void executeJavaScript(const std::string &script) override;

	void Resize();

private:
	QPointer<QWindow> window;
	QPointer<QWidget> container;
	enum State { Closing = -1, Initial, Active };
	volatile State state = State::Initial;
	std::mutex m;
	std::condition_variable cv;
	bool cefReady = false;

	void updateMargins();
};
