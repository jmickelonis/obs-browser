#pragma once

#define OBS_BROWSER_VERSION_MAJOR 2
#define OBS_BROWSER_VERSION_MINOR 25
#define OBS_BROWSER_VERSION_PATCH 3

#ifndef MAKE_SEMANTIC_VERSION
#define MAKE_SEMANTIC_VERSION(major, minor, patch) ((major << 24) | (minor << 16) | patch)
#endif

#define OBS_BROWSER_VERSION_INT \
	MAKE_SEMANTIC_VERSION(OBS_BROWSER_VERSION_MAJOR, OBS_BROWSER_VERSION_MINOR, OBS_BROWSER_VERSION_PATCH)

#define OBS_BROWSER_MACRO_STR_(x) #x
#define OBS_BROWSER_MACRO_STR(x) OBS_BROWSER_MACRO_STR_(x)

#define OBS_BROWSER_VERSION_STRING                       \
	OBS_BROWSER_MACRO_STR(OBS_BROWSER_VERSION_MAJOR) \
	"." OBS_BROWSER_MACRO_STR(OBS_BROWSER_VERSION_MINOR) "." OBS_BROWSER_MACRO_STR(OBS_BROWSER_VERSION_PATCH)
