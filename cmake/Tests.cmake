include(CTest)
enable_testing()
find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)

include(cmake/tests/Headers.cmake)
include(cmake/tests/Unit.cmake)
include(cmake/tests/Integration.cmake)

function(geocoin_enable_test_debug_env)
    foreach(test_name IN LISTS ARGN)
        if(TEST ${test_name})
            set_tests_properties(${test_name} PROPERTIES
                ENVIRONMENT "GEOCOIN_SYNC_DEBUG=1;GEOCOIN_NODE_VERBOSE=1"
            )
        endif()
    endforeach()
endfunction()

geocoin_enable_test_debug_env(
    test_txid_contract
    test_config_and_activation
    test_rpc_bind_policy
    test_rpc_auth_roles
    test_rpc_http_security
    test_rpc_security_matrix
    test_rpc_units
    test_rpc_payload_handlers
    test_rpc_info_handlers
    test_wallet_chainstate_integration
    test_wallet_crypto_units
    test_wallet_lifecycle_units
    test_transaction_guard_units
    test_nft_validator_units
    test_state_serialization_units
    test_block_storage_units
    test_chainstate_revert_units
    test_blockchain_units
    test_multisig_verifier_units
    test_hashes
    test_auth_network_units
    test_headers_message
    test_sync_manager_units
    test_mempool_units
    test_parser_fuzz
    test_network_policy
    test_peer_policy
    test_u64_math
    test_difficulty_emergency
    test_mining_units
    test_benchmark_units
    test_node_status_utils
    test_node_mining_guards
    test_node_peer_messages_units
    test_node_connections_units
    test_node_connection_backoff
    test_node_config_loader
    test_all_transactions
    test_sidechain_restart_persistence
    test_b2b_corporate_policy
    test_storage_split_metadata
    test_p2p_cluster_2_nodes_smoke
    test_p2p_cluster_3_nodes_smoke
    test_p2p_cluster_4_nodes_smoke
    test_rpc_fullnode_smoke
)

# Tests may exercise implementation details deliberately, but production
# targets above do not receive the private source include directory.
get_property(_geocoin_test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_geocoin_test_target IN LISTS _geocoin_test_targets)
    if(_geocoin_test_target MATCHES "^test_")
        target_include_directories(${_geocoin_test_target} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
        )
    endif()
endforeach()
