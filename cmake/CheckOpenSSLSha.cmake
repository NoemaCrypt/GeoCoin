set(_allowlist
    "${CMAKE_SOURCE_DIR}/src/geocoin/consensus/optimized_miner.cpp"
)

file(GLOB_RECURSE _sources
    "${CMAKE_SOURCE_DIR}/src/*.c"
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/include/*.h"
    "${CMAKE_SOURCE_DIR}/include/*.hpp"
)

foreach(_f IN LISTS _sources)
    list(FIND _allowlist "${_f}" _idx)
    if (_idx GREATER -1)
        continue()
    endif()
    file(READ "${_f}" _content)
    string(FIND "${_content}" "#include <openssl/sha.h>" _pos)
    if (NOT _pos EQUAL -1)
        message(FATAL_ERROR
            "OpenSSL SHA include is запрещён вне майнера. "
            "Найдена строка в файле: ${_f}. "
            "Разрешено только: src/geocoin/consensus/optimized_miner.cpp"
        )
    endif()
endforeach()
