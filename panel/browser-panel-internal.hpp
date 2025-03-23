#pragma once

#include <QTimer>
#include <QPointer>
#include <QPropertyAnimation>
#include <QPainterPath>
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

/* Shows that a browser panel is loading, and covers up any graphical blips
   until the content is completely ready. */
class ProgressWidget : public QWidget {
	Q_OBJECT
	Q_PROPERTY(qreal angle READ getAngle WRITE setAngle)

public:
	static const unsigned int w = 50;
	static const unsigned int h = 50;
	static const unsigned int thickness = 5;

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

#if CEF_USE_VIEWS
class BrowserWindowDelegate : public CefWindowDelegate {

public:
	BrowserWindowDelegate(CefRefPtr<CefView>);

	virtual cef_show_state_t GetInitialShowState(CefRefPtr<CefWindow>) override;
	virtual bool IsFrameless(CefRefPtr<CefWindow>) override;
	virtual bool CanResize(CefRefPtr<CefWindow>) override;
	virtual void OnWindowCreated(CefRefPtr<CefWindow>) override;
	virtual void OnWindowDestroyed(CefRefPtr<CefWindow>) override;

private:
	CefRefPtr<CefView> view;

	IMPLEMENT_REFCOUNTING(BrowserWindowDelegate);
	DISALLOW_COPY_AND_ASSIGN(BrowserWindowDelegate);
};
#endif

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

private:
	friend class QCefBrowserClient;

	QPointer<QWindow> window;
	QPointer<QWidget> container;
#if CEF_USE_VIEWS
	CefRefPtr<CefWindow> cefWindow;
#endif

	enum State { Closing = -1, Initial, Loading, Loaded };
	volatile State state = State::Initial;
	QTimer *showTimer = nullptr;
	std::mutex m;
	std::condition_variable cv;
	bool cefReady = false;

	void resizeBrowser(QResizeEvent *event = nullptr);
	void showContainer();
	void updateMargins();
	void onBrowserClosed(CefRefPtr<CefBrowser> browser);
	void onLoadingFinished();
};
