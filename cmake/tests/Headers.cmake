# Compile each public header first in its own translation unit. A combined TU
# would hide missing includes such as uint16_t without <cstdint>.
file(GLOB_RECURSE _geocoin_public_headers CONFIGURE_DEPENDS
    RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/geocoin/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/geocoin/*.h")
set(_geocoin_header_sources "")
foreach(_header IN LISTS _geocoin_public_headers)
    string(MAKE_C_IDENTIFIER "${_header}" _header_id)
    set(_source "${CMAKE_CURRENT_BINARY_DIR}/header-checks/${_header_id}.cpp")
    file(GENERATE OUTPUT "${_source}" CONTENT "#include <${_header}>\n")
    list(APPEND _geocoin_header_sources "${_source}")
endforeach()
add_library(geocoin_public_header_checks OBJECT ${_geocoin_header_sources})
target_link_libraries(geocoin_public_header_checks PRIVATE geocoin_rpc geocoin_node geocoin_benchmark geocoin_b2b)
