/******************************************************************************
 Copyright (C) 2014 by John R. Bradley <jrb@turrettech.com>
 Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 ******************************************************************************/

#include "browser-app.hpp"
#include "browser-version.h"
#include <nlohmann/json.hpp>

#include <QApplication>
#include <QFileSystemWatcher>
#include <fstream>
#include <regex>
#include <util/platform.h>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef ENABLE_BROWSER_QT_LOOP
#include <util/base.h>
#include <util/platform.h>
#include <util/threading.h>
#include <QTimer>
#endif

#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(x) \
	{                   \
		(void)x;    \
	}
#endif

/* Returns the contents of the specified CSS file.
 * If the file does not exist, an empty string is returned.
 */
std::string BrowserApp::GetCSS(std::string id)
{
	std::string path = GetConfigPath("." + id + ".css");
	if (path == "" || !os_file_exists(path.c_str()))
		return "";
	std::ifstream fs(path);
	std::stringstream buf;
	buf << fs.rdbuf();
	return buf.str();
}

/* Returns the full config path for the specified relative path.
 */
std::string BrowserApp::GetConfigPath(std::string relpath)
{
	std::string path = "obs-studio";
	if (relpath != "")
		path += "/" + relpath;
	char cpath[512];
	int res = os_get_config_path(cpath, sizeof(cpath), path.c_str());
	return res > 0 ? std::string(cpath, res) : "";
}

static float ParseEnvScale(const char *name, float defaultValue = 1.0)
{
	const char *value = std::getenv(name);
	if (value) {
		try {
			return std::stof(value);
		} catch (std::invalid_argument &) {
		} catch (std::out_of_range &) {
		}
	}
	return defaultValue;
}

BrowserApp::~BrowserApp()
{
	if (cssWatcherThread) {
		cssWatcherThread->join();
		delete cssWatcherThread;
		cssWatcherThread = nullptr;
		taskRunner = nullptr;
	}
}

CefRefPtr<CefRenderProcessHandler> BrowserApp::GetRenderProcessHandler()
{
	return this;
}

CefRefPtr<CefBrowserProcessHandler> BrowserApp::GetBrowserProcessHandler()
{
	return this;
}

void BrowserApp::OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar)
{
	registrar->AddCustomScheme("http",
				   CEF_SCHEME_OPTION_STANDARD |
					   CEF_SCHEME_OPTION_CORS_ENABLED);
}

void BrowserApp::OnBeforeChildProcessLaunch(
	CefRefPtr<CefCommandLine> command_line)
{
#ifdef _WIN32
	std::string pid = std::to_string(GetCurrentProcessId());
	command_line->AppendSwitchWithValue("parent_pid", pid);
#else
	(void)command_line;
#endif
}

void BrowserApp::OnBeforeCommandLineProcessing(
	const CefString &, CefRefPtr<CefCommandLine> command_line)
{
#ifdef _WIN32
	// Windows handles per-monitor scaling by default
	// Don't override that unless the user sets OBS_BROWSER_DOCK_SCALE
	float defaultScale = -1;
#else
	// Other platforms don't handle scaling
	// Set the scale factor based on DOCK_SCALE * PIXEL_RATIO
	float defaultScale = 1.0;
#endif
	float scale = ParseEnvScale("OBS_BROWSER_DOCK_SCALE", defaultScale);
	if (scale > 0) {
		float ratio = ParseEnvScale("OBS_PRIMARY_PIXEL_RATIO", -1);
		if (ratio > 0) {
			scale *= ratio;
			command_line->AppendSwitchWithValue(
				"--force-device-scale-factor",
				std::to_string(scale).c_str());
		}
	}

	// If a switch wasn't explicity provided,
	// enable or disable acceleration according to the environment.
	if (!command_line->HasSwitch("enable-gpu") &&
	    !command_line->HasSwitch("disable-gpu")) {
		const char *s = getenv("OBS_BROWSER_ENABLE_GPU");
		bool b = s ? QVariant(s).toBool() : true;
		command_line->AppendSwitch(b ? "--enable-gpu"
					     : "--disable-gpu");
	}

	if (!shared_texture_available) {
		bool enableGPU = command_line->HasSwitch("enable-gpu");
		CefString type = command_line->GetSwitchValue("type");

		if (!enableGPU && type.empty()) {
			command_line->AppendSwitch("disable-gpu-compositing");
		}
	}

	if (command_line->HasSwitch("disable-features")) {
		// Don't override existing, as this can break OSR
		std::string disableFeatures =
			command_line->GetSwitchValue("disable-features");
		disableFeatures += ",HardwareMediaKeyHandling";
		disableFeatures += ",WebBluetooth";
		command_line->AppendSwitchWithValue("disable-features",
						    disableFeatures);
	} else {
		command_line->AppendSwitchWithValue("disable-features",
						    "WebBluetooth,"
						    "HardwareMediaKeyHandling");
	}

	command_line->AppendSwitchWithValue("autoplay-policy",
					    "no-user-gesture-required");
#ifdef __APPLE__
	command_line->AppendSwitch("use-mock-keychain");
#elif !defined(_WIN32)
	command_line->AppendSwitchWithValue("ozone-platform",
					    wayland ? "wayland" : "x11");
#endif
}

std::vector<std::string> exposedFunctions = {
	"getControlLevel",     "getCurrentScene",  "getStatus",
	"startRecording",      "stopRecording",    "startStreaming",
	"stopStreaming",       "pauseRecording",   "unpauseRecording",
	"startReplayBuffer",   "stopReplayBuffer", "saveReplayBuffer",
	"startVirtualcam",     "stopVirtualcam",   "getScenes",
	"setCurrentScene",     "getTransitions",   "getCurrentTransition",
	"setCurrentTransition"};

void BrowserApp::OnContextCreated(CefRefPtr<CefBrowser> browser,
				  CefRefPtr<CefFrame>,
				  CefRefPtr<CefV8Context> context)
{
	CefRefPtr<CefV8Value> globalObj = context->GetGlobal();

	CefRefPtr<CefV8Value> obsStudioObj =
		CefV8Value::CreateObject(nullptr, nullptr);
	globalObj->SetValue("obsstudio", obsStudioObj,
			    V8_PROPERTY_ATTRIBUTE_NONE);

	CefRefPtr<CefV8Value> pluginVersion =
		CefV8Value::CreateString(OBS_BROWSER_VERSION_STRING);
	obsStudioObj->SetValue("pluginVersion", pluginVersion,
			       V8_PROPERTY_ATTRIBUTE_NONE);

	for (std::string name : exposedFunctions) {
		CefRefPtr<CefV8Value> func =
			CefV8Value::CreateFunction(name, this);
		obsStudioObj->SetValue(name, func, V8_PROPERTY_ATTRIBUTE_NONE);
	}

	// Create functions for docks to retrieve service CSS and get notified of changes
	std::string name;
	CefRefPtr<CefV8Value> func;
	name = "getCSS";
	func = CefV8Value::CreateFunction(name, this);
	obsStudioObj->SetValue(name, func, V8_PROPERTY_ATTRIBUTE_NONE);
	name = "onCSSChanged";
	func = CefV8Value::CreateFunction(name, this);
	obsStudioObj->SetValue(name, func, V8_PROPERTY_ATTRIBUTE_NONE);

#if !ENABLE_WASHIDDEN
	int id = browser->GetIdentifier();
	if (browserVis.find(id) != browserVis.end()) {
		SetDocumentVisibility(browser, browserVis[id]);
	}
#else
	UNUSED_PARAMETER(browser);
#endif
}

void BrowserApp::OnContextReleased(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
				   CefRefPtr<CefV8Context> context)
{
	// Remove registered callbacks associated with this context
	if (cssCallbackMap.empty())
		return;
	CSSCallbackMap::iterator it = cssCallbackMap.begin();
	while (it != cssCallbackMap.end()) {
		if (it->second.first->IsSame(context))
			cssCallbackMap.erase(it);
		it++;
	}
}

void BrowserApp::ExecuteJSFunction(CefRefPtr<CefBrowser> browser,
				   const char *functionName,
				   CefV8ValueList arguments)
{
	std::vector<CefString> names;
	browser->GetFrameNames(names);
	for (auto &name : names) {
		CefRefPtr<CefFrame> frame =
#if CHROME_VERSION_BUILD >= 6261
			browser->GetFrameByName(name);
#else
			browser->GetFrame(name);
#endif
		CefRefPtr<CefV8Context> context = frame->GetV8Context();

		context->Enter();

		CefRefPtr<CefV8Value> globalObj = context->GetGlobal();
		CefRefPtr<CefV8Value> obsStudioObj =
			globalObj->GetValue("obsstudio");
		CefRefPtr<CefV8Value> jsFunction =
			obsStudioObj->GetValue(functionName);

		if (jsFunction && jsFunction->IsFunction())
			jsFunction->ExecuteFunction(nullptr, arguments);

		context->Exit();
	}
}

#if !ENABLE_WASHIDDEN
void BrowserApp::SetFrameDocumentVisibility(CefRefPtr<CefBrowser> browser,
					    CefRefPtr<CefFrame> frame,
					    bool isVisible)
{
	UNUSED_PARAMETER(browser);

	CefRefPtr<CefV8Context> context = frame->GetV8Context();

	context->Enter();

	CefRefPtr<CefV8Value> globalObj = context->GetGlobal();

	CefRefPtr<CefV8Value> documentObject = globalObj->GetValue("document");

	if (!!documentObject) {
		documentObject->SetValue("hidden",
					 CefV8Value::CreateBool(!isVisible),
					 V8_PROPERTY_ATTRIBUTE_READONLY);

		documentObject->SetValue(
			"visibilityState",
			CefV8Value::CreateString(isVisible ? "visible"
							   : "hidden"),
			V8_PROPERTY_ATTRIBUTE_READONLY);

		std::string script = "new CustomEvent('visibilitychange', {});";

		CefRefPtr<CefV8Value> returnValue;
		CefRefPtr<CefV8Exception> exception;

		/* Create the CustomEvent object
		 * We have to use eval to invoke the new operator */
		bool success = context->Eval(script, frame->GetURL(), 0,
					     returnValue, exception);

		if (success) {
			CefV8ValueList arguments;
			arguments.push_back(returnValue);

			CefRefPtr<CefV8Value> dispatchEvent =
				documentObject->GetValue("dispatchEvent");

			/* Dispatch visibilitychange event on the document
			 * object */
			dispatchEvent->ExecuteFunction(documentObject,
						       arguments);
		}
	}

	context->Exit();
}

void BrowserApp::SetDocumentVisibility(CefRefPtr<CefBrowser> browser,
				       bool isVisible)
{
	/* This method might be called before OnContextCreated
	 * call is made. We'll save the requested visibility
	 * state here, and use it later in OnContextCreated to
	 * set initial page visibility state. */
	browserVis[browser->GetIdentifier()] = isVisible;

	std::vector<int64> frameIdentifiers;
	/* Set visibility state for every frame in the browser
	 *
	 * According to the Page Visibility API documentation:
	 * https://developer.mozilla.org/en-US/docs/Web/API/Page_Visibility_API
	 *
	 * "Visibility states of an <iframe> are the same as
	 * the parent document. Hiding an <iframe> using CSS
	 * properties (such as display: none;) doesn't trigger
	 * visibility events or change the state of the document
	 * contained within the frame."
	 *
	 * Thus, we set the same visibility state for every frame of the browser.
	 */
	browser->GetFrameIdentifiers(frameIdentifiers);

	for (int64 frameId : frameIdentifiers) {
		CefRefPtr<CefFrame> frame = browser->GetFrame(frameId);

		SetFrameDocumentVisibility(browser, frame, isVisible);
	}
}
#endif

CefRefPtr<CefV8Value> CefValueToCefV8Value(CefRefPtr<CefValue> value)
{
	CefRefPtr<CefV8Value> result;
	switch (value->GetType()) {
	case VTYPE_INVALID:
		result = CefV8Value::CreateNull();
		break;
	case VTYPE_NULL:
		result = CefV8Value::CreateNull();
		break;
	case VTYPE_BOOL:
		result = CefV8Value::CreateBool(value->GetBool());
		break;
	case VTYPE_INT:
		result = CefV8Value::CreateInt(value->GetInt());
		break;
	case VTYPE_DOUBLE:
		result = CefV8Value::CreateDouble(value->GetDouble());
		break;
	case VTYPE_STRING:
		result = CefV8Value::CreateString(value->GetString());
		break;
	case VTYPE_BINARY:
		result = CefV8Value::CreateNull();
		break;
	case VTYPE_DICTIONARY: {
		result = CefV8Value::CreateObject(nullptr, nullptr);
		CefRefPtr<CefDictionaryValue> dict = value->GetDictionary();
		CefDictionaryValue::KeyList keys;
		dict->GetKeys(keys);
		for (unsigned int i = 0; i < keys.size(); i++) {
			CefString key = keys[i];
			result->SetValue(
				key, CefValueToCefV8Value(dict->GetValue(key)),
				V8_PROPERTY_ATTRIBUTE_NONE);
		}
	} break;
	case VTYPE_LIST: {
		CefRefPtr<CefListValue> list = value->GetList();
		size_t size = list->GetSize();
		result = CefV8Value::CreateArray((int)size);
		for (size_t i = 0; i < size; i++) {
			result->SetValue((int)i, CefValueToCefV8Value(
							 list->GetValue(i)));
		}
	} break;
	}
	return result;
}

bool BrowserApp::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
					  CefRefPtr<CefFrame> frame,
					  CefProcessId source_process,
					  CefRefPtr<CefProcessMessage> message)
{
	UNUSED_PARAMETER(frame);
	DCHECK(source_process == PID_BROWSER);

	CefRefPtr<CefListValue> args = message->GetArgumentList();

	if (message->GetName() == "Visibility") {
		CefV8ValueList arguments;
		arguments.push_back(CefV8Value::CreateBool(args->GetBool(0)));

		ExecuteJSFunction(browser, "onVisibilityChange", arguments);

#if !ENABLE_WASHIDDEN
		SetDocumentVisibility(browser, args->GetBool(0));
#endif

	} else if (message->GetName() == "Active") {
		CefV8ValueList arguments;
		arguments.push_back(CefV8Value::CreateBool(args->GetBool(0)));

		ExecuteJSFunction(browser, "onActiveChange", arguments);

	} else if (message->GetName() == "DispatchJSEvent") {
		nlohmann::json payloadJson = nlohmann::json::parse(
			args->GetString(1).ToString(), nullptr, false);

		nlohmann::json wrapperJson;
		if (args->GetSize() > 1)
			wrapperJson["detail"] = payloadJson;
		std::string wrapperJsonString = wrapperJson.dump();
		std::string script;

		script += "new CustomEvent('";
		script += args->GetString(0).ToString();
		script += "', ";
		script += wrapperJsonString;
		script += ");";

		std::vector<CefString> names;
		browser->GetFrameNames(names);
		for (auto &name : names) {
			CefRefPtr<CefFrame> frame =
#if CHROME_VERSION_BUILD >= 6261
				browser->GetFrameByName(name);
#else
				browser->GetFrame(name);
#endif
			CefRefPtr<CefV8Context> context = frame->GetV8Context();

			context->Enter();

			CefRefPtr<CefV8Value> globalObj = context->GetGlobal();

			CefRefPtr<CefV8Value> returnValue;
			CefRefPtr<CefV8Exception> exception;

			/* Create the CustomEvent object
			* We have to use eval to invoke the new operator */
			context->Eval(script, browser->GetMainFrame()->GetURL(),
				      0, returnValue, exception);

			CefV8ValueList arguments;
			arguments.push_back(returnValue);

			CefRefPtr<CefV8Value> dispatchEvent =
				globalObj->GetValue("dispatchEvent");
			dispatchEvent->ExecuteFunction(nullptr, arguments);

			context->Exit();
		}

	} else if (message->GetName() == "executeCallback") {
		CefRefPtr<CefV8Context> context =
			browser->GetMainFrame()->GetV8Context();

		context->Enter();

		CefRefPtr<CefListValue> arguments = message->GetArgumentList();
		int callbackID = arguments->GetInt(0);
		CefString jsonString = arguments->GetString(1);

		CefRefPtr<CefValue> json =
			CefParseJSON(arguments->GetString(1).ToString(), {});

		CefRefPtr<CefV8Value> callback = callbackMap[callbackID];
		CefV8ValueList args;

		CefRefPtr<CefV8Value> retval = CefValueToCefV8Value(json);

		args.push_back(retval);

		if (callback)
			callback->ExecuteFunction(nullptr, args);

		context->Exit();

		callbackMap.erase(callbackID);

	} else {
		return false;
	}

	return true;
}

bool IsValidFunction(std::string function)
{
	std::vector<std::string>::iterator iterator;
	iterator = std::find(exposedFunctions.begin(), exposedFunctions.end(),
			     function);
	return iterator != exposedFunctions.end();
}

bool BrowserApp::Execute(const CefString &name, CefRefPtr<CefV8Value>,
			 const CefV8ValueList &arguments,
			 CefRefPtr<CefV8Value> &retval, CefString &)
{
	if (name == "getCSS") {
		// Returns the specified service CSS to the dock
		if (arguments.size() == 1 && arguments[0]->IsString()) {
			std::string id = arguments[0]->GetStringValue();
			std::string css = GetCSS(id);
			retval = CefV8Value::CreateString(css);
			return true;
		}
		return false;
	}

	if (name == "onCSSChanged") {
		// A dock requested updates to a service CSS file
		if (arguments.size() == 2 && arguments[0]->IsString() &&
		    arguments[1]->IsFunction()) {
			std::string id = arguments[0]->GetStringValue();
			auto func = arguments[1];

			// Save the reference to this callback/context
			CefRefPtr<CefV8Context> context =
				CefV8Context::GetCurrentContext();
			int browserID = context->GetBrowser()->GetIdentifier();
			cssCallbackMap.insert(
				std::make_pair(std::make_pair(id, browserID),
					       std::make_pair(context, func)));

			if (!cssWatcherThread) {
				// Start the watcher thread to process future changes
				taskRunner = CefTaskRunner::GetForThread(
					TID_RENDERER);
				cssWatcherThread = new std::thread(
					&BrowserApp::WatchCSS, this);
			}
			return true;
		}
		return false;
	}

	if (IsValidFunction(name.ToString())) {
		if (arguments.size() >= 1 && arguments[0]->IsFunction()) {
			callbackId++;
			callbackMap[callbackId] = arguments[0];
		}

		CefRefPtr<CefProcessMessage> msg =
			CefProcessMessage::Create(name);
		CefRefPtr<CefListValue> args = msg->GetArgumentList();
		args->SetInt(0, callbackId);

		/* Pass on arguments */
		for (u_long l = 0; l < arguments.size(); l++) {
			u_long pos;
			if (arguments[0]->IsFunction())
				pos = l;
			else
				pos = l + 1;

			if (arguments[l]->IsString())
				args->SetString(pos,
						arguments[l]->GetStringValue());
			else if (arguments[l]->IsInt())
				args->SetInt(pos, arguments[l]->GetIntValue());
			else if (arguments[l]->IsBool())
				args->SetBool(pos,
					      arguments[l]->GetBoolValue());
			else if (arguments[l]->IsDouble())
				args->SetDouble(pos,
						arguments[l]->GetDoubleValue());
		}

		CefRefPtr<CefBrowser> browser =
			CefV8Context::GetCurrentContext()->GetBrowser();
		SendBrowserProcessMessage(browser, PID_BROWSER, msg);

	} else {
		/* Function does not exist. */
		return false;
	}

	return true;
}

/* Sends updated service CSS to any interested callbacks.
 */
void BrowserApp::SendCSSChanged(std::string id)
{
	// cout << "CSS CHANGED " << id << endl;

	if (cssCallbackMap.empty())
		return;

	for (CSSCallbackMap::iterator it = cssCallbackMap.begin();
	     it != cssCallbackMap.end(); ++it) {
		if (it->first.first != id)
			continue;

		CefV8ValueList arguments;
		std::string css = GetCSS(id);
		arguments.push_back(CefV8Value::CreateString(css));

		CefRefPtr<CefV8Context> context = it->second.first;
		CefRefPtr<CefV8Value> callback = it->second.second;
		context->Enter();
		callback->ExecuteFunction(nullptr, arguments);
		context->Exit();
	}
}

/* Uses a file system watcher to react to changes in service CSS files.
 */
void BrowserApp::WatchCSS()
{
	// Used to post tasks on the renderer thread
	class TaskImpl : public CefTask {
		std::function<void()> fn;

	public:
		TaskImpl(std::function<void()> fn_) : fn(fn_) {}
		virtual void Execute() override { fn(); }
		IMPLEMENT_REFCOUNTING(TaskImpl);
	};

	// Create the Application for the event loop
	int argc = 1;
	char *argv[] = {(char *)"WatchCSS"};
	QApplication app(argc, argv);

	// Create a file system watcher (for just the config directory)
	std::string configPath = GetConfigPath();
	QFileSystemWatcher watcher;
	watcher.addPath(QString::fromStdString(configPath));

	// We only care about files named .<ID>.css in the config directory
	std::string sep(&std::filesystem::path::preferred_separator);
	std::regex cssPathPattern("^.*" + sep + "\\.(\\w+)\\.css$");

	// Called when a CSS file changes
	auto OnFileChanged = [&](const QString &qPath) {
		// Get the ID from the file path using regex
		std::string path = qPath.toStdString();
		std::smatch match;
		std::regex_match(path, match, cssPathPattern);
		std::string id = match[1];
		// cout << "FILE CHANGED " << id << endl;

		// From the renderer thread, notify the JavaScript callbacks
		auto fn = std::bind(&BrowserApp::SendCSSChanged, this, id);
		taskRunner->PostTask(CefRefPtr<TaskImpl>(new TaskImpl(fn)));
	};

	// Registers for updates to any of the service CSS files we care about
	auto UpdateFiles = [&](bool callFileChanged = true) {
		for (const auto &entry :
		     std::filesystem::directory_iterator(configPath)) {
			// Make sure it's a regular file that matches the regex pattern
			if (!entry.is_regular_file())
				continue;
			std::string path = entry.path().string();
			if (!regex_match(path, cssPathPattern))
				continue;

			QString qPath = QString::fromStdString(path);
			if (watcher.files().contains(qPath))
				// We're already registered
				continue;

			// Register for future updates and send off callbacks
			watcher.addPath(qPath);
			if (callFileChanged)
				OnFileChanged(qPath);
		}
	};

	// Called when a file is deleted or removed under our config directory
	auto OnDirectoryChanged = [&](const QString &) {
		// cout << "DIRECTORY CHANGED " << path.toStdString() << endl;
		UpdateFiles();
	};

	UpdateFiles(false);
	QObject::connect(&watcher, &QFileSystemWatcher::directoryChanged,
			 OnDirectoryChanged);
	QObject::connect(&watcher, &QFileSystemWatcher::fileChanged,
			 OnFileChanged);

	while (cssWatcherThread) {
		// We don't have to process events that often
		os_sleep_ms(1000);
		app.processEvents();
	}
}

#ifdef ENABLE_BROWSER_QT_LOOP
Q_DECLARE_METATYPE(MessageTask);
MessageObject messageObject;

void QueueBrowserTask(CefRefPtr<CefBrowser> browser, BrowserFunc func)
{
	std::lock_guard<std::mutex> lock(messageObject.browserTaskMutex);
	messageObject.browserTasks.emplace_back(browser, func);

	QMetaObject::invokeMethod(&messageObject, "ExecuteNextBrowserTask",
				  Qt::QueuedConnection);
}

bool MessageObject::ExecuteNextBrowserTask()
{
	Task nextTask;
	{
		std::lock_guard<std::mutex> lock(browserTaskMutex);
		if (!browserTasks.size())
			return false;

		nextTask = browserTasks[0];
		browserTasks.pop_front();
	}

	nextTask.func(nextTask.browser);
	return true;
}

void MessageObject::ExecuteTask(MessageTask task)
{
	task();
}

void MessageObject::DoCefMessageLoop(int ms)
{
	if (ms)
		QTimer::singleShot((int)ms + 2,
				   []() { CefDoMessageLoopWork(); });
	else
		CefDoMessageLoopWork();
}

void MessageObject::Process()
{
	CefDoMessageLoopWork();
}

void ProcessCef()
{
	QMetaObject::invokeMethod(&messageObject, "DoCefMessageLoop",
				  Qt::QueuedConnection, Q_ARG(int, (int)0));
}

#define MAX_DELAY (1000 / 30)

#if CHROME_VERSION_BUILD < 5938
void BrowserApp::OnScheduleMessagePumpWork(int64 delay_ms)
#else
void BrowserApp::OnScheduleMessagePumpWork(int64_t delay_ms)
#endif
{
	if (delay_ms < 0)
		delay_ms = 0;
	else if (delay_ms > MAX_DELAY)
		delay_ms = MAX_DELAY;

	if (!frameTimer.isActive()) {
		QObject::connect(&frameTimer, &QTimer::timeout, &messageObject,
				 &MessageObject::Process);
		frameTimer.setSingleShot(false);
		frameTimer.start(33);
	}

	QMetaObject::invokeMethod(&messageObject, "DoCefMessageLoop",
				  Qt::QueuedConnection,
				  Q_ARG(int, (int)delay_ms));
}
#endif
