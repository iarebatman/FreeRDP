include(FindPkgConfig)

if(PKG_CONFIG_FOUND)
	pkg_check_modules(PC_LIBEVENT QUIET libevent)
endif()

find_path(LIBEVENT_INCLUDE_DIR event2/event.h
	HINTS ${PC_LIBEVENT_INCLUDEDIR} ${PC_LIBEVENT_INCLUDE_DIRS})

find_library(LIBEVENT_LIBRARY NAMES event_core event_extra event_openssl event_pthreads
	HINTS ${PC_LIBEVENT_LIBDIR} ${PC_LIBEVENT_LIBRARY_DIRS})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LIBEVENT DEFAULT_MSG LIBEVENT_LIBRARY LIBEVENT_INCLUDE_DIR)

set(LIBEVENT_LIBRARIES ${LIBEVENT_LIBRARY})
set(LIBEVENT_INCLUDE_DIRS ${LIBEVENT_INCLUDE_DIR})

mark_as_advanced(LIBEVENT_INCLUDE_DIR LIBEVENT_LIBRARY)