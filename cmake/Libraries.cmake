# Core library
add_library(geocoin_core STATIC
    src/geocoin/core/transaction.cpp
    src/geocoin/core/transaction_ser.cpp
    src/geocoin/core/transaction_fields.cpp
    src/geocoin/core/transaction_sign.cpp
    src/geocoin/core/transaction_coinbase.cpp
    src/geocoin/core/transaction_card.cpp
    src/geocoin/core/transaction_sidechain.cpp
    src/geocoin/core/transaction_nft.cpp
    src/geocoin/core/transaction_foundation.cpp
    src/geocoin/core/transaction_multisig.cpp
    src/geocoin/core/transaction_cadastral.cpp
    src/geocoin/core/block.cpp
    src/geocoin/core/merkle.cpp
    src/geocoin/core/genesis.cpp
    src/geocoin/nft/nft_types.cpp
    src/geocoin/nft/geodesy.cpp
    src/geocoin/nft/geometry_index.cpp
    src/geocoin/nft/geometry_codec.cpp
    src/geocoin/nft/geojson_codec.cpp
    src/geocoin/nft/nft_validator.cpp
    src/geocoin/utils/logger.cpp
    src/geocoin/utils/metrics.cpp
    src/geocoin/utils/logging.cpp
    src/geocoin/utils/config.cpp
    src/geocoin/utils/activation_manifest.cpp
)
add_library(geocoin_crypto STATIC
    src/geocoin/crypto/geo_area.cpp
    src/geocoin/crypto/signatures.cpp
    src/geocoin/crypto/falcon512.cpp
    src/geocoin/crypto/ml_kem768.cpp
    src/geocoin/crypto/addresses.cpp
    src/geocoin/crypto/challenge_response.cpp
    src/geocoin/crypto/encryption.cpp
    src/geocoin/crypto/ioke_sha256.c
    src/geocoin/crypto/ioke_ripemd160.c
)
if ((CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i[3-6]86") AND
    (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang"))
    target_sources(geocoin_crypto PRIVATE src/geocoin/crypto/ioke_sha256_x86_sha_ni.c)
    set_source_files_properties(
        src/geocoin/crypto/ioke_sha256_x86_sha_ni.c
        PROPERTIES COMPILE_OPTIONS "-msha;-mssse3;-msse4.1"
    )
    target_compile_definitions(geocoin_crypto PRIVATE IOKE_SHA256_HAVE_X86_SHA_NI=1)
endif()
target_include_directories(geocoin_crypto PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/liboqs/include
    ${CMAKE_CURRENT_SOURCE_DIR}/liboqs/src
    ${CMAKE_CURRENT_SOURCE_DIR}/liboqs/src/common/pqclean_shims
)

add_library(geocoin_chain STATIC
    src/geocoin/chain/block_index.cpp
    src/geocoin/chain/chain.cpp
)

add_library(geocoin_consensus STATIC
    src/geocoin/consensus/block_verification.cpp
    src/geocoin/consensus/proof_of_work.cpp
    src/geocoin/consensus/optimized_miner.cpp
    src/geocoin/consensus/consensus.cpp
    src/geocoin/consensus/consensus_tx.cpp
    src/geocoin/consensus/consensus_card.cpp
    src/geocoin/consensus/consensus_nft.cpp
    src/geocoin/consensus/consensus_foundation.cpp
    src/geocoin/consensus/consensus_multisig.cpp
    src/geocoin/consensus/consensus_cadastral.cpp
    src/geocoin/consensus/consensus_sidechain_tx.cpp
    src/geocoin/consensus/consensus_difficulty.cpp
    src/geocoin/consensus/consensus_block.cpp
    src/geocoin/consensus/chainparams.cpp
    src/geocoin/consensus/sidechain_checkpoint.cpp
    src/geocoin/consensus/sidechain_rules.cpp
)

add_library(geocoin_blockchain STATIC
    src/geocoin/core/blockchain.cpp
    src/geocoin/core/blockchain_init.cpp
    src/geocoin/core/blockchain_accept.cpp
    src/geocoin/core/blockchain_index.cpp
    src/geocoin/core/blockchain_reorg.cpp
)

add_library(geocoin_state STATIC
    src/geocoin/state/state.cpp
    src/geocoin/state/state_accessors.cpp
    src/geocoin/state/state_lifecycle.cpp
    src/geocoin/state/state_metadata.cpp
    src/geocoin/state/state_nft.cpp
    src/geocoin/state/state_nft_projection.cpp
    src/geocoin/state/state_card.cpp
    src/geocoin/state/state_card_transition.cpp
    src/geocoin/state/state_card_projection.cpp
    src/geocoin/state/state_sidechain.cpp
    src/geocoin/state/state_sidechain_helpers.cpp
    src/geocoin/state/state_sidechain_transition.cpp
    src/geocoin/state/state_sidechain_projection.cpp
    src/geocoin/state/state_sidechain_validate.cpp
    src/geocoin/state/state_foundation.cpp
    src/geocoin/state/state_multisig.cpp
    src/geocoin/state/state_utxo.cpp
    src/geocoin/state/state_block.cpp
    src/geocoin/state/state_block_revert.cpp
    src/geocoin/state/state_persist_nft.cpp
    src/geocoin/state/state_persist_sidechain.cpp
    src/geocoin/state/state_persist_card.cpp
    src/geocoin/state/state_persist_foundation.cpp
    src/geocoin/state/state_persist_delegation.cpp
    src/geocoin/state/state_sidechain_undo.cpp
    src/geocoin/state/state_undo_store.cpp
    src/geocoin/state/state_queries.cpp
    src/geocoin/state/state_serialization.cpp
    src/geocoin/state/state_serialization_utxo.cpp
    src/geocoin/state/state_witness.cpp
    src/geocoin/state/storage.cpp
    src/geocoin/state/storage_memory.cpp
    src/geocoin/state/storage_rocksdb.cpp
)

add_library(geocoin_storage STATIC
    src/geocoin/storage/backend_support.cpp
    src/geocoin/storage/block_records.cpp
    src/geocoin/storage/block_storage.cpp
    src/geocoin/storage/rocksdb_store.cpp
    src/geocoin/storage/witness_payload.cpp
)

add_library(geocoin_network_transport STATIC
    src/geocoin/network/peer.cpp
    src/geocoin/network/tcp_peer_transport.cpp
    src/geocoin/network/secure_channel.cpp
    src/geocoin/network/secure_handshake.cpp
    src/geocoin/network/messages.cpp
    src/geocoin/network/falcon_auth.cpp
    src/geocoin/network/network_utils.cpp
)

add_library(geocoin_network_peer STATIC
    src/geocoin/network/p2p_server.cpp
    src/geocoin/network/peer_manager.cpp
    src/geocoin/network/peer_manager_auth.cpp
    src/geocoin/network/peer_manager_policy.cpp
    src/geocoin/network/peer_discovery.cpp
    src/geocoin/network/dns_resolver.cpp
    src/geocoin/network/addrman.cpp
)

add_library(geocoin_network_txpool STATIC
    src/geocoin/network/mempool.cpp
    src/geocoin/network/fee_estimator.cpp
    src/geocoin/network/mempool_policy.cpp
)

add_library(geocoin_network_sync STATIC
    src/geocoin/network/sync_manager.cpp
    src/geocoin/network/sync_manager_blocks.cpp
    src/geocoin/network/sync_manager_headers.cpp
    src/geocoin/network/sync_manager_orphans.cpp
)

# Compatibility facade for applications that intentionally compose a full
# networking stack. Individual modules and unit tests link focused targets.
add_library(geocoin_network INTERFACE)

add_library(geocoin_node STATIC
    src/geocoin/node/chain_coordinator.cpp
    src/geocoin/node/chain_event_dispatcher.cpp
    src/geocoin/node/node.cpp
    src/geocoin/node/node_connection_plan.cpp
    src/geocoin/node/node_connections.cpp
    src/geocoin/node/node_config_loader.cpp
    src/geocoin/node/node_config_policy.cpp
    src/geocoin/node/node_key.cpp
    src/geocoin/node/node_mining.cpp
    src/geocoin/node/node_peer_messages.cpp
    src/geocoin/node/node_pruning.cpp
    src/geocoin/node/node_runtime_loop.cpp
    src/geocoin/node/node_services.cpp
    src/geocoin/node/node_status.cpp
)

add_library(geocoin_multisig STATIC
    src/geocoin/wallet/multisig.cpp
    src/geocoin/wallet/angle_store.cpp
    src/geocoin/wallet/multisig_verifier.cpp
)

add_library(geocoin_wallet STATIC
    src/geocoin/wallet/wallet.cpp
    src/geocoin/wallet/wallet_keys.cpp
    src/geocoin/wallet/wallet_history.cpp
    src/geocoin/wallet/wallet_nft.cpp
    src/geocoin/wallet/wallet_persist.cpp
    src/geocoin/wallet/wallet_vault.cpp
    src/geocoin/wallet/wallet_persist_io.cpp
    src/geocoin/wallet/wallet_persist_load.cpp
    src/geocoin/wallet/wallet_sync.cpp
    src/geocoin/wallet/wallet_tx.cpp
    src/geocoin/wallet/address_funds.cpp
    src/geocoin/wallet/wallet_application_service.cpp
    src/geocoin/wallet/multisig_service.cpp
)

add_library(geocoin_benchmark STATIC
    src/geocoin/benchmark/node_benchmark.cpp
)

add_library(geocoin_b2b STATIC
    src/geocoin/b2b/corporate_policy.cpp
)

# RPC library
add_library(geocoin_rpc STATIC
    src/geocoin/rpc/rpc_amount.cpp
    src/geocoin/rpc/rpc_command_registry.cpp
    src/geocoin/rpc/rpc_dispatch.cpp
    src/geocoin/rpc/rpc_encoding.cpp
    src/geocoin/rpc/rpc_geography.cpp
    src/geocoin/rpc/rpc_blockchain_format.cpp
    src/geocoin/rpc/rpc_http_server.cpp
    src/geocoin/rpc/rpc_protocol.cpp
    src/geocoin/rpc/rpc_protocol_responses.cpp
    src/geocoin/rpc/rpc_security.cpp
    src/geocoin/rpc/rpc_sensitive_json.cpp
    src/geocoin/rpc/rpc_server.cpp
    src/geocoin/rpc/rpc_server_dispatch.cpp
    src/geocoin/rpc/rpc_server_help.cpp
    src/geocoin/rpc/rpc_server_security.cpp
    src/geocoin/rpc/rpc_modules.cpp
    src/geocoin/rpc/rpc_blockchain.cpp
    src/geocoin/rpc/rpc_blockchain_helpers.cpp
    src/geocoin/rpc/rpc_blockchain_core.cpp
    src/geocoin/rpc/rpc_blockchain_availability.cpp
    src/geocoin/rpc/rpc_blockchain_sidechain.cpp
    src/geocoin/rpc/rpc_blockchain_blocks.cpp
    src/geocoin/rpc/rpc_blockchain_pruning.cpp
    src/geocoin/rpc/rpc_blockchain_transactions.cpp
    src/geocoin/rpc/rpc_wallet_helpers.cpp
    src/geocoin/rpc/rpc_wallet_tx_helpers.cpp
    src/geocoin/rpc/rpc_wallet_sidechain_helpers.cpp
    src/geocoin/rpc/rpc_wallet.cpp
    src/geocoin/rpc/rpc_wallet_exchange.cpp
    src/geocoin/rpc/rpc_wallet_basic_accounts.cpp
    src/geocoin/rpc/rpc_wallet_basic_utxos.cpp
    src/geocoin/rpc/rpc_wallet_basic_transactions.cpp
    src/geocoin/rpc/rpc_wallet_txbuilder.cpp
    src/geocoin/rpc/rpc_wallet_nft.cpp
    src/geocoin/rpc/rpc_wallet_nft_queries.cpp
    src/geocoin/rpc/rpc_wallet_nft_payloads.cpp
    src/geocoin/rpc/rpc_nft_metadata.cpp
    src/geocoin/rpc/rpc_wallet_nft_finalize.cpp
    src/geocoin/rpc/rpc_wallet_nft_transactions.cpp
    src/geocoin/rpc/rpc_wallet_sidechain.cpp
    src/geocoin/rpc/rpc_wallet_sidechain_assets.cpp
    src/geocoin/rpc/rpc_wallet_sidechain_asset_issue.cpp
    src/geocoin/rpc/rpc_wallet_sidechain_asset_mint.cpp
    src/geocoin/rpc/rpc_wallet_sidechain_asset_redeem.cpp
    src/geocoin/rpc/rpc_wallet_sidechain_asset_slash.cpp
    src/geocoin/rpc/rpc_wallet_sidechain_deposits.cpp
    src/geocoin/rpc/rpc_wallet_sidechain_checkpoints.cpp
    src/geocoin/rpc/rpc_wallet_sidechain_checkpoint_single.cpp
    src/geocoin/rpc/rpc_wallet_sidechain_checkpoint_proof.cpp
    src/geocoin/rpc/rpc_wallet_sidechain_checkpoint_batch.cpp
    src/geocoin/rpc/rpc_wallet_sidechain_committee.cpp
    src/geocoin/rpc/rpc_wallet_sidechain_decommission.cpp
    src/geocoin/rpc/rpc_wallet_multisig.cpp
    src/geocoin/rpc/rpc_wallet_multisig_package.cpp
    src/geocoin/rpc/rpc_wallet_multisig_delegation.cpp
    src/geocoin/rpc/rpc_wallet_multisig_legacy.cpp
    src/geocoin/rpc/rpc_mempool.cpp
    src/geocoin/rpc/rpc_mining.cpp
    src/geocoin/rpc/rpc_mining_helpers.cpp
    src/geocoin/rpc/rpc_mining_info.cpp
    src/geocoin/rpc/rpc_mining_control.cpp
    src/geocoin/rpc/rpc_mining_template.cpp
    src/geocoin/rpc/rpc_network.cpp
    src/geocoin/rpc/rpc_misc.cpp
    src/geocoin/rpc/rpc_misc_control.cpp
    src/geocoin/rpc/rpc_misc_diagnostics.cpp
    src/geocoin/rpc/rpc_misc_signing.cpp
)

# Public headers and implementation-only headers are intentionally separated.
# This keeps targets from compiling only because a directory-wide include path
# happened to expose another module's private files.
function(geocoin_configure_library_target target_name)
    target_include_directories(${target_name}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_features(${target_name} PUBLIC cxx_std_20)
endfunction()

foreach(_geocoin_library_target
        geocoin_core
        geocoin_crypto
        geocoin_chain
        geocoin_consensus
        geocoin_blockchain
        geocoin_state
        geocoin_storage
        geocoin_network_transport
        geocoin_network_peer
        geocoin_network_txpool
        geocoin_network_sync
        geocoin_node
        geocoin_multisig
        geocoin_wallet
        geocoin_benchmark
        geocoin_b2b
        geocoin_rpc)
    geocoin_configure_library_target(${_geocoin_library_target})
endforeach()

# Stable, implementation-free API consumed by RPC and GUI adapters.
add_library(geocoin_node_api INTERFACE)
target_include_directories(geocoin_node_api
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_compile_features(geocoin_node_api INTERFACE cxx_std_20)
target_link_libraries(geocoin_node_api INTERFACE geocoin_core)

# Link libraries
#
# We call PQClean Falcon-512 internals (PQCLEAN_FALCON512_CLEAN_*), which are
# not exported from the shared `oqs` library. Link the required liboqs object
# libraries directly so consumers resolve those symbols at link time.
target_link_libraries(
    geocoin_crypto
    PUBLIC
        OpenSSL::SSL
        OpenSSL::Crypto
        falcon_512_clean
        common
        ${GEOCOIN_OPTIONAL_MATH_LIBS}
)
if(TARGET falcon_512_avx2)
    target_link_libraries(geocoin_crypto PUBLIC falcon_512_avx2)
    target_compile_definitions(geocoin_crypto PRIVATE GEOCOIN_FALCON512_AVX2=1)
endif()

# `common` is an OBJECT lib and does not automatically pull in the XKCP low-level
# Keccak objects or optional AVX512VL SHA3 objects; `oqs` normally does that via
# the `COMMON_OBJS` list. We must link these explicitly when bypassing `oqs`.
foreach(_oqs_obj
        ml_kem_768_ref
        ml_kem_768_x86_64
        ml_kem_768_aarch64
        xkcp_low_keccakp_1600_plain64
        xkcp_low_keccakp_1600times4_serial
        xkcp_low_keccakp_1600_avx2
        xkcp_low_keccakp_1600times4_avx2
        sha3_avx512vl_low)
    if(TARGET ${_oqs_obj})
        # Pull object code into our archive without inheriting the mutually
        # exclusive ML-KEM backend compile definitions in every consumer.
        target_sources(geocoin_crypto PRIVATE $<TARGET_OBJECTS:${_oqs_obj}>)
    endif()
endforeach()
target_link_libraries(geocoin_core PUBLIC geocoin_crypto)
target_link_libraries(geocoin_core PRIVATE GeographicLib::GeographicLib)
target_link_libraries(geocoin_core PRIVATE Boost::headers)
geocoin_strict_geodesy(geocoin_core)
target_link_libraries(geocoin_chain geocoin_core)
target_link_libraries(geocoin_consensus geocoin_core geocoin_chain)
target_link_libraries(geocoin_blockchain PUBLIC geocoin_core geocoin_chain geocoin_consensus geocoin_state geocoin_storage)
target_link_libraries(geocoin_multisig PUBLIC geocoin_core geocoin_crypto)
target_link_libraries(geocoin_state geocoin_core geocoin_chain geocoin_multisig geocoin_consensus geocoin_rocksdb geocoin_storage)
target_link_libraries(geocoin_storage geocoin_core geocoin_rocksdb geocoin_consensus)
target_link_libraries(
    geocoin_network_transport
    PUBLIC
        geocoin_core
        geocoin_crypto
        geocoin_consensus
)
target_link_libraries(
    geocoin_network_peer
    PUBLIC
        geocoin_network_transport
    PRIVATE
        geocoin_state
        geocoin_consensus
)
target_link_libraries(
    geocoin_network_txpool
    PUBLIC
        geocoin_core
        geocoin_consensus
        geocoin_state
)
target_link_libraries(
    geocoin_network_sync
    PUBLIC
        geocoin_network_transport
        geocoin_blockchain
        geocoin_state
        geocoin_consensus
)
target_link_libraries(
    geocoin_network
    INTERFACE
        geocoin_network_transport
        geocoin_network_peer
        geocoin_network_txpool
        geocoin_network_sync
)
target_link_libraries(geocoin_node PUBLIC geocoin_node_api geocoin_blockchain geocoin_state geocoin_storage geocoin_network geocoin_consensus geocoin_chain)
target_link_libraries(geocoin_wallet PUBLIC geocoin_core geocoin_crypto geocoin_multisig geocoin_state)
if(WIN32)
    target_link_libraries(geocoin_node PUBLIC advapi32)
    target_link_libraries(geocoin_wallet PUBLIC advapi32)
endif()
target_link_libraries(geocoin_benchmark geocoin_core geocoin_crypto geocoin_rocksdb geocoin_storage)
target_link_libraries(geocoin_b2b PUBLIC geocoin_crypto)
target_link_libraries(
    geocoin_rpc
    geocoin_core
    geocoin_blockchain
    geocoin_wallet
    geocoin_state
    geocoin_network_transport
    geocoin_network_peer
    geocoin_network_txpool
    geocoin_storage
    geocoin_node_api
    geocoin_rocksdb
    Threads::Threads
)
