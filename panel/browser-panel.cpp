#include "browser-panel-internal.hpp"
#include "browser-panel-client.hpp"
#include "cef-headers.hpp"
#include "browser-app.hpp"

#include <QWindow>
#include <QApplication>

#ifdef USE_QT_LOOP
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

#include <QDockWidget>
#include <QGridLayout>
#include <QPainter>
#include <QPainterPath>
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"

#if !defined(_WIN32) && !defined(__APPLE__)
#include <X11/Xlib.h>
#endif

extern bool QueueCEFTask(std::function<void()> task);
extern "C" void obs_browser_initialize(void);
extern os_event_t *cef_started_event;

std::mutex popup_whitelist_mutex;
std::vector<PopupWhitelistInfo> popup_whitelist;
std::vector<PopupWhitelistInfo> forced_popups;

/* ------------------------------------------------------------------------- */

class CookieCheck : public CefCookieVisitor {
public:
	QCefCookieManager::cookie_exists_cb callback;
	std::string target;
	bool cookie_found = false;

	inline CookieCheck(QCefCookieManager::cookie_exists_cb callback_,
			   const std::string target_)
		: callback(callback_), target(target_)
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

	QCefCookieManagerInternal(const std::string &storage_path,
				  bool persist_session_cookies)
	{
		if (os_event_try(cef_started_event) != 0)
			throw "Browser thread not initialized";

		BPtr<char> rpath = obs_module_config_path(storage_path.c_str());
		if (os_mkdirs(rpath.Get()) == MKDIR_ERROR)
			throw "Failed to create cookie directory";

		BPtr<char> path = os_get_abs_path_ptr(rpath.Get());

		CefRequestContextSettings settings;
		CefString(&settings.cache_path) = path.Get();
		rc = CefRequestContext::CreateContext(
			settings, CefRefPtr<CefRequestContextHandler>());
		if (rc)
			cm = rc->GetCookieManager(nullptr);

		UNUSED_PARAMETER(persist_session_cookies);
	}

	virtual bool DeleteCookies(const std::string &url,
				   const std::string &name) override
	{
		return !!cm ? cm->DeleteCookies(url, name, nullptr) : false;
	}

	virtual bool SetStoragePath(const std::string &storage_path,
				    bool persist_session_cookies) override
	{
		BPtr<char> rpath = obs_module_config_path(storage_path.c_str());
		BPtr<char> path = os_get_abs_path_ptr(rpath.Get());

		CefRequestContextSettings settings;
		CefString(&settings.cache_path) = storage_path;
		rc = CefRequestContext::CreateContext(
			settings, CefRefPtr<CefRequestContextHandler>());
		if (rc)
			cm = rc->GetCookieManager(nullptr);

		UNUSED_PARAMETER(persist_session_cookies);
		return true;
	}

	virtual bool FlushStore() override
	{
		return !!cm ? cm->FlushStore(nullptr) : false;
	}

	virtual void CheckForCookie(const std::string &site,
				    const std::string &cookie,
				    cookie_exists_cb callback) override
	{
		if (!cm)
			return;

		CefRefPtr<CookieCheck> c = new CookieCheck(callback, cookie);
		cm->VisitUrlCookies(site, false, c);
	}
};

/* ------------------------------------------------------------------------- */

ProgressWidget::ProgressWidget(QWidget *parent)
	: QWidget(parent), gradient(w / 2, h / 2, 0)
{
	setMaximumSize(sizeHint());

	gradient.setColorAt(0, palette().color(QPalette::Highlight));
	gradient.setColorAt(1, Qt::transparent);

	path.addEllipse(
		thickness / 2, thickness / 2,
		w - thickness, h - thickness);

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


QCefWidgetInternal::QCefWidgetInternal(QWidget *parent, const std::string &url_,
				       CefRefPtr<CefRequestContext> rqc_)
	: QCefWidget(parent), url(url_), rqc(rqc_)
{
	// setAttribute(Qt::WA_PaintOnScreen);
	// setAttribute(Qt::WA_StaticContents);
	// setAttribute(Qt::WA_NoSystemBackground);
	// setAttribute(Qt::WA_OpaquePaintEvent);
	// setAttribute(Qt::WA_DontCreateNativeAncestors);
	// setAttribute(Qt::WA_NativeWindow);

	setFocusPolicy(Qt::ClickFocus);

	QGridLayout *layout = new QGridLayout();
	layout->setContentsMargins(0, 0, 0, 0);
	setLayout(layout);
}

QCefWidgetInternal::~QCefWidgetInternal()
{
	loading = false;
	closeBrowser();

	if (cefWindow) {
		cefWindow->setParent(nullptr);
		delete cefWindow;
		cefWindow = nullptr;
	}

	cefContainer = nullptr;
	removeChildren();
}

void QCefWidgetInternal::closeBrowser()
{
	timer.stop();

	if (!!cefBrowser) {
		CefRefPtr<CefClient> client = cefBrowser->GetHost()->GetClient();
		QCefBrowserClient *browserClient =
			reinterpret_cast<QCefBrowserClient *>(client.get());
		if (browserClient)
			browserClient->widget = nullptr;

		// Close from CEF's event loop and wait for it to finish
		volatile bool closing = true;
		QueueCEFTask([&]() {
			nativeWindow->Hide();
			cefBrowser->GetHost()->CloseBrowser(true);
			closing = false;
		});
		while (closing) os_sleep_ms(1);

		cefBrowser = nullptr;
		nativeWindow = nullptr;

		/* So you're probably wondering what's going on here.  If you
		 * call CefBrowserHost::CloseBrowser, and it fails to unload
		 * the web page *before* WM_NCDESTROY is called on the browser
		 * HWND, it will call an internal CEF function
		 * CefBrowserPlatformDelegateNativeWin::CloseHostWindow, which
		 * will attempt to close the browser's main window itself.
		 * Problem is, this closes the root window containing the
		 * browser's HWND rather than the browser's specific HWND for
		 * whatever mysterious reason.  If the browser is in a dock
		 * widget, then the window it closes is, unfortunately, the
		 * main program's window, causing the entire program to shut
		 * down.
		 *
		 * So, instead, before closing the browser, we need to decouple
		 * the browser from the widget.  To do this, we hide it, then
		 * remove its parent. */
// #ifdef _WIN32
// 		HWND hwnd = (HWND)cefBrowser->GetHost()->GetWindowHandle();
// 		if (hwnd) {
// 			ShowWindow(hwnd, SW_HIDE);
// 			SetParent(hwnd, nullptr);
// 		}
// #elif __APPLE__
// 		// felt hacky, might delete later
// 		id view = (id)cefBrowser->GetHost()->GetWindowHandle();
// 		((void (*)(id, SEL))objc_msgSend)(
// 			view, sel_getUid("removeFromSuperview"));
// #endif
	}
}

#ifdef __linux__
static bool XWindowHasAtom(Display *display, Window w, Atom a)
{
	Atom type;
	int format;
	unsigned long nItems;
	unsigned long bytesAfter;
	unsigned char *data = NULL;

	if (XGetWindowProperty(display, w, a, 0, LONG_MAX, False,
			       AnyPropertyType, &type, &format, &nItems,
			       &bytesAfter, &data) != Success)
		return false;

	if (data)
		XFree(data);

	return type != None;
}

/* On Linux / X11, CEF sets the XdndProxy of the toplevel window
 * it's attached to, so that it can read drag events. When this
 * toplevel happens to be OBS Studio's main window (e.g. when a
 * browser panel is docked into to the main window), setting the
 * XdndProxy atom ends up breaking DnD of sources and scenes. Thus,
 * we have to manually unset this atom.
 */
void QCefWidgetInternal::unsetToplevelXdndProxy()
{
	if (!cefBrowser)
		return;

	CefWindowHandle browserHandle =
		cefBrowser->GetHost()->GetWindowHandle();
	Display *xDisplay = cef_get_xdisplay();
	Window toplevel, root, parent, *children;
	unsigned int nChildren;
	bool found = false;

	toplevel = browserHandle;

	// Find the toplevel
	Atom netWmPidAtom = XInternAtom(xDisplay, "_NET_WM_PID", False);
	do {
		if (XQueryTree(xDisplay, toplevel, &root, &parent, &children,
			       &nChildren) == 0)
			return;

		if (children)
			XFree(children);

		if (root == parent ||
		    !XWindowHasAtom(xDisplay, parent, netWmPidAtom)) {
			found = true;
			break;
		}
		toplevel = parent;
	} while (true);

	if (!found)
		return;

	// Check if the XdndProxy property is set
	Atom xDndProxyAtom = XInternAtom(xDisplay, "XdndProxy", False);
	if (needsDeleteXdndProxy &&
	    !XWindowHasAtom(xDisplay, toplevel, xDndProxyAtom)) {
		QueueCEFTask([this]() { unsetToplevelXdndProxy(); });
		return;
	}

	XDeleteProperty(xDisplay, toplevel, xDndProxyAtom);
	needsDeleteXdndProxy = false;
}
#endif

// Instead of the past solution (letting CEF render to our widgets directly),
// we let CEF create its own window, then grab it in a container so we can embed it.
// This solves a lot of issues we were seeing before (visual glitches, docks popping
// back out on drags, docks not rendering at all, etc).
// We don't even have to resize manually any more!
// Still seeing some segfaults on app exit, but this is much closer to being perfect.
class BrowserWindowDelegate : public CefWindowDelegate
{

public:

	BrowserWindowDelegate(CefRefPtr<CefBrowserView> browserView)
		: browserView(browserView)
	{ }

	virtual cef_show_state_t GetInitialShowState(CefRefPtr<CefWindow>) override
	{
		return CEF_SHOW_STATE_MINIMIZED;
	}

	virtual bool IsFrameless(CefRefPtr<CefWindow>) override
	{
		// For some reason returning true prevents presses near the border
		return false;
	}

	virtual bool CanResize(CefRefPtr<CefWindow>) override
	{
		return false;
	}

	void OnWindowCreated(CefRefPtr<CefWindow> window) override
	{
		window->SetBackgroundColor(CefColorSetARGB(0, 0, 0, 0));
		window->AddChildView(browserView);
	}

	void OnWindowDestroyed(CefRefPtr<CefWindow>) override
	{
		browserView = nullptr;
	}

	bool CanClose(CefRefPtr<CefWindow> window) override
	{
		if (browserView)
			// Removing the view before closing
			// helps prevent crashes when switching profiles
			window->RemoveChildView(browserView);
		return true;
	}

private:

	CefRefPtr<CefBrowserView> browserView;

	IMPLEMENT_REFCOUNTING(BrowserWindowDelegate);
	DISALLOW_COPY_AND_ASSIGN(BrowserWindowDelegate);

};

void QCefWidgetInternal::Init()
{
	bool success = QueueCEFTask([this]() {
		/* Make sure Init isn't called more than once. */
		if (cefBrowser)
			return;

		// See comments in BrowserWindowDelegate!

		CefBrowserSettings cefBrowserSettings;
		cefBrowserSettings.background_color = CefColorSetARGB(0, 0, 0, 0);

		CefRefPtr<CefBrowserView> browserView = CefBrowserView::CreateBrowserView(
			new QCefBrowserClient(this, script, allowAllPopups_),
			url, cefBrowserSettings,
			nullptr, rqc, nullptr);
		browserView->SetBackgroundColor(CefColorSetARGB(0, 0, 0, 0));
		nativeWindow = CefWindow::CreateTopLevelWindow(
			new BrowserWindowDelegate(browserView));

		CefRefPtr<CefBrowser> browser = browserView->GetBrowser();
		cefBrowser = browser;

		auto windowHandle = nativeWindow->GetWindowHandle();
		QTimer::singleShot(0, this, [this, browser, windowHandle]() {
			// We're back in the Qt event loop

			if (browser != cefBrowser)
				return;

			// Grab the browser window and put it in a container
			cefWindow = QWindow::fromWinId((WId) windowHandle);
			cefContainer = QWidget::createWindowContainer(cefWindow, this);
			// Set the initial size, otherwise it looks glitchy at first
			QRect bounds = contentsRect();
			cefContainer->resize(bounds.width(), bounds.height());
			// We'll make it visible after the browser is done loading
			cefContainer->setVisible(false);
			layout()->addWidget(cefContainer);

			auto showNative = [this, browser]() {
				QueueCEFTask([this, browser]() {
					if (browser != cefBrowser)
						return;
					// This won't show anything yet since the container isn't visible
					nativeWindow->Show();
				});
			};
#ifdef __linux__
			// Delay this slightly on Linux or it doesn't grab the window properly
			QTimer::singleShot(50, this, showNative);
#else
			showNative();
#endif

			if (!loading)
				// Finished already
				showContainer();
		});
	});

	if (success)
		timer.stop();
}

void QCefWidgetInternal::removeChildren()
{
	QLayout *layout = this->layout();
	QLayoutItem *child;
	while ((child = layout->takeAt(0)) != nullptr) {
		delete child->widget();
		delete child;
	}
}

void QCefWidgetInternal::showContainer()
{
	if (!cefContainer)
		return;

	// Show the container after a delay,
	// which will cover up a lot of stylesheet and loading blips
	CefRefPtr<CefBrowser> browser = cefBrowser;
	QTimer::singleShot(250, this, [this, browser]() {
		if (cefBrowser != browser)
			return;
		// Dispose of the progress indicator
		QLayoutItem *child = layout()->takeAt(0);
		delete child->widget();
		delete child;
		// Show the CEF window
		cefContainer->setVisible(true);
	});
}

void QCefWidgetInternal::onLoadEnd()
{
	if (!loading)
		return;

	loading = false;

	if (cefContainer) {
		nativeWindow->Show();
		QTimer::singleShot(0, this, &QCefWidgetInternal::showContainer);
	}
}

void QCefWidgetInternal::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
#ifndef __APPLE__
	// Resize();
}

void QCefWidgetInternal::Resize()
{
	qreal pixelRatio = devicePixelRatioF();
	QSize size = this->size() * pixelRatio;
	QMargins margins = contentsMargins() * pixelRatio;

	/*bool success =*/ QueueCEFTask([this, size, margins]() {
		if (!cefBrowser)
			return;

		CefWindowHandle handle =
			cefBrowser->GetHost()->GetWindowHandle();

		if (!handle)
			return;

#ifdef _WIN32
		SetWindowPos((HWND)handle, nullptr, 0, 0, size.width(),
			     size.height(),
			     SWP_NOMOVE | SWP_NOOWNERZORDER | SWP_NOZORDER);
		SendMessage((HWND)handle, WM_SIZE, 0,
			    MAKELPARAM(size.width(), size.height()));
#else
		Display *xDisplay = cef_get_xdisplay();

		if (!xDisplay)
			return;

		XWindowChanges changes = {0};
		int x = margins.left();
		int y = margins.top();
		changes.x = x;
		changes.y = y;
		changes.width = size.width() - (x + margins.right());
		changes.height = size.height() - (y + margins.bottom());
		XConfigureWindow(xDisplay, (Window)handle,
				 CWX | CWY | CWHeight | CWWidth, &changes);

#if CHROME_VERSION_BUILD >= 4638
		XFlush(xDisplay);
#endif
#endif
	});

	// if (success && container)
	// 	container->resize(size.width(), size.height());
#endif
}

void QCefWidgetInternal::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);

	if (!cefBrowser) {
		loading = true;

		// Show the progress indicator until the browser is done loading
		removeChildren();
		layout()->addWidget(new ProgressWidget);

		obs_browser_initialize();
		connect(&timer, SIGNAL(timeout()), this, SLOT(Init()));
		timer.start(500);
		Init();
	}
}

QPaintEngine *QCefWidgetInternal::paintEngine() const
{
	// return nullptr;
	return QCefWidget::paintEngine();
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

void QCefWidgetInternal::allowAllPopups(bool allow)
{
	allowAllPopups_ = allow;
}

/* ------------------------------------------------------------------------- */

struct QCefInternal : QCef {
	virtual bool init_browser(void) override;
	virtual bool initialized(void) override;
	virtual bool wait_for_browser_init(void) override;

	virtual QCefWidget *
	create_widget(QWidget *parent, const std::string &url,
		      QCefCookieManager *cookie_manager) override;

	virtual QCefCookieManager *
	create_cookie_manager(const std::string &storage_path,
			      bool persist_session_cookies) override;

	virtual BPtr<char>
	get_cookie_path(const std::string &storage_path) override;

	virtual void add_popup_whitelist_url(const std::string &url,
					     QObject *obj) override;
	virtual void add_force_popup_url(const std::string &url,
					 QObject *obj) override;
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

QCefWidget *QCefInternal::create_widget(QWidget *parent, const std::string &url,
					QCefCookieManager *cm)
{
	QCefCookieManagerInternal *cmi =
		reinterpret_cast<QCefCookieManagerInternal *>(cm);

	return new QCefWidgetInternal(parent, url, cmi ? cmi->rc : nullptr);
}

QCefCookieManager *
QCefInternal::create_cookie_manager(const std::string &storage_path,
				    bool persist_session_cookies)
{
	try {
		return new QCefCookieManagerInternal(storage_path,
						     persist_session_cookies);
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

#define BROWSER_PANEL_VERSION 2

extern "C" EXPORT int obs_browser_qcef_version_export(void)
{
	return BROWSER_PANEL_VERSION;
}
