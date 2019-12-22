find_path(KQUEUE_INCLUDE_DIR sys/event.h)

#kqueue is inside libc on OpenBSD
#find_library(KQUEUE_LIBRARY NAMES c)

find_package_handle_standard_args(KQUEUE DEFAULT_MSG KQUEUE_INCLUDE_DIR)

if (KQUEUE_FOUND)
	set(KQUEUE_INCLUDE_DIRS ${KQUEUE_INCLUDE_DIR})
endif()

mark_as_advanced(KQUEUE_INCLUDE_DIR KQUEUE_LIBRARY)