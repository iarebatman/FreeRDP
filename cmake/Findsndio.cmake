find_path(SNDIO_INCLUDE_DIR sndio.h)

find_library(SNDIO_LIBRARY NAMES sndio)

find_package_handle_standard_args(SNDIO DEFAULT_MSG SNDIO_INCLUDE_DIR)

if (SNDIO_FOUND)
	set(SNDIO_INCLUDE_DIRS ${SNDIO_INCLUDE_DIR})
  set(SNDIO_LIBRARIES ${SNDIO_LIBRARY})
endif()

mark_as_advanced(SNDIO_INCLUDE_DIR SNDIO_LIBRARY)