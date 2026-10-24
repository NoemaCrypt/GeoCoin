if(BUILD_QT_GUI)
    find_program(XVFB_RUN_EXECUTABLE xvfb-run)
    if(XVFB_RUN_EXECUTABLE)
        add_test(
            NAME test_geocoin_qt_smoke
            COMMAND ${XVFB_RUN_EXECUTABLE} -a
                    $<TARGET_FILE:geocoin-qt> -platform xcb --smoke-test -no-auto-connect
        )
    else()
        add_test(
            NAME test_geocoin_qt_smoke
            COMMAND ${CMAKE_COMMAND} -E env QT_QPA_PLATFORM=offscreen
                    $<TARGET_FILE:geocoin-qt> -platform offscreen --smoke-test -no-auto-connect
        )
    endif()
    set_tests_properties(test_geocoin_qt_smoke PROPERTIES TIMEOUT 60)
endif()

find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)
foreach(_architecture_script scripts/check_architecture.py tests/test_architecture_checker.py)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_architecture_script}")
        message(FATAL_ERROR "Required architecture check missing: ${_architecture_script}")
    endif()
endforeach()
add_test(NAME test_architecture_boundaries COMMAND ${Python3_EXECUTABLE}
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_architecture.py" --root "${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME test_architecture_checker COMMAND ${Python3_EXECUTABLE}
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_architecture_checker.py")
set_tests_properties(test_architecture_boundaries test_architecture_checker PROPERTIES LABELS "architecture;unit")

add_test(NAME test_geographiclib_build COMMAND ${Python3_EXECUTABLE}
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_geographiclib_build.py")
set_tests_properties(test_geographiclib_build PROPERTIES LABELS "geography;build;unit" TIMEOUT 60)
add_test(NAME test_geodesy_fingerprint COMMAND ${Python3_EXECUTABLE}
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_geo_fingerprints.py"
    --binary $<TARGET_FILE:test_geodesy> --platform local-${CMAKE_CXX_COMPILER_ID}-${CMAKE_SYSTEM_PROCESSOR}
    --output "${CMAKE_CURRENT_BINARY_DIR}/geographic-fingerprint.json")
set_tests_properties(test_geodesy_fingerprint PROPERTIES LABELS "consensus;geography" TIMEOUT 120)

add_test(NAME test_release_source COMMAND ${Python3_EXECUTABLE}
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_release_source.py")
add_test(NAME test_documentation_links COMMAND ${Python3_EXECUTABLE}
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_doc_links.py")


if(TARGET geocoin-node AND Python3_Interpreter_FOUND)
    add_test(
        NAME test_rpc_localnet_fullnode_smoke
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/rpc_smoke.py"
                --node-bin $<TARGET_FILE:geocoin-node>
                --network degree-test
                --min-height 0
                --sync-timeout 45
                --no-default-addnodes
    )
    set_tests_properties(test_rpc_localnet_fullnode_smoke PROPERTIES
        TIMEOUT 90
        LABELS "integration;rpc;fullnode;rocksdb"
    )

    foreach(_geocoin_p2p_smoke_nodes 2 3 4)
        add_test(
            NAME test_p2p_cluster_${_geocoin_p2p_smoke_nodes}_nodes_smoke
            COMMAND ${Python3_EXECUTABLE}
                    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/p2p_cluster_smoke.py"
                    --node-binary $<TARGET_FILE:geocoin-node>
                    --activation-manifest "${CMAKE_CURRENT_SOURCE_DIR}/config/activation-manifest.testing-testnet.json"
                    --network degree-test
                    --nodes ${_geocoin_p2p_smoke_nodes}
                    --timeout 90
        )
        set_tests_properties(test_p2p_cluster_${_geocoin_p2p_smoke_nodes}_nodes_smoke PROPERTIES
            TIMEOUT 150
            LABELS "integration;p2p;cluster;rocksdb"
        )
    endforeach()
endif()

if(GEOCOIN_ENABLE_GPU_LOCALNET_TESTS AND TARGET geocoin-node AND TARGET geocoin-gpu-miner AND Python3_Interpreter_FOUND)
    add_test(
        NAME test_nft_localnet_2_nodes_smoke
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/nft_node_smoke.py"
                --node-binary $<TARGET_FILE:geocoin-node>
                --miner-binary $<TARGET_FILE:geocoin-gpu-miner>
                --miner-kind gpu
                --activation-manifest "${CMAKE_CURRENT_SOURCE_DIR}/config/activation-manifest.testing-testnet.json"
                --network degree-test
                --timeout 180
                --gpu-intensity 16777216
                --gpu-max-launch-attempts 16777216
    )
    set_tests_properties(test_nft_localnet_2_nodes_smoke PROPERTIES
        TIMEOUT 300
        LABELS "integration;rpc;p2p;nft;rocksdb;gpu"
        ENVIRONMENT "GEOCOIN_RPC_RATE_LIMIT_MAX_REQUESTS_PER_WINDOW=10000"
    )
endif()

if(GEOCOIN_ENABLE_CPU_NFT_LOCALNET_TESTS AND TARGET geocoin-node AND TARGET geocoin-rpc-miner AND Python3_Interpreter_FOUND)
    add_test(
        NAME test_nft_localnet_2_nodes_smoke_cpu
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/nft_node_smoke.py"
                --node-binary $<TARGET_FILE:geocoin-node>
                --miner-binary $<TARGET_FILE:geocoin-rpc-miner>
                --miner-kind rpc-cpu
                --activation-manifest "${CMAKE_CURRENT_SOURCE_DIR}/config/activation-manifest.testing-testnet.json"
                --network degree-test
                --timeout 180
                --mining-threads 1
    )
    set_tests_properties(test_nft_localnet_2_nodes_smoke_cpu PROPERTIES
        TIMEOUT 300
        LABELS "integration;rpc;p2p;nft;rocksdb;cpu"
        ENVIRONMENT "GEOCOIN_RPC_RATE_LIMIT_MAX_REQUESTS_PER_WINDOW=10000"
    )
endif()

if(GEOCOIN_ENABLE_CPU_SIDECHAIN_LOCALNET_TESTS AND TARGET geocoin-node AND TARGET geocoin-rpc-miner AND TARGET sidechain_test_tool AND Python3_Interpreter_FOUND)
    add_test(
        NAME test_sidechain_v3_localnet
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/sidechain_v3_localnet_test.py"
                --node-binary $<TARGET_FILE:geocoin-node>
                --miner-binary $<TARGET_FILE:geocoin-rpc-miner>
                --signing-tool $<TARGET_FILE:sidechain_test_tool>
                --activation-manifest "${CMAKE_CURRENT_SOURCE_DIR}/config/activation-manifest.testing-testnet.json"
                --network degree-test
                --timeout 180
                --mining-threads 1
    )
    set_tests_properties(test_sidechain_v3_localnet PROPERTIES
        TIMEOUT 1200
        LABELS "integration;rpc;p2p;sidechain;rocksdb;reorg;cpu"
        ENVIRONMENT "GEOCOIN_RPC_RATE_LIMIT_MAX_REQUESTS_PER_WINDOW=10000"
    )
endif()

if(GEOCOIN_ENABLE_GPU_LOCALNET_TESTS AND TARGET geocoin-node AND TARGET multisig_package_builder AND TARGET geocoin-gpu-miner AND Python3_Interpreter_FOUND)
    add_test(
        NAME test_multisig_localnet_smoke
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/multisig_localnet_smoke.py"
                --node-binary $<TARGET_FILE:geocoin-node>
                --miner-binary $<TARGET_FILE:geocoin-gpu-miner>
                --miner-kind gpu
                --package-builder $<TARGET_FILE:multisig_package_builder>
                --activation-manifest "${CMAKE_CURRENT_SOURCE_DIR}/config/activation-manifest.testing-testnet.json"
                --network degree-test
                --timeout 180
                --gpu-intensity 16777216
                --gpu-max-launch-attempts 16777216
                --multisig-m 2
                --multisig-n 3
    )
    set_tests_properties(test_multisig_localnet_smoke PROPERTIES
        TIMEOUT 300
        LABELS "integration;rpc;p2p;multisig;rocksdb;gpu"
        ENVIRONMENT "GEOCOIN_RPC_RATE_LIMIT_MAX_REQUESTS_PER_WINDOW=10000"
    )

    add_test(
        NAME test_multisig_localnet_3_of_5_smoke
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/multisig_localnet_smoke.py"
                --node-binary $<TARGET_FILE:geocoin-node>
                --miner-binary $<TARGET_FILE:geocoin-gpu-miner>
                --miner-kind gpu
                --package-builder $<TARGET_FILE:multisig_package_builder>
                --activation-manifest "${CMAKE_CURRENT_SOURCE_DIR}/config/activation-manifest.testing-testnet.json"
                --network degree-test
                --timeout 180
                --gpu-intensity 16777216
                --gpu-max-launch-attempts 16777216
                --multisig-m 3
                --multisig-n 5
    )
    set_tests_properties(test_multisig_localnet_3_of_5_smoke PROPERTIES
        TIMEOUT 300
        LABELS "integration;rpc;p2p;multisig;rocksdb;gpu"
        ENVIRONMENT "GEOCOIN_RPC_RATE_LIMIT_MAX_REQUESTS_PER_WINDOW=10000"
    )

    add_test(
        NAME test_multisig_localnet_5_of_5_smoke
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/multisig_localnet_smoke.py"
                --node-binary $<TARGET_FILE:geocoin-node>
                --miner-binary $<TARGET_FILE:geocoin-gpu-miner>
                --miner-kind gpu
                --package-builder $<TARGET_FILE:multisig_package_builder>
                --activation-manifest "${CMAKE_CURRENT_SOURCE_DIR}/config/activation-manifest.testing-testnet.json"
                --network degree-test
                --timeout 180
                --gpu-intensity 16777216
                --gpu-max-launch-attempts 16777216
                --multisig-m 5
                --multisig-n 5
    )
    set_tests_properties(test_multisig_localnet_5_of_5_smoke PROPERTIES
        TIMEOUT 300
        LABELS "integration;rpc;p2p;multisig;rocksdb;gpu"
        ENVIRONMENT "GEOCOIN_RPC_RATE_LIMIT_MAX_REQUESTS_PER_WINDOW=10000"
    )

    add_test(
        NAME test_multisig_localnet_10_of_10_smoke
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/multisig_localnet_smoke.py"
                --node-binary $<TARGET_FILE:geocoin-node>
                --miner-binary $<TARGET_FILE:geocoin-gpu-miner>
                --miner-kind gpu
                --package-builder $<TARGET_FILE:multisig_package_builder>
                --activation-manifest "${CMAKE_CURRENT_SOURCE_DIR}/config/activation-manifest.testing-testnet.json"
                --network degree-test
                --timeout 180
                --gpu-intensity 16777216
                --gpu-max-launch-attempts 16777216
                --multisig-m 10
                --multisig-n 10
    )
    set_tests_properties(test_multisig_localnet_10_of_10_smoke PROPERTIES
        TIMEOUT 300
        LABELS "integration;rpc;p2p;multisig;rocksdb;gpu"
        ENVIRONMENT "GEOCOIN_RPC_RATE_LIMIT_MAX_REQUESTS_PER_WINDOW=10000"
    )

    add_test(
        NAME test_delegation_localnet_5_of_5_smoke
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/delegation_localnet_smoke.py"
                --node-binary $<TARGET_FILE:geocoin-node>
                --miner-binary $<TARGET_FILE:geocoin-gpu-miner>
                --miner-kind gpu
                --package-builder $<TARGET_FILE:multisig_package_builder>
                --activation-manifest "${CMAKE_CURRENT_SOURCE_DIR}/config/activation-manifest.testing-testnet.json"
                --network degree-test
                --timeout 240
                --gpu-intensity 16777216
                --gpu-max-launch-attempts 16777216
                --primary-m 5
                --primary-n 5
                --delegated-m 2
                --delegated-n 3
    )
    set_tests_properties(test_delegation_localnet_5_of_5_smoke PROPERTIES
        TIMEOUT 420
        LABELS "integration;rpc;p2p;multisig;delegation;rocksdb;gpu"
        ENVIRONMENT "GEOCOIN_RPC_RATE_LIMIT_MAX_REQUESTS_PER_WINDOW=10000"
    )
endif()

if(GEOCOIN_ENABLE_CPU_MULTISIG_LOCALNET_TESTS AND TARGET geocoin-node AND TARGET geocoin-rpc-miner AND TARGET multisig_package_builder AND Python3_Interpreter_FOUND)
    add_test(
        NAME test_multisig_localnet_smoke_cpu
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/multisig_localnet_smoke.py"
                --node-binary $<TARGET_FILE:geocoin-node>
                --miner-binary $<TARGET_FILE:geocoin-rpc-miner>
                --miner-kind rpc-cpu
                --package-builder $<TARGET_FILE:multisig_package_builder>
                --activation-manifest "${CMAKE_CURRENT_SOURCE_DIR}/config/activation-manifest.testing-testnet.json"
                --network degree-test
                --timeout 180
                --mining-threads 1
                --multisig-m 2
                --multisig-n 3
    )
    set_tests_properties(test_multisig_localnet_smoke_cpu PROPERTIES
        TIMEOUT 300
        LABELS "integration;rpc;p2p;multisig;rocksdb;cpu"
        ENVIRONMENT "GEOCOIN_RPC_RATE_LIMIT_MAX_REQUESTS_PER_WINDOW=10000"
    )
    add_test(
        NAME test_delegation_localnet_smoke_cpu
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/delegation_localnet_smoke.py"
                --node-binary $<TARGET_FILE:geocoin-node>
                --miner-binary $<TARGET_FILE:geocoin-rpc-miner>
                --miner-kind rpc-cpu
                --package-builder $<TARGET_FILE:multisig_package_builder>
                --activation-manifest "${CMAKE_CURRENT_SOURCE_DIR}/config/activation-manifest.testing-loadnet.json"
                --network degree-load
                --timeout 180
                --mining-threads 1
                --primary-m 5 --primary-n 5
                --delegated-m 2 --delegated-n 3
    )
    set_tests_properties(test_delegation_localnet_smoke_cpu PROPERTIES
        TIMEOUT 300
        LABELS "integration;rpc;p2p;multisig;delegation;rocksdb;cpu"
        ENVIRONMENT "GEOCOIN_RPC_RATE_LIMIT_MAX_REQUESTS_PER_WINDOW=10000"
    )
endif()

if(GEOCOIN_ENABLE_RPC_INTEGRATION_TESTS)
    if(NOT TARGET geocoin-node)
        message(FATAL_ERROR "GEOCOIN_ENABLE_RPC_INTEGRATION_TESTS requires BUILD_NODE_CLI=ON")
    endif()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(
        NAME test_rpc_fullnode_smoke
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/rpc_smoke.py"
                --node-bin $<TARGET_FILE:geocoin-node>
                --data-dir "${CMAKE_CURRENT_BINARY_DIR}/rpc-smoke-data"
                --clean-data-dir
                --keep-data-dir
    )
    set_tests_properties(test_rpc_fullnode_smoke PROPERTIES
        TIMEOUT 240
        LABELS "integration;rpc;fullnode;rocksdb"
    )
endif()
