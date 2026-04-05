### Generic native Skirmish AI config
#

include(FetchContent)

# Fetch nlohmann/json
FetchContent_Declare(
  json
  URL https://github.com/nlohmann/json/releases/download/v3.11.2/json.tar.xz
)
FetchContent_MakeAvailable(json)

# Fetch cpp-httplib
FetchContent_Declare(
  httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
	GIT_TAG        v0.41.0
)
FetchContent_MakeAvailable(httplib)

set(mySourceDirRel         "src") # Common values are "" or "src"
set(additionalSources      "")
set(additionalCompileFlags "")
set(additionalLibraries    ${Cpp_AIWRAPPER_TARGET} CUtils nlohmann_json::nlohmann_json)

if(WIN32)
	list(APPEND additionalLibraries ws2_32)
endif()

if    (BUILD_Cpp_AIWRAPPER)
	include_directories(BEFORE ${Cpp_AIWRAPPER_INCLUDE_DIRS})
	
	# Add the fetched headers to include path
	get_target_property(JSON_INCLUDE_DIR nlohmann_json::nlohmann_json INTERFACE_INCLUDE_DIRECTORIES)
	include_directories(SYSTEM ${JSON_INCLUDE_DIR})
	
	# httplib is a bit different depending on version, but usually provides a target
	if(TARGET httplib::httplib)
		get_target_property(HTTPLIB_INCLUDE_DIR httplib::httplib INTERFACE_INCLUDE_DIRECTORIES)
		include_directories(SYSTEM ${HTTPLIB_INCLUDE_DIR})
	endif()

	configure_native_skirmish_ai(mySourceDirRel additionalSources additionalCompileFlags additionalLibraries)
else  (BUILD_Cpp_AIWRAPPER)
	message ("warning: (New) C++ Controller AI will not be built! (missing Cpp Wrapper)")
endif (BUILD_Cpp_AIWRAPPER)
