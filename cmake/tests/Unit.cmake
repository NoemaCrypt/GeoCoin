add_executable(test_txid_contract tests/test_txid_contract.cpp)
target_link_libraries(test_txid_contract geocoin_core)
geocoin_stage_runtime_dependencies(test_txid_contract)
add_test(NAME test_txid_contract COMMAND test_txid_contract)

add_executable(test_config_and_activation tests/test_config_and_activation.cpp)
target_link_libraries(test_config_and_activation geocoin_core)
geocoin_stage_runtime_dependencies(test_config_and_activation)
add_test(NAME test_config_and_activation COMMAND test_config_and_activation)

add_executable(test_rpc_bind_policy tests/test_rpc_bind_policy.cpp)
target_link_libraries(test_rpc_bind_policy geocoin_core)
geocoin_stage_runtime_dependencies(test_rpc_bind_policy)
add_test(NAME test_rpc_bind_policy COMMAND test_rpc_bind_policy)

add_executable(test_rpc_auth_roles tests/test_rpc_auth_roles.cpp)
target_link_libraries(test_rpc_auth_roles geocoin_rpc geocoin_node geocoin_network)
geocoin_stage_runtime_dependencies(test_rpc_auth_roles)
add_test(NAME test_rpc_auth_roles COMMAND test_rpc_auth_roles)

add_executable(test_rpc_http_security tests/test_rpc_http_security.cpp)
target_link_libraries(test_rpc_http_security geocoin_rpc geocoin_node geocoin_network)
geocoin_stage_runtime_dependencies(test_rpc_http_security)
add_test(NAME test_rpc_http_security COMMAND test_rpc_http_security)

add_executable(test_rpc_security_matrix tests/test_rpc_security_matrix.cpp)
target_link_libraries(test_rpc_security_matrix geocoin_rpc geocoin_node geocoin_network)
target_compile_definitions(
    test_rpc_security_matrix
    PRIVATE GEOCOIN_ENABLE_RPC_SERVER_TEST_ACCESS
)
geocoin_stage_runtime_dependencies(test_rpc_security_matrix)
add_test(NAME test_rpc_security_matrix COMMAND test_rpc_security_matrix)

add_executable(test_rpc_units tests/test_rpc_units.cpp)
target_link_libraries(
    test_rpc_units
    geocoin_rpc
    geocoin_node
    geocoin_network
    geocoin_blockchain
    geocoin_state
    geocoin_storage
    geocoin_wallet
    geocoin_consensus
    geocoin_chain
    geocoin_core
    geocoin_crypto
)
target_compile_definitions(
    test_rpc_units
    PRIVATE
        GEOCOIN_ENABLE_CHAINSTATE_TEST_ACCESS
        GEOCOIN_ENABLE_RPC_SERVER_TEST_ACCESS
)
geocoin_stage_runtime_dependencies(test_rpc_units)
add_test(NAME test_rpc_units COMMAND test_rpc_units)

add_executable(test_rpc_contract tests/test_rpc_contract.cpp)
target_link_libraries(test_rpc_contract geocoin_rpc geocoin_node geocoin_network)
target_compile_definitions(test_rpc_contract PRIVATE
    GEOCOIN_ENABLE_RPC_SERVER_TEST_ACCESS GEOCOIN_ENABLE_BLOCKCHAIN_TEST_ACCESS)
geocoin_stage_runtime_dependencies(test_rpc_contract)
add_test(NAME test_rpc_contract COMMAND test_rpc_contract)

add_executable(test_rpc_payload_handlers tests/test_rpc_payload_handlers.cpp)
target_link_libraries(
    test_rpc_payload_handlers
    geocoin_rpc
    geocoin_node
    geocoin_network
    geocoin_blockchain
    geocoin_state
    geocoin_storage
    geocoin_wallet
    geocoin_consensus
    geocoin_chain
    geocoin_core
    geocoin_crypto
    Threads::Threads
)
geocoin_stage_runtime_dependencies(test_rpc_payload_handlers)
add_test(NAME test_rpc_payload_handlers COMMAND test_rpc_payload_handlers)

add_executable(test_rpc_info_handlers tests/test_rpc_info_handlers.cpp)
target_link_libraries(
    test_rpc_info_handlers
    geocoin_rpc
    geocoin_node
    geocoin_network
    geocoin_blockchain
    geocoin_state
    geocoin_storage
    geocoin_wallet
    geocoin_consensus
    geocoin_chain
    geocoin_core
    geocoin_crypto
    Threads::Threads
)
geocoin_stage_runtime_dependencies(test_rpc_info_handlers)
add_test(NAME test_rpc_info_handlers COMMAND test_rpc_info_handlers)

add_executable(test_wallet_chainstate_integration tests/test_wallet_chainstate_integration.cpp)
target_link_libraries(
    test_wallet_chainstate_integration
    geocoin_core
    geocoin_crypto
    geocoin_wallet
    geocoin_state
    geocoin_storage
    geocoin_consensus
)
geocoin_stage_runtime_dependencies(test_wallet_chainstate_integration)
add_test(NAME test_wallet_chainstate_integration COMMAND test_wallet_chainstate_integration)

add_executable(test_wallet_crypto_units tests/test_wallet_crypto_units.cpp)
target_link_libraries(
    test_wallet_crypto_units
    geocoin_core
    geocoin_crypto
    geocoin_wallet
    geocoin_state
    geocoin_storage
    geocoin_consensus
)
geocoin_stage_runtime_dependencies(test_wallet_crypto_units)
target_compile_definitions(test_wallet_crypto_units PRIVATE GEOCOIN_ENABLE_CHAINSTATE_TEST_ACCESS)
add_test(NAME test_wallet_crypto_units COMMAND test_wallet_crypto_units)

add_executable(test_wallet_lifecycle_units tests/test_wallet_lifecycle_units.cpp)
target_link_libraries(
    test_wallet_lifecycle_units
    geocoin_core
    geocoin_crypto
    geocoin_wallet
    geocoin_state
    geocoin_storage
    geocoin_consensus
)
geocoin_stage_runtime_dependencies(test_wallet_lifecycle_units)
target_compile_definitions(test_wallet_lifecycle_units PRIVATE GEOCOIN_ENABLE_CHAINSTATE_TEST_ACCESS)
add_test(NAME test_wallet_lifecycle_units COMMAND test_wallet_lifecycle_units)

add_executable(test_wallet_concurrency tests/test_wallet_concurrency.cpp)
target_link_libraries(test_wallet_concurrency PRIVATE geocoin_wallet)
geocoin_stage_runtime_dependencies(test_wallet_concurrency)
add_test(NAME test_wallet_concurrency COMMAND test_wallet_concurrency)
set_tests_properties(test_wallet_concurrency PROPERTIES TIMEOUT 120 LABELS "wallet;concurrency")

add_executable(test_transaction_guard_units tests/test_transaction_guard_units.cpp)
target_link_libraries(
    test_transaction_guard_units
    geocoin_core
    geocoin_crypto
    geocoin_consensus
)
geocoin_stage_runtime_dependencies(test_transaction_guard_units)
add_test(NAME test_transaction_guard_units COMMAND test_transaction_guard_units)

add_executable(test_nft_validator_units tests/test_nft_validator_units.cpp)
target_link_libraries(
    test_nft_validator_units
    geocoin_core
    geocoin_crypto
    geocoin_state
    geocoin_storage
    geocoin_consensus
)
geocoin_stage_runtime_dependencies(test_nft_validator_units)
add_test(NAME test_nft_validator_units COMMAND test_nft_validator_units)

add_executable(test_state_serialization_units tests/test_state_serialization_units.cpp)
target_link_libraries(
    test_state_serialization_units
    geocoin_state
    geocoin_storage
    geocoin_core
    geocoin_crypto
    geocoin_consensus
)
geocoin_stage_runtime_dependencies(test_state_serialization_units)
add_test(NAME test_state_serialization_units COMMAND test_state_serialization_units)

add_executable(test_block_storage_units tests/test_block_storage_units.cpp)
target_link_libraries(
    test_block_storage_units
    geocoin_storage
    geocoin_core
    geocoin_crypto
    geocoin_consensus
)
target_compile_definitions(test_block_storage_units PRIVATE GEOCOIN_ENABLE_BLOCK_STORAGE_TEST_ACCESS)
geocoin_stage_runtime_dependencies(test_block_storage_units)
add_test(NAME test_block_storage_units COMMAND test_block_storage_units)

add_executable(test_split_write_failures tests/test_split_write_failures.cpp)
target_link_libraries(test_split_write_failures PRIVATE geocoin_state geocoin_storage geocoin_core geocoin_crypto)
target_compile_definitions(test_split_write_failures PRIVATE
    GEOCOIN_ENABLE_BLOCK_STORAGE_TEST_ACCESS GEOCOIN_ENABLE_STATE_STORAGE_TEST_ACCESS)
geocoin_stage_runtime_dependencies(test_split_write_failures)
add_test(NAME test_split_write_failures COMMAND test_split_write_failures)
set_tests_properties(test_split_write_failures PROPERTIES TIMEOUT 120 LABELS "storage;recovery;rocksdb")

add_executable(test_chainstate_revert_units tests/test_chainstate_revert_units.cpp)
target_link_libraries(
    test_chainstate_revert_units
    geocoin_state
    geocoin_wallet
    geocoin_storage
    geocoin_core
    geocoin_crypto
    geocoin_consensus
)
target_compile_definitions(test_chainstate_revert_units PRIVATE GEOCOIN_ENABLE_CHAINSTATE_TEST_ACCESS)
geocoin_stage_runtime_dependencies(test_chainstate_revert_units)
add_test(NAME test_chainstate_revert_units COMMAND test_chainstate_revert_units)

add_executable(test_blockchain_units tests/test_blockchain_units.cpp)
target_link_libraries(
    test_blockchain_units
    geocoin_blockchain
    geocoin_state
    geocoin_storage
    geocoin_chain
    geocoin_core
    geocoin_crypto
    geocoin_consensus
)
target_compile_definitions(
    test_blockchain_units
    PRIVATE
    GEOCOIN_ENABLE_BLOCKCHAIN_TEST_ACCESS
    GEOCOIN_ENABLE_BLOCK_STORAGE_TEST_ACCESS
)
geocoin_stage_runtime_dependencies(test_blockchain_units)
add_test(NAME test_blockchain_units COMMAND test_blockchain_units)

add_executable(test_blockchain_failures tests/test_blockchain_failures.cpp)
target_link_libraries(test_blockchain_failures PRIVATE geocoin_blockchain)
geocoin_stage_runtime_dependencies(test_blockchain_failures)
add_test(NAME test_blockchain_failures COMMAND test_blockchain_failures)

add_executable(test_chain_recovery tests/test_chain_recovery.cpp)
target_link_libraries(test_chain_recovery PRIVATE geocoin_node)
geocoin_stage_runtime_dependencies(test_chain_recovery)
add_test(NAME test_chain_recovery COMMAND test_chain_recovery)

add_executable(test_node_commit_events tests/test_node_commit_events.cpp)
target_link_libraries(test_node_commit_events PRIVATE geocoin_node)
target_compile_definitions(test_node_commit_events PRIVATE GEOCOIN_ENABLE_BLOCK_STORAGE_TEST_ACCESS)
geocoin_stage_runtime_dependencies(test_node_commit_events)
add_test(NAME test_node_commit_events COMMAND test_node_commit_events)
set_tests_properties(test_node_commit_events PROPERTIES LABELS "node;recovery;mempool;rocksdb" TIMEOUT 120)

add_executable(test_mempool_chain_transitions tests/test_mempool_chain_transitions.cpp)
target_link_libraries(test_mempool_chain_transitions PRIVATE geocoin_node)
geocoin_stage_runtime_dependencies(test_mempool_chain_transitions)
add_test(NAME test_mempool_chain_transitions COMMAND test_mempool_chain_transitions)
set_tests_properties(test_mempool_chain_transitions PROPERTIES LABELS "mempool;recovery;consensus" TIMEOUT 120)

add_executable(test_sidechain_mining tests/test_sidechain_mining.cpp)
target_link_libraries(test_sidechain_mining PRIVATE geocoin_network)
geocoin_stage_runtime_dependencies(test_sidechain_mining)
add_test(NAME test_sidechain_mining COMMAND test_sidechain_mining)
set_tests_properties(test_sidechain_mining PROPERTIES LABELS "mempool;sidechain;consensus" TIMEOUT 120)

add_executable(test_card_mining tests/test_card_mining.cpp)
target_link_libraries(test_card_mining PRIVATE geocoin_network)
geocoin_stage_runtime_dependencies(test_card_mining)
add_test(NAME test_card_mining COMMAND test_card_mining)
set_tests_properties(test_card_mining PROPERTIES LABELS "mempool;cards;consensus" TIMEOUT 120)

add_executable(test_nft_mining tests/test_nft_mining.cpp)
target_link_libraries(test_nft_mining PRIVATE geocoin_network)
geocoin_stage_runtime_dependencies(test_nft_mining)
add_test(NAME test_nft_mining COMMAND test_nft_mining)
set_tests_properties(test_nft_mining PROPERTIES LABELS "mempool;nft;consensus" TIMEOUT 120)

add_executable(test_foundation_mining tests/test_foundation_mining.cpp)
target_link_libraries(test_foundation_mining PRIVATE geocoin_network)
geocoin_stage_runtime_dependencies(test_foundation_mining)
add_test(NAME test_foundation_mining COMMAND test_foundation_mining)
set_tests_properties(test_foundation_mining PROPERTIES LABELS "mempool;foundation;consensus" TIMEOUT 120)

add_executable(test_interest_distribution tests/test_interest_distribution.cpp)
target_link_libraries(test_interest_distribution PRIVATE geocoin_network)
target_compile_definitions(test_interest_distribution PRIVATE GEOCOIN_ENABLE_CHAINSTATE_TEST_ACCESS)
geocoin_stage_runtime_dependencies(test_interest_distribution)
add_test(NAME test_interest_distribution COMMAND test_interest_distribution)
set_tests_properties(test_interest_distribution PROPERTIES LABELS "consensus;storage;recovery" TIMEOUT 120)

add_executable(test_coinbase_contract tests/test_coinbase_contract.cpp)
target_link_libraries(test_coinbase_contract PRIVATE geocoin_network)
geocoin_stage_runtime_dependencies(test_coinbase_contract)
add_test(NAME test_coinbase_contract COMMAND test_coinbase_contract)
set_tests_properties(test_coinbase_contract PROPERTIES LABELS "consensus;storage;recovery" TIMEOUT 120)

add_executable(test_consensus_security_regressions tests/test_consensus_security_regressions.cpp)
target_link_libraries(test_consensus_security_regressions PRIVATE geocoin_network)
geocoin_stage_runtime_dependencies(test_consensus_security_regressions)
add_test(NAME test_consensus_security_regressions COMMAND test_consensus_security_regressions)
set_tests_properties(test_consensus_security_regressions PROPERTIES LABELS "consensus;sidechain;security" TIMEOUT 120)

add_executable(test_database_contracts tests/test_database_contracts.cpp)
target_link_libraries(test_database_contracts PRIVATE geocoin_node)
geocoin_stage_runtime_dependencies(test_database_contracts)
add_test(NAME test_database_contracts COMMAND test_database_contracts)
set_tests_properties(test_database_contracts PROPERTIES TIMEOUT 180 LABELS "storage;recovery;concurrency")
add_executable(test_state_reload tests/test_state_reload.cpp)
target_link_libraries(test_state_reload PRIVATE geocoin_blockchain)
geocoin_stage_runtime_dependencies(test_state_reload)
add_test(NAME test_state_reload COMMAND test_state_reload)
set_tests_properties(test_state_reload PROPERTIES TIMEOUT 120 LABELS "storage;recovery")
add_executable(test_transaction_lookup tests/test_transaction_lookup.cpp)
target_link_libraries(test_transaction_lookup PRIVATE geocoin_blockchain)
geocoin_stage_runtime_dependencies(test_transaction_lookup)
add_test(NAME test_transaction_lookup COMMAND test_transaction_lookup)
set_tests_properties(test_transaction_lookup PROPERTIES TIMEOUT 120 LABELS "storage;reorg")
add_executable(test_witness_lookup tests/test_witness_lookup.cpp)
target_link_libraries(test_witness_lookup PRIVATE geocoin_state)
geocoin_stage_runtime_dependencies(test_witness_lookup)
add_test(NAME test_witness_lookup COMMAND test_witness_lookup)
set_tests_properties(test_witness_lookup PROPERTIES TIMEOUT 120 LABELS "storage")
add_executable(test_database_schema tests/test_database_schema.cpp)
target_link_libraries(test_database_schema PRIVATE geocoin_wallet geocoin_state)
target_compile_definitions(test_database_schema PRIVATE GEOCOIN_ENABLE_CHAINSTATE_TEST_ACCESS)
geocoin_stage_runtime_dependencies(test_database_schema)
add_test(NAME test_database_schema COMMAND test_database_schema)
set_tests_properties(test_database_schema PROPERTIES TIMEOUT 180 LABELS "storage;recovery;wallet")

add_executable(test_card_registry tests/test_card_registry.cpp)
target_link_libraries(test_card_registry PRIVATE geocoin_state)
target_compile_definitions(test_card_registry PRIVATE GEOCOIN_ENABLE_CHAINSTATE_TEST_ACCESS)
geocoin_stage_runtime_dependencies(test_card_registry)
add_test(NAME test_card_registry COMMAND test_card_registry)
set_tests_properties(test_card_registry PROPERTIES TIMEOUT 120 LABELS "storage;recovery")

add_executable(test_sidechain_registry tests/test_sidechain_registry.cpp)
target_link_libraries(test_sidechain_registry PRIVATE geocoin_state)
target_compile_definitions(test_sidechain_registry PRIVATE GEOCOIN_ENABLE_CHAINSTATE_TEST_ACCESS)
geocoin_stage_runtime_dependencies(test_sidechain_registry)
add_test(NAME test_sidechain_registry COMMAND test_sidechain_registry)
set_tests_properties(test_sidechain_registry PROPERTIES TIMEOUT 180 LABELS "storage;recovery;sidechain")

add_executable(test_transaction_hardening tests/test_transaction_hardening.cpp)
target_link_libraries(test_transaction_hardening PRIVATE geocoin_network_txpool geocoin_state)
geocoin_stage_runtime_dependencies(test_transaction_hardening)
add_test(NAME test_transaction_hardening COMMAND test_transaction_hardening)
set_tests_properties(test_transaction_hardening PROPERTIES TIMEOUT 120 LABELS "consensus;sidechain;mempool")

add_executable(test_storage_scan tests/test_storage_scan.cpp)
target_link_libraries(test_storage_scan PRIVATE geocoin_state)
geocoin_stage_runtime_dependencies(test_storage_scan)
add_test(NAME test_storage_scan COMMAND test_storage_scan)
set_tests_properties(test_storage_scan PROPERTIES TIMEOUT 120 LABELS "storage")
add_executable(test_state_hot_paths tests/test_state_hot_paths.cpp)
target_link_libraries(test_state_hot_paths PRIVATE geocoin_blockchain)
geocoin_stage_runtime_dependencies(test_state_hot_paths)
add_test(NAME test_state_hot_paths COMMAND test_state_hot_paths)
set_tests_properties(test_state_hot_paths PROPERTIES TIMEOUT 180 LABELS "storage;consensus")
add_executable(test_wallet_durable_save tests/test_wallet_durable_save.cpp)
target_link_libraries(test_wallet_durable_save PRIVATE geocoin_wallet)
geocoin_stage_runtime_dependencies(test_wallet_durable_save)
add_test(NAME test_wallet_durable_save COMMAND test_wallet_durable_save)
if(TARGET geocoin-db)
    add_test(NAME test_database_backup COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/test_database_backup.py
        $<TARGET_FILE:test_database_contracts> $<TARGET_FILE:geocoin-db>)
endif()

add_executable(test_wallet_scan_recovery tests/test_wallet_scan_recovery.cpp)
target_link_libraries(test_wallet_scan_recovery PRIVATE geocoin_blockchain geocoin_wallet)
geocoin_stage_runtime_dependencies(test_wallet_scan_recovery)
add_test(NAME test_wallet_scan_recovery COMMAND test_wallet_scan_recovery)
set_tests_properties(test_wallet_scan_recovery PROPERTIES TIMEOUT 180)

if(BUILD_QT_GUI)
    find_package(Qt5 COMPONENTS Core Gui Widgets REQUIRED)
    add_executable(
        test_wallet_sync_worker
        tests/test_wallet_sync_worker.cpp
        src/qt/walletsyncworker.cpp
    )
    target_include_directories(test_wallet_sync_worker PRIVATE src/qt)
    target_link_libraries(
        test_wallet_sync_worker
        PRIVATE
            Qt5::Core
            geocoin_blockchain
            geocoin_wallet
            Threads::Threads
    )
    geocoin_stage_runtime_dependencies(test_wallet_sync_worker)
    add_test(NAME test_wallet_sync_worker COMMAND test_wallet_sync_worker)
    set_tests_properties(test_wallet_sync_worker PROPERTIES TIMEOUT 30 LABELS "wallet;recovery")

    add_executable(
        test_gui_wallet_persistence
        tests/test_gui_wallet_persistence.cpp
        src/qt/walletmodel.cpp
        src/qt/guiutil.cpp
        src/qt/optionsmodel.cpp
        src/qt/transactiontablemodel.cpp
        src/qt/addresstablemodel.cpp
    )
    set_target_properties(test_gui_wallet_persistence PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON)
    target_include_directories(test_gui_wallet_persistence PRIVATE src/qt "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(
        test_gui_wallet_persistence
        PRIVATE
            Qt5::Core
            Qt5::Gui
            Qt5::Widgets
            geocoin_core
            geocoin_consensus
            geocoin_crypto
            geocoin_network
            geocoin_rpc
            geocoin_state
            geocoin_storage
            geocoin_wallet
            geocoin_node
            Threads::Threads
    )
    geocoin_stage_runtime_dependencies(test_gui_wallet_persistence)
    find_program(XVFB_RUN_EXECUTABLE xvfb-run)
    if(XVFB_RUN_EXECUTABLE)
        add_test(
            NAME test_gui_wallet_persistence
            COMMAND ${XVFB_RUN_EXECUTABLE} -a
                    $<TARGET_FILE:test_gui_wallet_persistence> -platform xcb
        )
    else()
        add_test(
            NAME test_gui_wallet_persistence
            COMMAND ${CMAKE_COMMAND} -E env QT_QPA_PLATFORM=offscreen
                    $<TARGET_FILE:test_gui_wallet_persistence> -platform offscreen
        )
    endif()
    set_tests_properties(test_gui_wallet_persistence PROPERTIES
        TIMEOUT 120
        LABELS "wallet;gui;recovery"
    )

    add_executable(
        test_gui_contract
        tests/test_gui_contract.cpp
        src/qt/walletmodel.cpp
        src/qt/guiutil.cpp
        src/qt/optionsmodel.cpp
        src/qt/clientmodel.cpp
        src/qt/transactiontablemodel.cpp
        src/qt/addresstablemodel.cpp
        src/qt/overviewpage.cpp
        src/qt/sendcoinspage.cpp
        src/qt/receivecoinspage.cpp
        src/qt/transactionpage.cpp
        src/qt/miningpage.cpp
        src/qt/miningpage_gpu.cpp
        src/qt/nftpage.cpp
        src/qt/nftpage_create.cpp
        src/qt/multisigpage.cpp
        src/qt/multisigpage_initiator.cpp
        src/qt/multisigpage_signer.cpp
        src/qt/multisigpage_history.cpp
        src/qt/multisigpage_scanner.cpp
        src/qt/commitmentscanner.cpp
        src/qt/corporatepage.cpp
        src/qt/corporatepage_founder.cpp
        src/qt/corporatepage_director.cpp
        src/qt/corporatepage_signer.cpp
        src/qt/corporatepage_history.cpp
    )
    set_target_properties(test_gui_contract PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON)
    target_include_directories(test_gui_contract PRIVATE src/qt "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(
        test_gui_contract
        PRIVATE
            Qt5::Core
            Qt5::Gui
            Qt5::Widgets
            geocoin_core
            geocoin_consensus
            geocoin_crypto
            geocoin_network
            geocoin_rpc
            geocoin_state
            geocoin_storage
            geocoin_wallet
            geocoin_node
            Threads::Threads
    )
    geocoin_stage_runtime_dependencies(test_gui_contract)
    if(XVFB_RUN_EXECUTABLE)
        add_test(
            NAME test_gui_contract
            COMMAND ${XVFB_RUN_EXECUTABLE} -a
                    $<TARGET_FILE:test_gui_contract> -platform xcb
        )
    else()
        add_test(
            NAME test_gui_contract
            COMMAND ${CMAKE_COMMAND} -E env QT_QPA_PLATFORM=offscreen
                    $<TARGET_FILE:test_gui_contract> -platform offscreen
        )
    endif()
    set_tests_properties(test_gui_contract PROPERTIES
        TIMEOUT 180
        LABELS "wallet;gui"
    )
endif()

add_executable(test_active_chain_index tests/test_active_chain_index.cpp)
target_link_libraries(test_active_chain_index PRIVATE geocoin_blockchain)
geocoin_stage_runtime_dependencies(test_active_chain_index)
add_test(NAME test_active_chain_index COMMAND test_active_chain_index)

add_executable(test_multisig_verifier_units tests/test_multisig_verifier_units.cpp)
target_link_libraries(
    test_multisig_verifier_units
    geocoin_core
    geocoin_crypto
    geocoin_wallet
)
geocoin_stage_runtime_dependencies(test_multisig_verifier_units)
target_compile_definitions(test_multisig_verifier_units PRIVATE GEOCOIN_ENABLE_CHAINSTATE_TEST_ACCESS)
add_test(NAME test_multisig_verifier_units COMMAND test_multisig_verifier_units)

add_executable(test_multisig_e2e tests/test_multisig_e2e.cpp)
target_link_libraries(test_multisig_e2e PRIVATE geocoin_network geocoin_wallet)
geocoin_stage_runtime_dependencies(test_multisig_e2e)
add_test(NAME test_multisig_e2e COMMAND test_multisig_e2e)
set_tests_properties(test_multisig_e2e PROPERTIES LABELS "consensus;multisig;delegation;rocksdb" TIMEOUT 180)

add_executable(test_consensus_authorization tests/test_consensus_authorization.cpp)
target_link_libraries(test_consensus_authorization geocoin_wallet geocoin_blockchain geocoin_network_txpool)
target_compile_definitions(test_consensus_authorization PRIVATE GEOCOIN_ENABLE_CHAINSTATE_TEST_ACCESS)
geocoin_stage_runtime_dependencies(test_consensus_authorization)
add_test(NAME test_consensus_authorization COMMAND test_consensus_authorization)
set_tests_properties(test_consensus_authorization PROPERTIES LABELS "consensus;multisig;delegation;rocksdb" TIMEOUT 120)

add_executable(test_delegation_timing tests/test_delegation_timing.cpp)
target_link_libraries(test_delegation_timing PRIVATE geocoin_rpc geocoin_node geocoin_network geocoin_blockchain)
target_compile_definitions(test_delegation_timing PRIVATE
    GEOCOIN_ENABLE_BLOCKCHAIN_TEST_ACCESS GEOCOIN_ENABLE_NODE_TEST_ACCESS)
geocoin_stage_runtime_dependencies(test_delegation_timing)
add_test(NAME test_delegation_timing COMMAND test_delegation_timing)
set_tests_properties(test_delegation_timing PROPERTIES LABELS "consensus;multisig;delegation;rocksdb" TIMEOUT 120)

add_executable(test_transaction_finality tests/test_transaction_finality.cpp)
target_link_libraries(test_transaction_finality PRIVATE geocoin_rpc geocoin_node geocoin_network geocoin_blockchain)
target_compile_definitions(test_transaction_finality PRIVATE GEOCOIN_ENABLE_CHAINSTATE_TEST_ACCESS)
geocoin_stage_runtime_dependencies(test_transaction_finality)
add_test(NAME test_transaction_finality COMMAND test_transaction_finality)
set_tests_properties(test_transaction_finality PROPERTIES LABELS "consensus;mempool;rocksdb" TIMEOUT 180)

add_executable(test_transaction_envelope tests/test_transaction_envelope.cpp)
target_link_libraries(test_transaction_envelope PRIVATE geocoin_rpc geocoin_network geocoin_blockchain)
target_compile_definitions(test_transaction_envelope PRIVATE GEOCOIN_ENABLE_RPC_SERVER_TEST_ACCESS)
geocoin_stage_runtime_dependencies(test_transaction_envelope)
add_test(NAME test_transaction_envelope COMMAND test_transaction_envelope)
set_tests_properties(test_transaction_envelope PROPERTIES LABELS "consensus;mempool;rocksdb" TIMEOUT 120)

add_executable(test_transaction_fields tests/test_transaction_fields.cpp)
target_link_libraries(test_transaction_fields PRIVATE geocoin_network geocoin_blockchain)
target_compile_definitions(test_transaction_fields PRIVATE GEOCOIN_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
geocoin_stage_runtime_dependencies(test_transaction_fields)
add_test(NAME test_transaction_fields COMMAND test_transaction_fields)
set_tests_properties(test_transaction_fields PROPERTIES LABELS "consensus;mempool;rocksdb" TIMEOUT 120)

add_executable(test_node_witness_contract tests/test_node_witness_contract.cpp)
target_link_libraries(test_node_witness_contract PRIVATE geocoin_network geocoin_blockchain)
geocoin_stage_runtime_dependencies(test_node_witness_contract)
add_test(NAME test_node_witness_contract COMMAND test_node_witness_contract)
set_tests_properties(test_node_witness_contract PROPERTIES LABELS "consensus;mempool;rocksdb;node" TIMEOUT 120)

add_executable(test_node_sync_recovery tests/test_node_sync_recovery.cpp)
target_link_libraries(test_node_sync_recovery PRIVATE geocoin_network_sync geocoin_blockchain)
target_compile_definitions(test_node_sync_recovery PRIVATE
    GEOCOIN_ENABLE_SYNC_MANAGER_TEST_ACCESS GEOCOIN_ENABLE_BLOCK_STORAGE_TEST_ACCESS)
geocoin_stage_runtime_dependencies(test_node_sync_recovery)
add_test(NAME test_node_sync_recovery COMMAND test_node_sync_recovery)
set_tests_properties(test_node_sync_recovery PROPERTIES LABELS "reorg;recovery;node;p2p" TIMEOUT 60)

add_executable(test_hashes tests/test_hashes.cpp)
target_link_libraries(
    test_hashes
    geocoin_core
    geocoin_crypto
)
geocoin_stage_runtime_dependencies(test_hashes)
add_test(NAME test_hashes COMMAND test_hashes)

add_executable(test_auth_network_units tests/test_auth_network_units.cpp)
target_link_libraries(
    test_auth_network_units
    geocoin_crypto
    geocoin_network_peer
    geocoin_core
)
geocoin_stage_runtime_dependencies(test_auth_network_units)
add_test(NAME test_auth_network_units COMMAND test_auth_network_units)

add_executable(test_headers_message tests/test_headers_message.cpp)
target_link_libraries(
    test_headers_message
    geocoin_network_transport
    geocoin_core
    geocoin_consensus
    geocoin_crypto
)
geocoin_stage_runtime_dependencies(test_headers_message)
add_test(NAME test_headers_message COMMAND test_headers_message)

add_executable(test_sync_manager_units tests/test_sync_manager_units.cpp)
target_link_libraries(
    test_sync_manager_units
    geocoin_network_sync
    geocoin_blockchain
    geocoin_state
    geocoin_storage
    geocoin_consensus
    geocoin_chain
    geocoin_crypto
    geocoin_core
)
target_compile_definitions(
    test_sync_manager_units
    PRIVATE
        GEOCOIN_ENABLE_SYNC_MANAGER_TEST_ACCESS
        GEOCOIN_ENABLE_BLOCKCHAIN_TEST_ACCESS
)
geocoin_stage_runtime_dependencies(test_sync_manager_units)
add_test(NAME test_sync_manager_units COMMAND test_sync_manager_units)

add_executable(test_mempool_units tests/test_mempool_units.cpp)
target_link_libraries(
    test_mempool_units
    geocoin_network_txpool
    geocoin_state
    geocoin_storage
    geocoin_wallet
    geocoin_consensus
    geocoin_core
    geocoin_crypto
)
target_compile_definitions(test_mempool_units PRIVATE GEOCOIN_ENABLE_CHAINSTATE_TEST_ACCESS)
geocoin_stage_runtime_dependencies(test_mempool_units)
add_test(NAME test_mempool_units COMMAND test_mempool_units)

add_executable(test_parser_fuzz tests/test_parser_fuzz.cpp)
target_link_libraries(
    test_parser_fuzz
    geocoin_rpc
    geocoin_network_transport
    geocoin_core
    geocoin_consensus
    geocoin_crypto
)
geocoin_stage_runtime_dependencies(test_parser_fuzz)
add_test(NAME test_parser_fuzz COMMAND test_parser_fuzz)

add_executable(test_network_policy tests/test_network_policy.cpp)
target_link_libraries(
    test_network_policy
    geocoin_network_peer
    geocoin_core
    geocoin_crypto
)
geocoin_stage_runtime_dependencies(test_network_policy)
add_test(NAME test_network_policy COMMAND test_network_policy)

add_executable(test_peer_policy tests/test_peer_policy.cpp)
target_link_libraries(
    test_peer_policy
    geocoin_network_peer
    geocoin_core
    geocoin_consensus
    geocoin_crypto
)
target_compile_definitions(test_peer_policy PRIVATE GEOCOIN_ENABLE_PEER_MANAGER_TEST_ACCESS)
geocoin_stage_runtime_dependencies(test_peer_policy)
add_test(NAME test_peer_policy COMMAND test_peer_policy)

add_executable(test_peer_transport_units tests/test_peer_transport_units.cpp)
target_link_libraries(
    test_peer_transport_units
    geocoin_network_transport
    geocoin_core
    geocoin_consensus
    geocoin_crypto
)
geocoin_stage_runtime_dependencies(test_peer_transport_units)
add_test(NAME test_peer_transport_units COMMAND test_peer_transport_units)

add_executable(test_p2p_regressions tests/test_p2p_regressions.cpp)
target_link_libraries(test_p2p_regressions PRIVATE geocoin_network_peer)
target_compile_definitions(test_p2p_regressions PRIVATE
    GEOCOIN_ENABLE_PEER_MANAGER_TEST_ACCESS GEOCOIN_ENABLE_PEER_DISCOVERY_TEST_ACCESS)
geocoin_stage_runtime_dependencies(test_p2p_regressions)
set(p2p_regression_scenarios discovery_thread listener session_replacement dns_addresses testnet_discovery)
if(NOT WIN32)
    list(APPEND p2p_regression_scenarios buffered_eof invalid_inbound high_fd_connect high_fd_send address_store callback_failure)
endif()
foreach(scenario IN LISTS p2p_regression_scenarios)
    add_test(NAME test_p2p_${scenario} COMMAND test_p2p_regressions ${scenario})
    set_tests_properties(test_p2p_${scenario} PROPERTIES LABELS "p2p;regression" TIMEOUT 15 SKIP_RETURN_CODE 77)
endforeach()

add_executable(test_secure_channel_units tests/test_secure_channel_units.cpp)
target_link_libraries(
    test_secure_channel_units
    geocoin_network_transport
    geocoin_core
    geocoin_consensus
    geocoin_crypto
)
geocoin_stage_runtime_dependencies(test_secure_channel_units)
add_test(NAME test_secure_channel_units COMMAND test_secure_channel_units)

add_executable(test_u64_math tests/test_u64_math.cpp)
target_link_libraries(
    test_u64_math
    geocoin_core
)
geocoin_stage_runtime_dependencies(test_u64_math)
add_test(NAME test_u64_math COMMAND test_u64_math)

add_executable(test_difficulty_emergency tests/test_difficulty_emergency.cpp)
target_link_libraries(
    test_difficulty_emergency
    geocoin_consensus
    geocoin_chain
    geocoin_core
    geocoin_crypto
)
geocoin_stage_runtime_dependencies(test_difficulty_emergency)
add_test(NAME test_difficulty_emergency COMMAND test_difficulty_emergency)

add_executable(test_mining_units tests/test_mining_units.cpp)
target_link_libraries(
    test_mining_units
    geocoin_core
    geocoin_consensus
    geocoin_core
    geocoin_crypto
    geocoin_consensus
)
geocoin_stage_runtime_dependencies(test_mining_units)
add_test(NAME test_mining_units COMMAND test_mining_units)

add_executable(test_geography tests/test_geography.cpp)
target_link_libraries(test_geography geocoin_rpc geocoin_node geocoin_network)
target_compile_definitions(test_geography PRIVATE
    GEOCOIN_ENABLE_RPC_SERVER_TEST_ACCESS GEOCOIN_ENABLE_BLOCKCHAIN_TEST_ACCESS
    GEOCOIN_ENABLE_BLOCK_STORAGE_TEST_ACCESS)
geocoin_stage_runtime_dependencies(test_geography)
add_test(NAME test_geography COMMAND test_geography)
set_tests_properties(test_geography PROPERTIES LABELS "consensus;rpc;geography" TIMEOUT 60)

add_executable(test_geodesy tests/test_geodesy.cpp)
target_link_libraries(test_geodesy geocoin_core geocoin_consensus geocoin_state GeographicLib::GeographicLib Threads::Threads)
geocoin_strict_geodesy(test_geodesy)
geocoin_stage_runtime_dependencies(test_geodesy)
add_test(NAME test_geodesy COMMAND test_geodesy)
set_tests_properties(test_geodesy PROPERTIES LABELS "consensus;geography;nft" TIMEOUT 60)

# Exercise the pinned algorithms against their own global regression vectors.
foreach(_geocoin_geo_test geodtest polygontest intersecttest)
    add_executable(test_geographiclib_${_geocoin_geo_test}
        geographiclib/tests/${_geocoin_geo_test}.cpp)
    target_link_libraries(test_geographiclib_${_geocoin_geo_test} PRIVATE GeographicLib::GeographicLib)
    geocoin_strict_geodesy(test_geographiclib_${_geocoin_geo_test})
    add_test(NAME test_geographiclib_${_geocoin_geo_test}
        COMMAND test_geographiclib_${_geocoin_geo_test})
    set_tests_properties(test_geographiclib_${_geocoin_geo_test} PROPERTIES LABELS "geography;unit" TIMEOUT 60)
endforeach()

add_executable(test_benchmark_units tests/test_benchmark_units.cpp)
target_link_libraries(
    test_benchmark_units
    geocoin_benchmark
    geocoin_core
    geocoin_crypto
    geocoin_rocksdb
)
geocoin_stage_runtime_dependencies(test_benchmark_units)
add_test(NAME test_benchmark_units COMMAND test_benchmark_units)

add_executable(test_node_status_utils tests/test_node_status_utils.cpp)
target_link_libraries(
    test_node_status_utils
    geocoin_core
)
geocoin_stage_runtime_dependencies(test_node_status_utils)
add_test(NAME test_node_status_utils COMMAND test_node_status_utils)

add_executable(test_node_mining_guards tests/test_node_mining_guards.cpp)
target_link_libraries(
    test_node_mining_guards
    geocoin_rpc
    geocoin_node
    geocoin_network
    geocoin_blockchain
    geocoin_state
    geocoin_storage
    geocoin_wallet
    geocoin_consensus
    geocoin_chain
    geocoin_core
    geocoin_crypto
)
target_compile_definitions(
    test_node_mining_guards
    PRIVATE
        GEOCOIN_ENABLE_NODE_TEST_ACCESS
        GEOCOIN_ENABLE_PEER_MANAGER_TEST_ACCESS
        GEOCOIN_ENABLE_BLOCKCHAIN_TEST_ACCESS
        GEOCOIN_ENABLE_CHAINSTATE_TEST_ACCESS
)
geocoin_stage_runtime_dependencies(test_node_mining_guards)
add_test(NAME test_node_mining_guards COMMAND test_node_mining_guards)

add_executable(test_mining_reward_scenarios tests/test_mining_reward_scenarios.cpp)
target_link_libraries(
    test_mining_reward_scenarios PRIVATE
    geocoin_rpc geocoin_node geocoin_network geocoin_blockchain geocoin_state
    geocoin_storage geocoin_wallet geocoin_consensus geocoin_chain geocoin_core geocoin_crypto
)
target_compile_definitions(test_mining_reward_scenarios PRIVATE GEOCOIN_ENABLE_NODE_TEST_ACCESS)
geocoin_stage_runtime_dependencies(test_mining_reward_scenarios)
add_test(NAME test_mining_reward_scenarios COMMAND test_mining_reward_scenarios)
set_tests_properties(test_mining_reward_scenarios PROPERTIES LABELS "mining;consensus;storage" TIMEOUT 120)
add_test(NAME test_invalid_reward_restart COMMAND test_mining_reward_scenarios invalid-reward-restart)
set_tests_properties(test_invalid_reward_restart PROPERTIES LABELS "mining;consensus;storage;regression" TIMEOUT 30)

add_executable(test_node_peer_messages_units tests/test_node_peer_messages_units.cpp)
target_link_libraries(
    test_node_peer_messages_units
    geocoin_node
    geocoin_network
    geocoin_blockchain
    geocoin_state
    geocoin_storage
    geocoin_wallet
    geocoin_consensus
    geocoin_chain
    geocoin_core
    geocoin_crypto
)
target_compile_definitions(
    test_node_peer_messages_units
    PRIVATE
        GEOCOIN_ENABLE_NODE_TEST_ACCESS
        GEOCOIN_ENABLE_PEER_MANAGER_TEST_ACCESS
)
geocoin_stage_runtime_dependencies(test_node_peer_messages_units)
add_test(NAME test_node_peer_messages_units COMMAND test_node_peer_messages_units)

add_executable(test_node_connections_units tests/test_node_connections_units.cpp)
target_link_libraries(
    test_node_connections_units
    geocoin_node
    geocoin_network
    geocoin_blockchain
    geocoin_state
    geocoin_storage
    geocoin_wallet
    geocoin_consensus
    geocoin_chain
    geocoin_core
    geocoin_crypto
    Threads::Threads
)
target_compile_definitions(
    test_node_connections_units
    PRIVATE
        GEOCOIN_ENABLE_NODE_TEST_ACCESS
)
geocoin_stage_runtime_dependencies(test_node_connections_units)
add_test(NAME test_node_connections_units COMMAND test_node_connections_units)

add_executable(test_node_connection_backoff tests/test_node_connection_backoff.cpp)
target_link_libraries(
    test_node_connection_backoff
    geocoin_node
    geocoin_network
    geocoin_blockchain
    geocoin_state
    geocoin_storage
    geocoin_consensus
    geocoin_chain
    geocoin_core
    geocoin_crypto
    Threads::Threads
)
geocoin_stage_runtime_dependencies(test_node_connection_backoff)
add_test(NAME test_node_connection_backoff COMMAND test_node_connection_backoff)
set_tests_properties(test_node_connection_backoff PROPERTIES LABELS "node;network" TIMEOUT 90)

add_executable(test_node_event_recovery tests/test_node_event_recovery.cpp)
target_link_libraries(test_node_event_recovery PRIVATE geocoin_node)
geocoin_stage_runtime_dependencies(test_node_event_recovery)
add_test(NAME test_node_event_recovery COMMAND test_node_event_recovery)
set_tests_properties(test_node_event_recovery PROPERTIES LABELS "node;recovery" TIMEOUT 45)

add_executable(test_node_shutdown tests/test_node_shutdown.cpp)
target_link_libraries(test_node_shutdown PRIVATE geocoin_node)
geocoin_stage_runtime_dependencies(test_node_shutdown)
add_test(NAME test_node_shutdown COMMAND ${Python3_EXECUTABLE}
    ${PROJECT_SOURCE_DIR}/tests/test_node_shutdown.py $<TARGET_FILE:test_node_shutdown>)
set_tests_properties(test_node_shutdown PROPERTIES LABELS "node;lifecycle" TIMEOUT 75)

add_executable(test_node_config_loader tests/test_node_config_loader.cpp)
target_link_libraries(
    test_node_config_loader
    geocoin_node
)
geocoin_stage_runtime_dependencies(test_node_config_loader)
add_test(NAME test_node_config_loader COMMAND test_node_config_loader)

add_executable(test_all_transactions tests/test_all_transactions.cpp)
target_link_libraries(
    test_all_transactions
    geocoin_core
    geocoin_crypto
    geocoin_consensus
    geocoin_state
    geocoin_storage
    geocoin_wallet
)
geocoin_stage_runtime_dependencies(test_all_transactions)
add_test(NAME test_all_transactions COMMAND test_all_transactions)

add_executable(multisig_package_builder tests/multisig_package_builder.cpp)
target_link_libraries(
    multisig_package_builder
    geocoin_core
    geocoin_crypto
    geocoin_wallet
    geocoin_state
    geocoin_storage
    geocoin_consensus
)
geocoin_stage_runtime_dependencies(multisig_package_builder)

add_executable(sidechain_test_tool tests/sidechain_test_tool.cpp)
target_link_libraries(
    sidechain_test_tool
    geocoin_core
    geocoin_crypto
)
geocoin_stage_runtime_dependencies(sidechain_test_tool)

add_executable(test_sidechain_restart_persistence tests/test_sidechain_restart_persistence.cpp)
target_link_libraries(
    test_sidechain_restart_persistence
    geocoin_core
    geocoin_crypto
    geocoin_consensus
    geocoin_state
    geocoin_storage
    geocoin_wallet
)
geocoin_stage_runtime_dependencies(test_sidechain_restart_persistence)
add_test(NAME test_sidechain_restart_persistence COMMAND test_sidechain_restart_persistence)

add_executable(test_b2b_corporate_policy tests/test_b2b_corporate_policy.cpp)
target_link_libraries(test_b2b_corporate_policy geocoin_b2b geocoin_crypto)
geocoin_stage_runtime_dependencies(test_b2b_corporate_policy)
add_test(NAME test_b2b_corporate_policy COMMAND test_b2b_corporate_policy)

add_executable(test_storage_split_metadata tests/test_storage_split_metadata.cpp)
target_link_libraries(
    test_storage_split_metadata
    geocoin_state
    geocoin_storage
    geocoin_wallet
    geocoin_core
    geocoin_crypto
    geocoin_consensus
)
geocoin_stage_runtime_dependencies(test_storage_split_metadata)
add_test(NAME test_storage_split_metadata COMMAND test_storage_split_metadata)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_address_compat.cpp")
    add_executable(test_address_compat tests/test_address_compat.cpp)
    target_link_libraries(test_address_compat geocoin_core geocoin_crypto geocoin_wallet)
    geocoin_stage_runtime_dependencies(test_address_compat)
    add_test(NAME test_address_compat COMMAND test_address_compat)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_wallet_legacy_compat.cpp")
    add_executable(test_wallet_legacy_compat tests/test_wallet_legacy_compat.cpp)
    target_link_libraries(
        test_wallet_legacy_compat
        geocoin_core
        geocoin_crypto
        geocoin_wallet
        geocoin_state
        geocoin_storage
        geocoin_consensus
    )
    geocoin_stage_runtime_dependencies(test_wallet_legacy_compat)
    add_test(NAME test_wallet_legacy_compat COMMAND test_wallet_legacy_compat)
endif()

add_executable(test_chainstate_determinism tests/test_chainstate_determinism.cpp)
target_link_libraries(test_chainstate_determinism PRIVATE geocoin_blockchain)
target_compile_definitions(test_chainstate_determinism PRIVATE
    GEOCOIN_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
geocoin_stage_runtime_dependencies(test_chainstate_determinism)
add_test(NAME test_chainstate_determinism COMMAND test_chainstate_determinism)
set_tests_properties(test_chainstate_determinism PROPERTIES LABELS "consensus;determinism" TIMEOUT 180)
