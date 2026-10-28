# CMake Windows defaults module

include_guard(GLOBAL)

# Enable find_package targets to become globally available targets
set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL TRUE)

include(buildspec)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  # $ENV{ALLUSERSPROFILE} is usually a native path such as C:\ProgramData.
  # A backslash written into a cached path would end up unescaped in the
  # generated cmake_install.cmake ("\P" is not a valid escape), so convert it
  # to a CMake-style path first.
  file(TO_CMAKE_PATH "$ENV{ALLUSERSPROFILE}/obs-studio/plugins" _obs_plugin_install_prefix)
  set(
    CMAKE_INSTALL_PREFIX
    "${_obs_plugin_install_prefix}"
    CACHE STRING
    "Default plugin installation directory"
    FORCE
  )
  unset(_obs_plugin_install_prefix)
endif()
