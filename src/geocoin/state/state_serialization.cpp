#include "geocoin/state/state_serialization_internal.hpp"

#include <stdexcept>
#include <string>

namespace geocoin::state {

void read_deposit_undo_payload(
    ByteReader& reader,
    TxUndo::DepositUndo& out,
    const char* label
) {
    out.sidechain_id = reader.read_hash256(label);
    const uint32_t owner_len = reader.read_u32_le("deposit owner length");
    out.prev_owner = reader.read_string(owner_len, "deposit owner");
    out.prev_amount = reader.read_amount("deposit amount");
    out.prev_unlock_height = reader.read_u64_le("deposit unlock height");
    out.prev_checkpoint = reader.read_hash256("deposit checkpoint");

    out.prev_last_checkpoint_height = reader.read_u64_le("deposit last checkpoint height");
    out.prev_last_sidechain_checkpoint_height =
        reader.read_u64_le("deposit last sidechain checkpoint height");
}

core::CardAuthority read_card_authority(ByteReader& reader) {
    core::CardAuthority authority;
    authority.quorum = reader.read_u8("card authority quorum");
    const uint32_t count = reader.read_u32_le("card authority member count");
    if (count == 0 || count > consensus::params::MAX_CARD_AUTHORITY_MEMBERS) {
        throw std::runtime_error("TxUndo: invalid card authority member count");
    }
    authority.members.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        const uint32_t length = reader.read_u32_le("card authority member length");
        const std::string address = reader.read_string(length, "card authority member");
        if (address.empty() || address.size() > consensus::params::MAX_ADDRESS_SIZE) {
            throw std::runtime_error("TxUndo: invalid card authority member");
        }
        authority.members.emplace_back(Address(address));
    }
    return authority;
}

CardProgramEntry read_card_program(ByteReader& reader) {
    CardProgramEntry program;
    const uint32_t issuer_length = reader.read_u32_le("card program issuer length");
    program.issuer = Address(reader.read_string(issuer_length, "card program issuer"));
    const uint32_t vendor_length = reader.read_u32_le("card program vendor length");
    program.chip_vendor = reader.read_string(vendor_length, "card program vendor");
    const uint32_t os_length = reader.read_u32_le("card program os length");
    program.chip_os = reader.read_string(os_length, "card program os");
    const uint32_t aid_length = reader.read_u32_le("card program aid length");
    program.applet_aid = reader.read_string(aid_length, "card program aid");
    program.applet_version = reader.read_u32_le("card program applet version");
    program.applet_package_hash = reader.read_hash256("card program package hash");
    program.policy_id = reader.read_u32_le("card program policy id");
    program.scope_id = reader.read_hash256("card program scope id");
    program.created_height = reader.read_u64_le("card program created height");
    program.corporate_policy.version = reader.read_u8("card program corporate policy version");
    if (program.corporate_policy.version == 0) {
        return program;
    }
    if (program.corporate_policy.version != 1) {
        throw std::runtime_error("TxUndo: invalid card corporate policy version");
    }
    program.corporate_policy.mint = read_card_authority(reader);
    program.corporate_policy.freeze = read_card_authority(reader);
    program.corporate_policy.operations = read_card_authority(reader);
    program.corporate_policy.recovery = read_card_authority(reader);
    program.corporate_policy.compliance_policy_hash = reader.read_hash256("card compliance policy hash");
    program.corporate_policy.max_active_cards = reader.read_u32_le("card max active cards");
    return program;
}

bytes TxUndo::serialize() const {
    bytes data;

    if ((deposit_undo.kind != 0 && deposit_undo.prev_amount < 0) ||
        (sidechain_undo.kind != 0 &&
         (sidechain_undo.prev_limit < 0 || sidechain_undo.prev_issued < 0))) {
        throw std::runtime_error("TxUndo: negative monetary state");
    }
    if (sidechain_undo.prev_accounting_namespace.size() >
        consensus::params::MAX_SIDECHAIN_NAMESPACE) {
        throw std::runtime_error("TxUndo: accounting namespace too long");
    }
    for (const auto& undo : deposit_undo_batch) {
        if (undo.kind != 0 && undo.prev_amount < 0) {
            throw std::runtime_error("TxUndo: negative batch deposit amount");
        }
    }

    // Количество UTXO (4 байта)
    uint32_t count = spent_utxos.size();
    for (int i = 0; i < 4; i++) {
        data.push_back((count >> (i * 8)) & 0xFF);
    }

    // Сериализуем каждый UTXO
    for (const auto& utxo : spent_utxos) {
        bytes utxo_data = utxo.serialize();

        // Длина UTXO (2 байта)
        uint16_t len = utxo_data.size();
        data.push_back(len & 0xFF);
        data.push_back((len >> 8) & 0xFF);

        // Данные UTXO
        data.insert(data.end(), utxo_data.begin(), utxo_data.end());
    }

    // Undo по NFT
    data.push_back(nft_undo.kind);
    if (nft_undo.kind != 0) {
        data.insert(data.end(), nft_undo.nft_id.begin(), nft_undo.nft_id.end());
        uint32_t owner_len = static_cast<uint32_t>(nft_undo.prev_owner.size());
        for (int i = 0; i < 4; i++) {
            data.push_back((owner_len >> (i * 8)) & 0xFF);
        }
        data.insert(data.end(), nft_undo.prev_owner.begin(), nft_undo.prev_owner.end());

        uint32_t meta_len = static_cast<uint32_t>(nft_undo.prev_metadata.size());
        for (int i = 0; i < 4; i++) {
            data.push_back((meta_len >> (i * 8)) & 0xFF);
        }
        data.insert(data.end(), nft_undo.prev_metadata.begin(), nft_undo.prev_metadata.end());
    }

    // Undo по депозитам сайдчейна
    data.push_back(deposit_undo.kind);
    if (deposit_undo.kind != 0) {
        data.insert(data.end(), deposit_undo.sidechain_id.begin(), deposit_undo.sidechain_id.end());

        uint32_t owner_len = static_cast<uint32_t>(deposit_undo.prev_owner.size());
        for (int i = 0; i < 4; i++) {
            data.push_back((owner_len >> (i * 8)) & 0xFF);
        }
        data.insert(data.end(), deposit_undo.prev_owner.begin(), deposit_undo.prev_owner.end());

        for (int i = 0; i < 8; i++) {
            data.push_back((static_cast<uint64_t>(deposit_undo.prev_amount) >> (i * 8)) & 0xFF);
        }

        for (int i = 0; i < 8; i++) {
            data.push_back((static_cast<uint64_t>(deposit_undo.prev_unlock_height) >> (i * 8)) & 0xFF);
        }

        data.insert(data.end(), deposit_undo.prev_checkpoint.begin(), deposit_undo.prev_checkpoint.end());

        for (int i = 0; i < 8; i++) {
            data.push_back((static_cast<uint64_t>(deposit_undo.prev_last_checkpoint_height) >> (i * 8)) & 0xFF);
        }
        for (int i = 0; i < 8; i++) {
            data.push_back((static_cast<uint64_t>(deposit_undo.prev_last_sidechain_checkpoint_height) >> (i * 8)) & 0xFF);
        }
    }

    // Batch undo по депозитам (опциональный блок)
    data.push_back(0xBC);
    uint32_t batch_count = static_cast<uint32_t>(deposit_undo_batch.size());
    for (int i = 0; i < 4; i++) {
        data.push_back((batch_count >> (i * 8)) & 0xFF);
    }
    for (const auto& du : deposit_undo_batch) {
        data.push_back(du.kind);
        if (du.kind == 0) {
            continue;
        }
        data.insert(data.end(), du.sidechain_id.begin(), du.sidechain_id.end());
        uint32_t owner_len = static_cast<uint32_t>(du.prev_owner.size());
        for (int i = 0; i < 4; i++) {
            data.push_back((owner_len >> (i * 8)) & 0xFF);
        }
        data.insert(data.end(), du.prev_owner.begin(), du.prev_owner.end());
        for (int i = 0; i < 8; i++) {
            data.push_back((static_cast<uint64_t>(du.prev_amount) >> (i * 8)) & 0xFF);
        }
        for (int i = 0; i < 8; i++) {
            data.push_back((static_cast<uint64_t>(du.prev_unlock_height) >> (i * 8)) & 0xFF);
        }
        data.insert(data.end(), du.prev_checkpoint.begin(), du.prev_checkpoint.end());
        for (int i = 0; i < 8; i++) {
            data.push_back((static_cast<uint64_t>(du.prev_last_checkpoint_height) >> (i * 8)) & 0xFF);
        }
        for (int i = 0; i < 8; i++) {
            data.push_back((static_cast<uint64_t>(du.prev_last_sidechain_checkpoint_height) >> (i * 8)) & 0xFF);
        }
    }

    // Undo по реестру сайдчейна
    data.push_back(sidechain_undo.kind);
    if (sidechain_undo.kind != 0) {
        data.insert(data.end(), sidechain_undo.sidechain_id.begin(), sidechain_undo.sidechain_id.end());

        uint32_t owner_len = static_cast<uint32_t>(sidechain_undo.prev_owner.size());
        for (int i = 0; i < 4; i++) {
            data.push_back((owner_len >> (i * 8)) & 0xFF);
        }
        data.insert(data.end(), sidechain_undo.prev_owner.begin(), sidechain_undo.prev_owner.end());

        for (int i = 0; i < 8; i++) {
            data.push_back((static_cast<uint64_t>(sidechain_undo.prev_limit) >> (i * 8)) & 0xFF);
        }

        for (int i = 0; i < 8; i++) {
            data.push_back((static_cast<uint64_t>(sidechain_undo.prev_issued) >> (i * 8)) & 0xFF);
        }

        for (int i = 0; i < 4; i++) {
            data.push_back((static_cast<uint32_t>(sidechain_undo.prev_multiplier) >> (i * 8)) & 0xFF);
        }
        for (int i = 0; i < 4; i++) {
            data.push_back((static_cast<uint32_t>(sidechain_undo.prev_fee_bps) >> (i * 8)) & 0xFF);
        }
        for (int i = 0; i < 4; i++) {
            data.push_back((static_cast<uint32_t>(sidechain_undo.prev_max_multiplier) >> (i * 8)) & 0xFF);
        }

        uint32_t sym_len = static_cast<uint32_t>(sidechain_undo.prev_symbol.size());
        for (int i = 0; i < 4; i++) {
            data.push_back((sym_len >> (i * 8)) & 0xFF);
        }
        data.insert(data.end(), sidechain_undo.prev_symbol.begin(), sidechain_undo.prev_symbol.end());

        data.push_back(sidechain_undo.prev_committee_m);
        data.push_back(sidechain_undo.prev_committee_n);
        uint32_t count = static_cast<uint32_t>(sidechain_undo.prev_committee_members.size());
        for (int i = 0; i < 4; i++) {
            data.push_back((count >> (i * 8)) & 0xFF);
        }
        for (const auto& p : sidechain_undo.prev_committee_members) {
            uint32_t len = static_cast<uint32_t>(p.size());
            for (int i = 0; i < 4; i++) {
                data.push_back((len >> (i * 8)) & 0xFF);
            }
            data.insert(data.end(), p.begin(), p.end());
        }
        data.insert(data.end(), sidechain_undo.prev_committee_id.begin(), sidechain_undo.prev_committee_id.end());
    }

    // Undo по фонду
    data.push_back(foundation_undo.kind);
    if (foundation_undo.kind != 0) {
        uint32_t addr_len = static_cast<uint32_t>(foundation_undo.prev_address.size());
        for (int i = 0; i < 4; i++) {
            data.push_back((addr_len >> (i * 8)) & 0xFF);
        }
        data.insert(data.end(), foundation_undo.prev_address.begin(), foundation_undo.prev_address.end());

        data.push_back(foundation_undo.prev_m);
        data.push_back(foundation_undo.prev_n);

        uint32_t count = static_cast<uint32_t>(foundation_undo.prev_participants.size());
        for (int i = 0; i < 4; i++) {
            data.push_back((count >> (i * 8)) & 0xFF);
        }
        for (const auto& p : foundation_undo.prev_participants) {
            uint32_t len = static_cast<uint32_t>(p.size());
            for (int i = 0; i < 4; i++) {
                data.push_back((len >> (i * 8)) & 0xFF);
            }
            data.insert(data.end(), p.begin(), p.end());
        }
    }

    // Undo по карточному state-layer (опциональный теговый блок)
    data.push_back(0xC4);
    data.push_back(card_undo.kind);
    if (card_undo.kind != 0) {
        data.insert(data.end(), card_undo.program_id.begin(), card_undo.program_id.end());
        data.insert(data.end(), card_undo.card_uid_hash.begin(), card_undo.card_uid_hash.end());
        data.insert(data.end(), card_undo.card_pubkey_hash.begin(), card_undo.card_pubkey_hash.end());
        uint32_t issuer_len = static_cast<uint32_t>(card_undo.prev_issuer.size());
        for (int i = 0; i < 4; i++) {
            data.push_back((issuer_len >> (i * 8)) & 0xFF);
        }
        data.insert(data.end(), card_undo.prev_issuer.begin(), card_undo.prev_issuer.end());
        for (int i = 0; i < 8; i++) {
            data.push_back((card_undo.prev_valid_from >> (i * 8)) & 0xFF);
        }
        for (int i = 0; i < 8; i++) {
            data.push_back((card_undo.prev_valid_to >> (i * 8)) & 0xFF);
        }
        data.push_back(card_undo.prev_status);
        data.insert(data.end(), card_undo.prev_metadata_hash.begin(), card_undo.prev_metadata_hash.end());
        for (int i = 0; i < 8; i++) {
            data.push_back((static_cast<uint64_t>(card_undo.prev_issued_height) >> (i * 8)) & 0xFF);
        }
        for (int i = 0; i < 8; i++) {
            data.push_back((static_cast<uint64_t>(card_undo.prev_updated_height) >> (i * 8)) & 0xFF);
        }
        if (card_undo.kind == 4) {
            append_card_program(data, card_undo.prev_program);
        }
    }

    // Undo по witness pubkey cache (новые ключи, добавленные этой tx)
    data.push_back(0xD1);
    uint32_t pk_count = static_cast<uint32_t>(added_witness_pubkeys.size());
    for (int i = 0; i < 4; i++) {
        data.push_back((pk_count >> (i * 8)) & 0xFF);
    }
    for (const auto& h : added_witness_pubkeys) {
        data.insert(data.end(), h.begin(), h.end());
    }

    // Checkpoint policy/version are required whenever sidechain undo is present.
    if (sidechain_undo.kind != 0) {
        data.push_back(kTxUndoSidechainPolicyTagLocal);
        data.push_back(sidechain_undo.prev_checkpoint_policy_id);
        data.push_back(sidechain_undo.prev_checkpoint_version);

        data.push_back(kTxUndoSidechainRegistryMetaTagLocal);
        data.push_back(kTxUndoSidechainRegistryMetaVersionLocal);
        data.push_back(static_cast<uint8_t>(sidechain_undo.prev_kind));
        data.push_back(static_cast<uint8_t>(sidechain_undo.prev_status));
        for (int i = 0; i < 8; ++i) {
            data.push_back(
                (static_cast<uint64_t>(sidechain_undo.prev_decommission_height) >>
                 (i * 8)) & 0xFF
            );
        }
        const uint32_t namespace_len =
            static_cast<uint32_t>(sidechain_undo.prev_accounting_namespace.size());
        for (int i = 0; i < 4; ++i) {
            data.push_back((namespace_len >> (i * 8)) & 0xFF);
        }
        data.insert(
            data.end(),
            sidechain_undo.prev_accounting_namespace.begin(),
            sidechain_undo.prev_accounting_namespace.end()
        );
        data.insert(
            data.end(),
            sidechain_undo.prev_accounting_schema_hash.begin(),
            sidechain_undo.prev_accounting_schema_hash.end()
        );
    }

    return data;
}

TxUndo TxUndo::deserialize(const bytes& data) {
    ByteReader reader(data, "TxUndo");
    TxUndo tx_undo;

    // Количество UTXO (4 bytes)
    uint32_t count = reader.read_u32_le("UTXO count");
    if (count > reader.remaining() / 2) {
        throw std::runtime_error("TxUndo: invalid UTXO count");
    }

    // Десериализуем каждый UTXO
    for (uint32_t i = 0; i < count; i++) {
        // Длина UTXO (2 bytes)
        uint16_t len = reader.read_u16_le("UTXO length");
        if (len == 0) {
            throw std::runtime_error("TxUndo: empty UTXO payload");
        }

        // Данные UTXO
        bytes utxo_data = reader.read_bytes(len, "UTXO payload");
        tx_undo.spent_utxos.push_back(UTXO::deserialize(utxo_data));
    }

    // Undo по NFT
    {
        tx_undo.nft_undo.kind = reader.read_u8("nft undo kind");
        if (tx_undo.nft_undo.kind > 3) {
            throw std::runtime_error("TxUndo: invalid nft undo kind");
        }
        if (tx_undo.nft_undo.kind != 0) {
            tx_undo.nft_undo.nft_id = reader.read_hash256("nft_id");
            uint32_t owner_len = reader.read_u32_le("nft owner length");
            tx_undo.nft_undo.prev_owner = reader.read_string(owner_len, "nft owner");
            uint32_t meta_len = reader.read_u32_le("nft metadata length");
            tx_undo.nft_undo.prev_metadata = reader.read_bytes(meta_len, "nft metadata");
        }
    }

    // Undo по депозитам сайдчейна
    {
        tx_undo.deposit_undo.kind = reader.read_u8("deposit undo kind");
        if (tx_undo.deposit_undo.kind > 3) {
            throw std::runtime_error("TxUndo: invalid deposit undo kind");
        }
        if (tx_undo.deposit_undo.kind != 0) {
            read_deposit_undo_payload(
                reader,
                tx_undo.deposit_undo,
                "deposit sidechain_id"
            );
        }
    }

    // Batch undo по депозитам сайдчейна (tagged extension).
    {
        if (reader.read_u8("deposit batch tag") != kTxUndoDepositBatchTagLocal)
            throw std::runtime_error("TxUndo: invalid deposit batch tag");
        uint32_t batch_count = reader.read_u32_le("deposit batch count");
        if (batch_count > reader.remaining()) {
            throw std::runtime_error("TxUndo: invalid deposit batch count");
        }
        tx_undo.deposit_undo_batch.clear();
        tx_undo.deposit_undo_batch.reserve(batch_count);
        for (uint32_t i = 0; i < batch_count; ++i) {
            TxUndo::DepositUndo du;
            du.kind = reader.read_u8("deposit batch undo kind");
            if (du.kind > 3) {
                throw std::runtime_error("TxUndo: invalid deposit batch undo kind");
            }
            if (du.kind != 0) {
                read_deposit_undo_payload(
                    reader,
                    du,
                    "deposit batch sidechain_id"
                );
            }
            tx_undo.deposit_undo_batch.push_back(std::move(du));
        }
    }

    // Undo по реестру сайдчейна
    {
        tx_undo.sidechain_undo.kind = reader.read_u8("sidechain undo kind");
        if (tx_undo.sidechain_undo.kind > 2) {
            throw std::runtime_error("TxUndo: invalid sidechain undo kind");
        }
        if (tx_undo.sidechain_undo.kind != 0) {
            tx_undo.sidechain_undo.sidechain_id = reader.read_hash256("sidechain id");
            uint32_t owner_len = reader.read_u32_le("sidechain owner length");
            tx_undo.sidechain_undo.prev_owner = reader.read_string(owner_len, "sidechain owner");
            tx_undo.sidechain_undo.prev_limit = reader.read_amount("sidechain limit");
            tx_undo.sidechain_undo.prev_issued = reader.read_amount("sidechain issued");
            tx_undo.sidechain_undo.prev_multiplier = reader.read_u32_le("sidechain multiplier");
            tx_undo.sidechain_undo.prev_fee_bps = reader.read_u32_le("sidechain fee bps");
            tx_undo.sidechain_undo.prev_max_multiplier = reader.read_u32_le("sidechain max multiplier");
            uint32_t sym_len = reader.read_u32_le("sidechain symbol length");
            tx_undo.sidechain_undo.prev_symbol = reader.read_string(sym_len, "sidechain symbol");
            tx_undo.sidechain_undo.prev_committee_m = reader.read_u8("sidechain committee m");
            tx_undo.sidechain_undo.prev_committee_n = reader.read_u8("sidechain committee n");

            uint32_t count = reader.read_u32_le("sidechain committee count");
            if (count > reader.remaining() / 4) {
                throw std::runtime_error("TxUndo: invalid committee count");
            }
            tx_undo.sidechain_undo.prev_committee_members.clear();
            tx_undo.sidechain_undo.prev_committee_members.reserve(count);
            for (uint32_t i = 0; i < count; i++) {
                uint32_t len = reader.read_u32_le("committee member length");
                tx_undo.sidechain_undo.prev_committee_members.push_back(
                    reader.read_string(len, "committee member")
                );
            }
            tx_undo.sidechain_undo.prev_committee_id = reader.read_hash256("committee id");
        }
    }

    // Undo по фонду
    {
        tx_undo.foundation_undo.kind = reader.read_u8("foundation undo kind");
        if (tx_undo.foundation_undo.kind > 1) {
            throw std::runtime_error("TxUndo: invalid foundation undo kind");
        }
        if (tx_undo.foundation_undo.kind != 0) {
            uint32_t addr_len = reader.read_u32_le("foundation address length");
            tx_undo.foundation_undo.prev_address =
                reader.read_string(addr_len, "foundation address");
            tx_undo.foundation_undo.prev_m = reader.read_u8("foundation m");
            tx_undo.foundation_undo.prev_n = reader.read_u8("foundation n");

            uint32_t count = reader.read_u32_le("foundation participants count");
            if (count > reader.remaining() / 4) {
                throw std::runtime_error("TxUndo: invalid foundation participants count");
            }
            tx_undo.foundation_undo.prev_participants.clear();
            tx_undo.foundation_undo.prev_participants.reserve(count);
            for (uint32_t i = 0; i < count; i++) {
                uint32_t len = reader.read_u32_le("foundation participant length");
                tx_undo.foundation_undo.prev_participants.push_back(
                    reader.read_string(len, "foundation participant")
                );
            }
        }
    }

    // Undo по карточному state-layer
    {
        if (reader.read_u8("card undo tag") != kTxUndoCardTagLocal)
            throw std::runtime_error("TxUndo: invalid card undo tag");
        tx_undo.card_undo.kind = reader.read_u8("card undo kind");
        if (tx_undo.card_undo.kind > 4) {
            throw std::runtime_error("TxUndo: invalid card undo kind");
        }
        if (tx_undo.card_undo.kind != 0) {
            tx_undo.card_undo.program_id = reader.read_hash256("card program id");
            tx_undo.card_undo.card_uid_hash = reader.read_hash256("card uid hash");
            tx_undo.card_undo.card_pubkey_hash = reader.read_hash256("card pubkey hash");
            uint32_t issuer_len = reader.read_u32_le("card issuer length");
            tx_undo.card_undo.prev_issuer = reader.read_string(issuer_len, "card issuer");
            tx_undo.card_undo.prev_valid_from = reader.read_u64_le("card valid from");
            tx_undo.card_undo.prev_valid_to = reader.read_u64_le("card valid to");
            tx_undo.card_undo.prev_status = reader.read_u8("card status");
            tx_undo.card_undo.prev_metadata_hash = reader.read_hash256("card metadata hash");
            tx_undo.card_undo.prev_issued_height = reader.read_u64_le("card issued height");
            tx_undo.card_undo.prev_updated_height = reader.read_u64_le("card updated height");
            if (tx_undo.card_undo.kind == 4) {
                tx_undo.card_undo.prev_program = read_card_program(reader);
            }
        }
    }

    // Undo по witness pubkey cache
    {
        if (reader.read_u8("witness pubkey tag") != kTxUndoWitnessPubkeyTagLocal)
            throw std::runtime_error("TxUndo: invalid witness pubkey tag");
        uint32_t pk_count = reader.read_u32_le("witness pubkey count");
        if (pk_count > reader.remaining() / 32) {
            throw std::runtime_error("TxUndo: invalid witness pubkey count");
        }
        tx_undo.added_witness_pubkeys.clear();
        tx_undo.added_witness_pubkeys.reserve(pk_count);
        for (uint32_t i = 0; i < pk_count; i++) {
            tx_undo.added_witness_pubkeys.push_back(
                reader.read_hash256("witness pubkey hash")
            );
        }
    }

    if (tx_undo.sidechain_undo.kind != 0) {
        if (reader.read_u8("sidechain policy tag") != kTxUndoSidechainPolicyTagLocal)
            throw std::runtime_error("TxUndo: invalid sidechain policy tag");
        if (tx_undo.sidechain_undo.kind == 0) {
            throw std::runtime_error("TxUndo: sidechain policy without sidechain undo");
        }
        tx_undo.sidechain_undo.prev_checkpoint_policy_id =
            reader.read_u8("sidechain checkpoint policy");
        tx_undo.sidechain_undo.prev_checkpoint_version =
            reader.read_u8("sidechain checkpoint version");
    }

    if (tx_undo.sidechain_undo.kind != 0) {
        if (reader.read_u8("sidechain registry meta tag") != kTxUndoSidechainRegistryMetaTagLocal)
            throw std::runtime_error("TxUndo: invalid sidechain registry tag");
        if (tx_undo.sidechain_undo.kind == 0 ||
            reader.read_u8("sidechain registry meta version") !=
                kTxUndoSidechainRegistryMetaVersionLocal) {
            throw std::runtime_error("TxUndo: invalid sidechain registry metadata");
        }
        const uint8_t kind_byte = reader.read_u8("sidechain kind");
        const uint8_t status_byte = reader.read_u8("sidechain status");
        if (kind_byte > static_cast<uint8_t>(core::SidechainKind::ACCOUNTING) ||
            status_byte > static_cast<uint8_t>(SidechainStatus::DECOMMISSIONING)) {
            throw std::runtime_error("TxUndo: invalid sidechain kind/status");
        }
        tx_undo.sidechain_undo.prev_kind =
            static_cast<core::SidechainKind>(kind_byte);
        tx_undo.sidechain_undo.prev_status =
            static_cast<SidechainStatus>(status_byte);
        tx_undo.sidechain_undo.prev_decommission_height =
            reader.read_u64_le("sidechain decommission height");
        const uint32_t namespace_len =
            reader.read_u32_le("sidechain accounting namespace length");
        if (namespace_len > consensus::params::MAX_SIDECHAIN_NAMESPACE) {
            throw std::runtime_error("TxUndo: accounting namespace too long");
        }
        tx_undo.sidechain_undo.prev_accounting_namespace =
            reader.read_string(namespace_len, "sidechain accounting namespace");
        tx_undo.sidechain_undo.prev_accounting_schema_hash =
            reader.read_hash256("sidechain accounting schema hash");
    }

    if (!reader.empty()) {
        throw std::runtime_error("TxUndo: trailing data");
    }

    return tx_undo;
}

bytes BlockUndo::serialize() const {
    bytes data;

    // height (8 байт)
    for (int i = 0; i < 8; i++) {
        data.push_back((height >> (i * 8)) & 0xFF);
    }

    // block_hash (32 байта)
    data.insert(data.end(), block_hash.begin(), block_hash.end());

    // Количество транзакций (4 bytes)
    uint32_t count = tx_undos.size();
    for (int i = 0; i < 4; i++) {
        data.push_back((count >> (i * 8)) & 0xFF);
    }

    // Сериализуем каждый TxUndo
    for (const auto& tx_undo : tx_undos) {
        bytes tx_undo_data = tx_undo.serialize();

        // Длина TxUndo (4 bytes)
        uint32_t len = tx_undo_data.size();
        for (int i = 0; i < 4; i++) {
            data.push_back((len >> (i * 8)) & 0xFF);
        }

        // Данные TxUndo
        data.insert(data.end(), tx_undo_data.begin(), tx_undo_data.end());
    }

    // Опциональный хвост v2: изменение interest pool этим блоком.
    data.push_back(kBlockUndoInterestPoolTagLocal);
    for (int i = 0; i < 8; i++) {
        data.push_back((static_cast<uint64_t>(interest_pool_delta) >> (i * 8)) & 0xFF);
    }

    return data;
}

BlockUndo BlockUndo::deserialize(const bytes& data) {
    ByteReader reader(data, "BlockUndo");
    BlockUndo block_undo;

    // height (8 байт)
    block_undo.height = reader.read_u64_le("height");

    // block_hash (32 байта)
    block_undo.block_hash = reader.read_hash256("block_hash");

    // Количество транзакций (4 bytes)
    uint32_t count = reader.read_u32_le("tx undo count");
    if (count > reader.remaining() / 4) {
        throw std::runtime_error("BlockUndo: invalid tx_undo count");
    }

    // Десериализуем каждый TxUndo
    block_undo.tx_undos.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        // Длина TxUndo (4 bytes)
        uint32_t len = reader.read_u32_le("tx_undo length");
        if (len == 0) {
            throw std::runtime_error("BlockUndo: empty tx_undo payload");
        }

        // Данные TxUndo
        bytes tx_undo_data = reader.read_bytes(len, "tx_undo payload");

        block_undo.tx_undos.push_back(TxUndo::deserialize(tx_undo_data));
    }

    {
        if (reader.read_u8("interest pool tag") != kBlockUndoInterestPoolTagLocal)
            throw std::runtime_error("BlockUndo: invalid interest pool tag");
        block_undo.interest_pool_delta = reader.read_i64_le("interest_pool_delta");
    }

    if (!reader.empty()) {
        throw std::runtime_error("BlockUndo: trailing data");
    }

    return block_undo;
}

} // namespace geocoin::state
