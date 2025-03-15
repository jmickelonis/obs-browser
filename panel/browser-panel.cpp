#include "browser-panel-internal.hpp"
#include "browser-panel-client.hpp"
#include "cef-headers.hpp"
#include "browser-app.hpp"

#include <QWindow>
#include <QApplication>
#include <QGridLayout>
#include <QStyleOption>
#include <QResizeEvent>
#include <QPainter>

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

ProgressWidget::ProgressWidget(QWidget *parent) : QWidget(parent), gradient(w / 2.0, h / 2.0, 0)
{
	setMaximumSize(sizeHint());

	gradient.setColorAt(0, palette().color(QPalette::Highlight));
	gradient.setColorAt(1, Qt::transparent);

	path.addEllipse(thickness / 2.0, thickness / 2.0, w - thickness, h - thickness);

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
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setAttribute(Qt::WA_StyledBackground);

	setFocusPolicy(Qt::ClickFocus);

	QGridLayout *layout = new QGridLayout();
	layout->setContentsMargins(0, 0, 0, 0);
	setLayout(layout);
	updateMargins();
}

QCefWidgetInternal::~QCefWidgetInternal()
{
	closeBrowser();
}

void QCefWidgetInternal::closeBrowser()
{
	bool wasBrowserActive = state >= State::Loading;

	if (!wasBrowserActive) {
		state = State::Initial;
		return;
	}

	state = State::Closing;
	cefReady = false;

	if (showTimer) {
		delete showTimer;
		showTimer = nullptr;
	}

	QueueCEFTask([this]() {
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
#ifdef _WIN32
		HWND hwnd = (HWND)cefWindowHandle;
		if (hwnd) {
			ShowWindow(hwnd, SW_HIDE);
			SetParent(hwnd, nullptr);
		}
#elif __APPLE__
		// felt hacky, might delete later
		void *view = (id)cefWindowHandle;
		if (*((bool *)view))
			((void (*)(id, SEL))objc_msgSend)((id)view, sel_getUid("removeFromSuperview"));
#endif

		cefBrowser->GetHost()->CloseBrowser(true);
	});

	// Wait for CEF
	{
		std::unique_lock<std::mutex> lk(m);
		cv.wait(lk, [this] { return cefReady; });
	}

	delete container;
	container = nullptr;
	delete window;
	window = nullptr;

	state = State::Initial;
}

/* This is called from the client.
   We wait for this, in order to avoid closing the window too early,
   which can cause the GPU process to crash. */
void QCefWidgetInternal::onBrowserClosed()
{
	CefRefPtr<CefBrowserHost> browserHost = cefBrowser->GetHost();
	QCefBrowserClient *browserClient = reinterpret_cast<QCefBrowserClient *>(browserHost->GetClient().get());
	browserClient->widget = nullptr;

	cefBrowser = nullptr;
	cefWindowHandle = 0;

	// Notify Qt
	{
		std::lock_guard<std::mutex> lk(m);
		cefReady = true;
	}
	cv.notify_one();
}

void QCefWidgetInternal::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	resizeBrowser(event);
}

void QCefWidgetInternal::resizeBrowser(QResizeEvent *event)
{
	if (!cefWindowHandle)
		return;

	const QSize size = event ? event->size() : this->size();
	QMargins margins = contentsMargins();
	qreal ratio = devicePixelRatioF();
	unsigned int w = (size.width() - (margins.left() + margins.right())) * ratio;
	unsigned int h = (size.height() - (margins.top() + margins.bottom())) * ratio;

	QueueCEFTask([this, w, h]() {
		if (!cefWindowHandle)
			return;

#ifdef _WIN32
		SetWindowPos((HWND)handle, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOOWNERZORDER | SWP_NOZORDER);
		SendMessage((HWND)handle, WM_SIZE, 0, MAKELPARAM(w, h));
#else
		Display *xDisplay = cef_get_xdisplay();

		if (!xDisplay)
			return;

		XWindowChanges changes = {0};
		changes.x = 0;
		changes.y = 0;
		changes.width = w;
		changes.height = h;
		XConfigureWindow(xDisplay, (Window)cefWindowHandle, CWX | CWY | CWHeight | CWWidth, &changes);
#if CHROME_VERSION_BUILD >= 4638
		XSync(xDisplay, false);
#endif
#endif
	});
}

void QCefWidgetInternal::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);

	if (state != State::Initial)
		return;

	state = State::Loading;
	cefReady = false;

	if (os_event_try(cef_started_event) != 0) {
		obs_browser_initialize();
		os_event_wait(cef_started_event);
	}

	window = new QWindow();
	window->setFlags(Qt::FramelessWindowHint);
	WId handle = window->winId();

	QueueCEFTask([this, handle]() {
		CefBrowserSettings browserSettings;

		CefWindowInfo windowInfo;
#if CHROME_VERSION_BUILD >= 6533
		windowInfo.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
#endif
		// Set the initial size to 1x1, so resize works later
		// (otherwise floating panels might not have the correct initial size)
		windowInfo.SetAsChild((CefWindowHandle)handle, CefRect(0, 0, 1, 1));

		CefRefPtr<QCefBrowserClient> browserClient = new QCefBrowserClient(this, script, allowAllPopups_);
		cefBrowser = CefBrowserHost::CreateBrowserSync(windowInfo, browserClient, url, browserSettings,
							       CefRefPtr<CefDictionaryValue>(), rqc);
		cefWindowHandle = cefBrowser->GetHost()->GetWindowHandle();

		// Notify Qt
		{
			std::lock_guard<std::mutex> lk(m);
			cefReady = true;
		}
		cv.notify_one();
	});

	// Wait for CEF
	{
		std::unique_lock<std::mutex> lk(m);
		cv.wait(lk, [this] { return cefReady; });
	}

	container = QWidget::createWindowContainer(window);
	container->setVisible(false);
	QGridLayout *layout = static_cast<QGridLayout *>(this->layout());
	layout->addWidget(container, 0, 0);
	layout->addWidget(new ProgressWidget, 0, 0, Qt::AlignCenter);

	resizeBrowser();

	if (state == State::Loaded)
		// Finished already
		showContainer();
}

void QCefWidgetInternal::onLoadingFinished()
{
	if (state != State::Loading)
		return;

	state = State::Loaded;
	QTimer::singleShot(0, this, &QCefWidgetInternal::showContainer);
}

void QCefWidgetInternal::showContainer()
{
	if (showTimer)
		return;

	// Show the container after a delay to cover up a lot of loading blips
	showTimer = new QTimer();
	showTimer->setInterval(250);
	showTimer->setSingleShot(true);
	connect(showTimer, &QTimer::timeout, this, [this]() {
		if (state != State::Loaded || container->isVisible())
			return;

		delete showTimer;
		showTimer = nullptr;

		// Dispose of the progress indicator
		QLayoutItem *child = layout()->takeAt(1);
		delete child->widget();
		delete child;

		// Show the CEF window
		container->setVisible(true);
	});
	showTimer->start();
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
