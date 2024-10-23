#include "browser-panel-internal.hpp"
#include "browser-panel-client.hpp"
#include "cef-headers.hpp"
#include "browser-app.hpp"

#include <QWindow>
#include <QApplication>

#ifdef ENABLE_BROWSER_QT_LOOP
#include <QEventLoop>
#include <QThread>
#endif

#ifdef __APPLE__
#include <objc/objc.h>
#endif

#include <obs-module.h>
#include <util/threading.h>
#include <util/base.h>
#include <thread>
#include <cmath>

#include <QGridLayout>
#include <QPainter>
#include <QStyle>
#include <QStyleOption>
#include "include/views/cef_browser_view.h"

#if !defined(_WIN32) && !defined(__APPLE__)
#include <X11/Xlib.h>
#endif

extern bool QueueCEFTask(std::function<void()> task);
extern "C" void obs_browser_initialize(void);
extern os_event_t *cef_started_event;

std::mutex popup_whitelist_mutex;
std::vector<PopupWhitelistInfo> popup_whitelist;
std::vector<PopupWhitelistInfo> forced_popups;

static int zoomLvls[] = {25, 33, 50, 67, 75, 80, 90, 100, 110, 125, 150, 175, 200, 250, 300, 400};

/* ------------------------------------------------------------------------- */

class CookieCheck : public CefCookieVisitor {
public:
	QCefCookieManager::cookie_exists_cb callback;
	std::string target;
	bool cookie_found = false;

	inline CookieCheck(QCefCookieManager::cookie_exists_cb callback_, const std::string target_)
		: callback(callback_),
		  target(target_)
	{
	}

	virtual ~CookieCheck() { callback(cookie_found); }

	virtual bool Visit(const CefCookie &cookie, int, int, bool &) override
	{
		CefString cef_name = cookie.name.str;
		std::string name = cef_name;

		if (name == target) {
			cookie_found = true;
			return false;
		}
		return true;
	}

	IMPLEMENT_REFCOUNTING(CookieCheck);
};

struct QCefCookieManagerInternal : QCefCookieManager {
	CefRefPtr<CefCookieManager> cm;
	CefRefPtr<CefRequestContext> rc;

	QCefCookieManagerInternal(const std::string &storage_path, bool persist_session_cookies)
	{
		if (os_event_try(cef_started_event) != 0)
			throw "Browser thread not initialized";

		BPtr<char> rpath = obs_module_config_path(storage_path.c_str());
		if (os_mkdirs(rpath.Get()) == MKDIR_ERROR)
			throw "Failed to create cookie directory";

		BPtr<char> path = os_get_abs_path_ptr(rpath.Get());

		CefRequestContextSettings settings;
#if CHROME_VERSION_BUILD <= 6533
		settings.persist_user_preferences = 1;
#endif
		CefString(&settings.cache_path) = path.Get();
		rc = CefRequestContext::CreateContext(settings, CefRefPtr<CefRequestContextHandler>());
		if (rc)
			cm = rc->GetCookieManager(nullptr);

		UNUSED_PARAMETER(persist_session_cookies);
	}

	virtual bool DeleteCookies(const std::string &url, const std::string &name) override
	{
		return !!cm ? cm->DeleteCookies(url, name, nullptr) : false;
	}

	virtual bool SetStoragePath(const std::string &storage_path, bool persist_session_cookies) override
	{
		BPtr<char> rpath = obs_module_config_path(storage_path.c_str());
		BPtr<char> path = os_get_abs_path_ptr(rpath.Get());

		CefRequestContextSettings settings;
#if CHROME_VERSION_BUILD <= 6533
		settings.persist_user_preferences = 1;
#endif
		CefString(&settings.cache_path) = storage_path;
		rc = CefRequestContext::CreateContext(settings, CefRefPtr<CefRequestContextHandler>());
		if (rc)
			cm = rc->GetCookieManager(nullptr);

		UNUSED_PARAMETER(persist_session_cookies);
		return true;
	}

	virtual bool FlushStore() override { return !!cm ? cm->FlushStore(nullptr) : false; }

	virtual void CheckForCookie(const std::string &site, const std::string &cookie,
				    cookie_exists_cb callback) override
	{
		if (!cm)
			return;

		CefRefPtr<CookieCheck> c = new CookieCheck(callback, cookie);
		cm->VisitUrlCookies(site, false, c);
	}
};

/* ------------------------------------------------------------------------- */

ProgressWidget::ProgressWidget(QWidget *parent) : QWidget(parent), gradient(w / 2, h / 2, 0)
{
	setMaximumSize(sizeHint());

	gradient.setColorAt(0, palette().color(QPalette::Highlight));
	gradient.setColorAt(1, Qt::transparent);

	path.addEllipse(thickness / 2, thickness / 2, w - thickness, h - thickness);

	animation = nullptr;
}

ProgressWidget::~ProgressWidget()
{
	if (animation) {
		delete animation;
		animation = nullptr;
	}
}

qreal ProgressWidget::getAngle()
{
	return gradient.angle();
}

void ProgressWidget::setAngle(qreal angle)
{
	gradient.setAngle(angle);
	update();
}

QSize ProgressWidget::sizeHint() const
{
	return QSize(w, h);
}

bool ProgressWidget::event(QEvent *event)
{
	switch (event->type()) {
	case QEvent::PaletteChange:
		gradient.setColorAt(0, palette().color(QPalette::Highlight));
		break;
	case QEvent::Show:
		if (!animation) {
			animation = new QPropertyAnimation(this, "angle");
			animation->setDuration(1000);
			animation->setStartValue(360);
			animation->setEndValue(0);
			animation->setLoopCount(-1);
			animation->start();
		}
		break;
	case QEvent::Hide:
		if (animation) {
			animation->stop();
			delete animation;
			animation = nullptr;
		}
		break;
	default:
		break;
	}
	return QWidget::event(event);
}

void ProgressWidget::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(QPen(gradient, thickness));
	painter.drawPath(path);
}

QCefWidgetInternal::QCefWidgetInternal(QWidget *parent, const std::string &url_, CefRefPtr<CefRequestContext> rqc_)
	: QCefWidget(parent),
	  url(url_),
	  rqc(rqc_)
{
	// setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	// setAttribute(Qt::WA_NoSystemBackground);
	// setAttribute(Qt::WA_OpaquePaintEvent);
	// setAttribute(Qt::WA_DontCreateNativeAncestors);
	// setAttribute(Qt::WA_NativeWindow);
	setAttribute(Qt::WA_StyledBackground);

	setFocusPolicy(Qt::ClickFocus);

	QGridLayout *layout = new QGridLayout();
	layout->setContentsMargins(0, 0, 0, 0);
	setLayout(layout);

	updateMargins();
}

QCefWidgetInternal::~QCefWidgetInternal()
{
	timer.stop();
	loading = false;
	if (window)
		window->setParent(nullptr);
	closeBrowser();
	delete window;
	window = nullptr;
	delete container;
	container = nullptr;
	removeChildren();
}

void QCefWidgetInternal::closeBrowser()
{
	CefRefPtr<CefBrowser> browser = cefBrowser;
	if (!!browser) {
		removeChildren();

		// Close from CEF's event loop
		QueueCEFTask([&]() {
			cefBrowser = nullptr;
			CefRefPtr<CefBrowserHost> browserHost = browser->GetHost();

#ifdef _WIN32
			HWND hwnd = (HWND)browserHost->GetWindowHandle();
			if (hwnd)
				DestroyWindow(hwnd);
#else
			cefWindow->Close();
			cefWindow = nullptr;
#endif

			browserHost->CloseBrowser(true);
		});

		QEventLoop loop;
		connect(this, &QCefWidgetInternal::readyToClose, &loop, &QEventLoop::quit);
		QTimer::singleShot(1000, &loop, &QEventLoop::quit);
		loop.exec();

		cefBrowser = nullptr;
	}
}

#ifndef _WIN32
/*
Instead of the past solution (letting CEF render to our widgets directly),
we let CEF create its own window, then grab it in a container so we can embed it.
This solves a lot of issues we were seeing before
(visual glitches, docks popping back out on drags, docks not rendering at all, etc).
We don't even have to resize manually any more!
*/
class BrowserWindowDelegate : public CefWindowDelegate {

public:
	BrowserWindowDelegate(CefRefPtr<CefView> view) : view(view) {}

	virtual cef_show_state_t GetInitialShowState(CefRefPtr<CefWindow>) override { return CEF_SHOW_STATE_MINIMIZED; }

	virtual bool IsFrameless(CefRefPtr<CefWindow>) override
	{
		// For some reason going frameless prevents presses near the border
		return false;
	}

	virtual bool CanResize(CefRefPtr<CefWindow>) override { return false; }

	void OnWindowCreated(CefRefPtr<CefWindow> window) override
	{
		window->SetBackgroundColor(CefColorSetARGB(0, 0, 0, 0));
		window->AddChildView(view);
	}

	void OnWindowDestroyed(CefRefPtr<CefWindow>) override { view = nullptr; }

private:
	CefRefPtr<CefView> view;

	IMPLEMENT_REFCOUNTING(BrowserWindowDelegate);
	DISALLOW_COPY_AND_ASSIGN(BrowserWindowDelegate);
};
#endif

void QCefWidgetInternal::Init()
{
	bool success = QueueCEFTask([this]() {
		// Make sure Init isn't called more than once
		if (cefBrowser)
			return;

		CefBrowserSettings browserSettings;
		browserSettings.background_color = CefColorSetARGB(0, 0, 0, 0);
		CefRefPtr<QCefBrowserClient> browserClient = new QCefBrowserClient(this, script, allowAllPopups_);

#ifdef _WIN32
		// Have to go with the "old" way for Windows
		// Using the views framework, the native windows will not focus properly,
		// and will not receive key events (can't type)
		CefWindowInfo windowInfo;
		windowInfo.style = WS_POPUP;

#if CHROME_VERSION_BUILD >= 6533
		windowInfo.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
#endif

		CefRefPtr<CefBrowser> browser = CefBrowserHost::CreateBrowserSync(windowInfo, browserClient, url,
										  browserSettings, nullptr, rqc);
		auto windowHandle = browser->GetHost()->GetWindowHandle();
#else
		// See comments in BrowserWindowDelegate!
		CefRefPtr<CefBrowserView> browserView =
			CefBrowserView::CreateBrowserView(browserClient, url, browserSettings, nullptr, rqc, nullptr);
		browserView->SetBackgroundColor(CefColorSetARGB(0, 0, 0, 0));
		cefWindow = CefWindow::CreateTopLevelWindow(new BrowserWindowDelegate(browserView));
		CefRefPtr<CefBrowser> browser = browserView->GetBrowser();
		auto windowHandle = cefWindow->GetWindowHandle();
#endif

		cefBrowser = browser;

		QTimer::singleShot(0, this, [this, browser, windowHandle]() {
			// We're back in the Qt event loop

			if (browser != cefBrowser)
				return;

			// Grab the browser window and put it in a container
			window = QWindow::fromWinId((WId)windowHandle);
			container = QWidget::createWindowContainer(window);

			// This stops the animations/blips when closing later
			window->setOpacity(0);
			window->setFlags(window->flags() | Qt::BypassWindowManagerHint);

			// Set the initial size, otherwise it looks glitchy at first
			QRect bounds = contentsRect();
			container->resize(bounds.width(), bounds.height());

			// We'll make it visible after the browser is done loading
			container->setVisible(false);
			layout()->addWidget(container);

			if (!loading)
				// Finished already
				showContainer();
		});
	});

	if (success)
		timer.stop();
}

bool QCefWidgetInternal::event(QEvent *event)
{
	switch (event->type()) {
	case QEvent::StyleChange:
		updateMargins();
		break;
	default:
		break;
	}

	return QCefWidget::event(event);
}

void QCefWidgetInternal::CloseSafely()
{
	emit readyToClose();
}

void QCefWidgetInternal::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);

	if (!cefBrowser) {
		// Show the progress indicator until the browser is done loading
		loading = true;
		removeChildren();
		layout()->addWidget(new ProgressWidget);

		obs_browser_initialize();
		connect(&timer, &QTimer::timeout, this, &QCefWidgetInternal::Init);
		timer.start(500);
		Init();
	}
}

void QCefWidgetInternal::setURL(const std::string &url_)
{
	url = url_;
	if (cefBrowser) {
		cefBrowser->GetMainFrame()->LoadURL(url);
	}
}

void QCefWidgetInternal::reloadPage()
{
	if (cefBrowser)
		cefBrowser->ReloadIgnoreCache();
}

void QCefWidgetInternal::setStartupScript(const std::string &script_)
{
	script = script_;
}

void QCefWidgetInternal::executeJavaScript(const std::string &script_)
{
	if (!cefBrowser)
		return;

	CefRefPtr<CefFrame> frame = cefBrowser->GetMainFrame();
	std::string url = frame->GetURL();
	frame->ExecuteJavaScript(script_, url, 0);
}

void QCefWidgetInternal::allowAllPopups(bool allow)
{
	allowAllPopups_ = allow;
}

bool QCefWidgetInternal::zoomPage(int direction)
{
	if (!cefBrowser || direction < -1 || direction > 1)
		return false;

	CefRefPtr<CefBrowserHost> host = cefBrowser->GetHost();
	if (direction == 0) {
		// Reset zoom
		host->SetZoomLevel(0);
		return true;
	}

	int currentZoomPercent = round(pow(1.2, host->GetZoomLevel()) * 100.0);
	int zoomCount = sizeof(zoomLvls) / sizeof(zoomLvls[0]);
	int zoomIdx = 0;

	while (zoomIdx < zoomCount) {
		if (zoomLvls[zoomIdx] == currentZoomPercent) {
			break;
		}
		zoomIdx++;
	}
	if (zoomIdx == zoomCount)
		return false;

	int newZoomIdx = zoomIdx;
	if (direction == -1 && zoomIdx > 0) {
		// Zoom out
		newZoomIdx -= 1;
	} else if (direction == 1 && zoomIdx >= 0 && zoomIdx < zoomCount - 1) {
		// Zoom in
		newZoomIdx += 1;
	}

	if (newZoomIdx != zoomIdx) {
		int newZoomLvl = zoomLvls[newZoomIdx];
		// SetZoomLevel only accepts a zoomLevel, not a percentage
		host->SetZoomLevel(log(newZoomLvl / 100.0) / log(1.2));
		return true;
	}
	return false;
}

void QCefWidgetInternal::removeChildren()
{
	qDeleteAll(findChildren<QWidget*>("", Qt::FindDirectChildrenOnly));
}

void QCefWidgetInternal::showContainer()
{
	if (!container)
		return;

	// Show the container after a delay to cover up a lot of loading blips
	CefRefPtr<CefBrowser> browser = cefBrowser;
	QTimer::singleShot(500, this, [this, browser]() {
		if (cefBrowser != browser)
			return;
		// Dispose of the progress indicator
		delete layout()->takeAt(0);
		// Show the CEF window
		container->setVisible(true);
	});
}

void QCefWidgetInternal::updateMargins()
{
	QStyleOption opt;
	opt.initFrom(this);
	opt.rect.setRect(0, 0, 0xffff, 0xffff);

	QRect rect = style()->subElementRect(QStyle::SE_ShapedFrameContents, &opt, this);
	if (rect.isValid()) {
		setContentsMargins(rect.left(), rect.top(), opt.rect.right() - rect.right(),
				   opt.rect.bottom() - rect.bottom());
	} else {
		setContentsMargins(0, 0, 0, 0);
	}
}

void QCefWidgetInternal::onLoadEnd()
{
	if (!loading)
		return;

	loading = false;

	if (container) {
#ifndef _WIN32
		cefWindow->Show();
#endif
		QTimer::singleShot(0, this, &QCefWidgetInternal::showContainer);
	}
}

/* ------------------------------------------------------------------------- */

struct QCefInternal : QCef {
	virtual bool init_browser(void) override;
	virtual bool initialized(void) override;
	virtual bool wait_for_browser_init(void) override;

	virtual QCefWidget *create_widget(QWidget *parent, const std::string &url,
					  QCefCookieManager *cookie_manager) override;

	virtual QCefCookieManager *create_cookie_manager(const std::string &storage_path,
							 bool persist_session_cookies) override;

	virtual BPtr<char> get_cookie_path(const std::string &storage_path) override;

	virtual void add_popup_whitelist_url(const std::string &url, QObject *obj) override;
	virtual void add_force_popup_url(const std::string &url, QObject *obj) override;
};

bool QCefInternal::init_browser(void)
{
	if (os_event_try(cef_started_event) == 0)
		return true;

	obs_browser_initialize();
	return false;
}

bool QCefInternal::initialized(void)
{
	return os_event_try(cef_started_event) == 0;
}

bool QCefInternal::wait_for_browser_init(void)
{
	return os_event_wait(cef_started_event) == 0;
}

QCefWidget *QCefInternal::create_widget(QWidget *parent, const std::string &url, QCefCookieManager *cm)
{
	QCefCookieManagerInternal *cmi = reinterpret_cast<QCefCookieManagerInternal *>(cm);

	return new QCefWidgetInternal(parent, url, cmi ? cmi->rc : nullptr);
}

QCefCookieManager *QCefInternal::create_cookie_manager(const std::string &storage_path, bool persist_session_cookies)
{
	try {
		return new QCefCookieManagerInternal(storage_path, persist_session_cookies);
	} catch (const char *error) {
		blog(LOG_ERROR, "Failed to create cookie manager: %s", error);
		return nullptr;
	}
}

BPtr<char> QCefInternal::get_cookie_path(const std::string &storage_path)
{
	BPtr<char> rpath = obs_module_config_path(storage_path.c_str());
	return os_get_abs_path_ptr(rpath.Get());
}

void QCefInternal::add_popup_whitelist_url(const std::string &url, QObject *obj)
{
	std::lock_guard<std::mutex> lock(popup_whitelist_mutex);
	popup_whitelist.emplace_back(url, obj);
}

void QCefInternal::add_force_popup_url(const std::string &url, QObject *obj)
{
	std::lock_guard<std::mutex> lock(popup_whitelist_mutex);
	forced_popups.emplace_back(url, obj);
}

extern "C" EXPORT QCef *obs_browser_create_qcef(void)
{
	return new QCefInternal();
}

#define BROWSER_PANEL_VERSION 3

extern "C" EXPORT int obs_browser_qcef_version_export(void)
{
	return BROWSER_PANEL_VERSION;
}
