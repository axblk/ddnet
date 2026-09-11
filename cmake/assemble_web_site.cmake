# Puts together the directory a web server has to serve, out of a build
# directory that holds a good deal more than that.
#
# Run with `cmake -P`, from the `web-site` target. Expects `BUILD_DIR`,
# `SITE_DIR`, `PROGRAMS` and `PAGE_FILES`.

if(NOT DEFINED BUILD_DIR OR NOT DEFINED SITE_DIR)
  message(FATAL_ERROR "BUILD_DIR and SITE_DIR must be given")
endif()

# The site is what the arguments say it is and nothing that was there before,
# so a file that stops being part of it stops being served.
file(REMOVE_RECURSE "${SITE_DIR}")
file(MAKE_DIRECTORY "${SITE_DIR}")

# What emscripten emits for one program. Not all of it exists: there is a
# `.data` only for a program that preloads files, and whether there is a
# separate worker script depends on the emscripten version.
foreach(program IN LISTS PROGRAMS)
  set(found FALSE)
  foreach(suffix ".js" ".wasm" ".data" ".worker.js")
    if(EXISTS "${BUILD_DIR}/${program}${suffix}")
      file(COPY "${BUILD_DIR}/${program}${suffix}" DESTINATION "${SITE_DIR}")
      set(found TRUE)
    endif()
  endforeach()
  if(NOT found)
    message(FATAL_ERROR "'${program}' was not built")
  endif()
endforeach()

foreach(page_file IN LISTS PAGE_FILES)
  if(NOT EXISTS "${BUILD_DIR}/${page_file}")
    message(FATAL_ERROR "'${page_file}' is missing from '${BUILD_DIR}'")
  endif()
  file(COPY "${BUILD_DIR}/${page_file}" DESTINATION "${SITE_DIR}")
endforeach()

# The data directory itself, which the client fetches a file at a time instead
# of being handed all of it before it starts.
if(NOT EXISTS "${BUILD_DIR}/data/index.txt")
  message(FATAL_ERROR "The data directory has no index; build `web_data_index`")
endif()
file(COPY "${BUILD_DIR}/data" DESTINATION "${SITE_DIR}")
