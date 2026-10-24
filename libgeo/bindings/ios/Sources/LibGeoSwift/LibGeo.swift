import Foundation
import CLibGeo

public enum GeoError: Error {
    case failed(code: Int32, message: String)
    case invalidInput(String)
}

public enum LibGeo {
    public static var apiVersion: UInt32 {
        geo_api_version()
    }

    public static func verifyAddress(_ address: String) throws -> Bool {
        var valid: Int32 = 0
        let code = address.withCString { cstr in
            geo_verify_address(cstr, &valid)
        }
        guard code == GEO_OK else {
            throw GeoError.failed(code: Int32(code.rawValue), message: lastError())
        }
        return valid != 0
    }

    public static func deriveAddress(privateKey32: Data) throws -> String {
        guard privateKey32.count == 32 else {
            throw GeoError.invalidInput("privateKey32 must be 32 bytes")
        }

        var out = [CChar](repeating: 0, count: Int(LIBGEO_ADDRESS_MAX_LEN))
        let code = privateKey32.withUnsafeBytes { ptr in
            geo_derive_address_from_private_key(
                ptr.bindMemory(to: UInt8.self).baseAddress,
                privateKey32.count,
                &out,
                out.count
            )
        }

        guard code == GEO_OK else {
            throw GeoError.failed(code: Int32(code.rawValue), message: lastError())
        }
        return String(cString: out)
    }

    public static func signDigest32Recoverable(digest32: Data, privateKey32: Data) throws -> Data {
        guard digest32.count == 32 else {
            throw GeoError.invalidInput("digest32 must be 32 bytes")
        }
        guard privateKey32.count == 32 else {
            throw GeoError.invalidInput("privateKey32 must be 32 bytes")
        }

        var sig = [UInt8](repeating: 0, count: 4096)
        var written: Int = 0

        let code = digest32.withUnsafeBytes { dptr in
            privateKey32.withUnsafeBytes { kptr in
                geo_sign_digest32_recoverable(
                    dptr.bindMemory(to: UInt8.self).baseAddress,
                    kptr.bindMemory(to: UInt8.self).baseAddress,
                    privateKey32.count,
                    &sig,
                    sig.count,
                    &written
                )
            }
        }

        guard code == GEO_OK else {
            throw GeoError.failed(code: Int32(code.rawValue), message: lastError())
        }

        return Data(sig.prefix(written))
    }

    public static func verifySignedMessage(digest32: Data, signature: Data) throws -> Bool {
        guard digest32.count == 32 else {
            throw GeoError.invalidInput("digest32 must be 32 bytes")
        }

        var valid: Int32 = 0
        let code = digest32.withUnsafeBytes { dptr in
            signature.withUnsafeBytes { sptr in
                geo_verify_signed_message(
                    dptr.bindMemory(to: UInt8.self).baseAddress,
                    sptr.bindMemory(to: UInt8.self).baseAddress,
                    signature.count,
                    &valid
                )
            }
        }

        guard code == GEO_OK else {
            throw GeoError.failed(code: Int32(code.rawValue), message: lastError())
        }
        return valid != 0
    }

    public static func recoverSignerAddress(digest32: Data, signature: Data) throws -> String {
        guard digest32.count == 32 else {
            throw GeoError.invalidInput("digest32 must be 32 bytes")
        }

        var out = [CChar](repeating: 0, count: Int(LIBGEO_ADDRESS_MAX_LEN))
        let code = digest32.withUnsafeBytes { dptr in
            signature.withUnsafeBytes { sptr in
                geo_recover_signer_address(
                    dptr.bindMemory(to: UInt8.self).baseAddress,
                    sptr.bindMemory(to: UInt8.self).baseAddress,
                    signature.count,
                    &out,
                    out.count
                )
            }
        }

        guard code == GEO_OK else {
            throw GeoError.failed(code: Int32(code.rawValue), message: lastError())
        }

        return String(cString: out)
    }

    public static func multisigCreate(m: UInt8, n: UInt8, participants: [String]) throws -> String {
        guard participants.count == Int(n) else {
            throw GeoError.invalidInput("participants.count must equal n")
        }

        var out = [CChar](repeating: 0, count: Int(LIBGEO_ADDRESS_MAX_LEN))
        let cStrings = participants.map { strdup($0) }
        defer {
            for ptr in cStrings where ptr != nil {
                free(ptr)
            }
        }

        var ptrs = cStrings.map { UnsafePointer<CChar>($0) }

        let code = ptrs.withUnsafeMutableBufferPointer { buf in
            geo_multisig_create(
                m,
                n,
                buf.baseAddress,
                &out,
                out.count
            )
        }

        guard code == GEO_OK else {
            throw GeoError.failed(code: Int32(code.rawValue), message: lastError())
        }
        return String(cString: out)
    }

    public static func multisigVerify(_ multisigAddress: String) throws -> Bool {
        var valid: Int32 = 0
        let code = multisigAddress.withCString { cstr in
            geo_multisig_verify(cstr, &valid)
        }
        guard code == GEO_OK else {
            throw GeoError.failed(code: Int32(code.rawValue), message: lastError())
        }
        return valid != 0
    }

    public static func txHashFromRaw(_ rawTx: Data) throws -> Data {
        var out = [UInt8](repeating: 0, count: 32)
        let code = rawTx.withUnsafeBytes { ptr in
            geo_tx_hash_from_raw(
                ptr.bindMemory(to: UInt8.self).baseAddress,
                rawTx.count,
                &out
            )
        }
        guard code == GEO_OK else {
            throw GeoError.failed(code: Int32(code.rawValue), message: lastError())
        }
        return Data(out)
    }

    public static func txSignInput(txHash32: Data, inputIndex: UInt64, privateKey32: Data) throws -> Data {
        guard txHash32.count == 32 else {
            throw GeoError.invalidInput("txHash32 must be 32 bytes")
        }
        guard privateKey32.count == 32 else {
            throw GeoError.invalidInput("privateKey32 must be 32 bytes")
        }

        var sig = [UInt8](repeating: 0, count: 4096)
        var written: Int = 0

        let code = txHash32.withUnsafeBytes { hptr in
            privateKey32.withUnsafeBytes { kptr in
                geo_tx_sign_input(
                    hptr.bindMemory(to: UInt8.self).baseAddress,
                    inputIndex,
                    kptr.bindMemory(to: UInt8.self).baseAddress,
                    privateKey32.count,
                    &sig,
                    sig.count,
                    &written
                )
            }
        }
        guard code == GEO_OK else {
            throw GeoError.failed(code: Int32(code.rawValue), message: lastError())
        }
        return Data(sig.prefix(written))
    }

    public static func txVerifyInputSignature(txHash32: Data, inputIndex: UInt64, signature: Data) throws -> Bool {
        guard txHash32.count == 32 else {
            throw GeoError.invalidInput("txHash32 must be 32 bytes")
        }
        var valid: Int32 = 0
        let code = txHash32.withUnsafeBytes { hptr in
            signature.withUnsafeBytes { sptr in
                geo_tx_verify_input_signature(
                    hptr.bindMemory(to: UInt8.self).baseAddress,
                    inputIndex,
                    sptr.bindMemory(to: UInt8.self).baseAddress,
                    signature.count,
                    &valid
                )
            }
        }
        guard code == GEO_OK else {
            throw GeoError.failed(code: Int32(code.rawValue), message: lastError())
        }
        return valid != 0
    }

    public static func txRecoverInputSignerAddress(txHash32: Data, inputIndex: UInt64, signature: Data) throws -> String {
        guard txHash32.count == 32 else {
            throw GeoError.invalidInput("txHash32 must be 32 bytes")
        }
        var out = [CChar](repeating: 0, count: Int(LIBGEO_ADDRESS_MAX_LEN))
        let code = txHash32.withUnsafeBytes { hptr in
            signature.withUnsafeBytes { sptr in
                geo_tx_recover_input_signer_address(
                    hptr.bindMemory(to: UInt8.self).baseAddress,
                    inputIndex,
                    sptr.bindMemory(to: UInt8.self).baseAddress,
                    signature.count,
                    &out,
                    out.count
                )
            }
        }
        guard code == GEO_OK else {
            throw GeoError.failed(code: Int32(code.rawValue), message: lastError())
        }
        return String(cString: out)
    }

    public static func lastError() -> String {
        guard let ptr = geo_last_error() else {
            return ""
        }
        return String(cString: ptr)
    }
}
