#pragma once

#define MOD_NAME "Crimson Weather"
#define MOD_BASE_VERSION_MAJOR 0
#define MOD_BASE_VERSION_MINOR 7
#define MOD_BASE_VERSION_PATCH 2
#define MOD_BASE_VERSION_BUILD 0
#define MOD_BASE_VERSION "0.7.2"
#define MOD_ADDON_DESCRIPTION "Crimson Weather Addon"
#define MOD_COMPANY_NAME "Nosty"
#define MOD_INTERNAL_NAME "Crimson Weather"

#if defined(CW_WIND_ONLY)
#define MOD_DISPLAY_NAME "Crimson Weather (Wind only)"
#define MOD_CONFIG_FILE "CrimsonWeather.WindOnly.ini"
#define MOD_LOG_FILE "CrimsonWeather.WindOnly.log"
#define MOD_ORIGINAL_FILENAME "CrimsonWeather.WindOnly.addon64"
#elif defined(CW_DEV_BUILD)
#define MOD_DISPLAY_NAME MOD_NAME
#define MOD_CONFIG_FILE "CrimsonWeather.ini"
#define MOD_LOG_FILE "CrimsonWeather.log"
#define MOD_ORIGINAL_FILENAME "CrimsonWeather.DEV.addon64"
#else
#define MOD_DISPLAY_NAME MOD_NAME
#define MOD_CONFIG_FILE "CrimsonWeather.ini"
#define MOD_LOG_FILE "CrimsonWeather.log"
#define MOD_ORIGINAL_FILENAME "CrimsonWeather.addon64"
#endif

#if defined(CW_DEV_BUILD)
#define MOD_VERSION MOD_BASE_VERSION "-dev"
#else
#define MOD_VERSION MOD_BASE_VERSION
#endif

#define MOD_VERSION_RC MOD_BASE_VERSION_MAJOR,MOD_BASE_VERSION_MINOR,MOD_BASE_VERSION_PATCH,MOD_BASE_VERSION_BUILD
