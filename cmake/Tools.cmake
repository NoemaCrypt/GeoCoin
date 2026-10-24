# Tools
if(BUILD_TOOLS)
    add_executable(generate_geopow_grid EXCLUDE_FROM_ALL tools/generate_geopow_grid.cpp)
    target_link_libraries(generate_geopow_grid PRIVATE GeographicLib::GeographicLib)
    geocoin_strict_geodesy(generate_geopow_grid)
    add_executable(geocoin-db tools/geocoin_db.cpp)
    target_link_libraries(geocoin-db PRIVATE geocoin_state geocoin_storage geocoin_crypto)
    geocoin_stage_runtime_dependencies(geocoin-db)
    function(geocoin_add_executable_alias alias_name canonical_target)
        set(_alias_path "$<TARGET_FILE_DIR:${canonical_target}>/${alias_name}${CMAKE_EXECUTABLE_SUFFIX}")
        if(WIN32)
            add_custom_target(${alias_name} ALL
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${canonical_target}>" "${_alias_path}"
                DEPENDS ${canonical_target}
                VERBATIM
            )
        else()
            add_custom_target(${alias_name} ALL
                COMMAND ${CMAKE_COMMAND} -E rm -f "${_alias_path}"
                COMMAND ${CMAKE_COMMAND} -E create_symlink "$<TARGET_FILE_NAME:${canonical_target}>" "${_alias_path}"
                DEPENDS ${canonical_target}
                VERBATIM
            )
        endif()
    endfunction()

    if(BUILD_NODE_CLI)
        add_executable(geocoin-node tools/degree_node.cpp)
        target_link_libraries(geocoin-node geocoin_node geocoin_state geocoin_storage geocoin_rpc geocoin_crypto geocoin_benchmark)
        geocoin_stage_runtime_dependencies(geocoin-node)
        geocoin_add_executable_alias(degree_node geocoin-node)
    endif()

    if(BUILD_GENESIS_CLI OR BUILD_MINER_CLI)
        add_executable(genesis tools/mine_genesis.cpp)
        target_link_libraries(genesis geocoin_core geocoin_consensus geocoin_crypto)
        geocoin_stage_runtime_dependencies(genesis)
        if(BUILD_GENESIS_CLI)
            geocoin_add_executable_alias(mine_genesis genesis)
        endif()
    endif()

    if(BUILD_MINER_CLI)
        # Compatibility name for the same genesis/diagnostic miner implementation.
        geocoin_add_executable_alias(geocoin-miner genesis)

        add_executable(geocoin-rpc-miner tools/rpc_miner.cpp)
        target_link_libraries(geocoin-rpc-miner geocoin_crypto)
        geocoin_stage_runtime_dependencies(geocoin-rpc-miner)
    endif()

    if(BUILD_GPU_MINER)
        include(cmake/GpuMiner.cmake)
    endif()

    if(BUILD_MULTISIG_CLI)
        add_executable(multisig_cli tools/multisig_cli.cpp)
        target_link_libraries(multisig_cli geocoin_wallet geocoin_state geocoin_storage geocoin_core geocoin_crypto)
        geocoin_stage_runtime_dependencies(multisig_cli)
    endif()
endif()
