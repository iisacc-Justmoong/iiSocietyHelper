import Foundation
import CryptoKit

// AES-GCM's combined form is nonce(12) + ciphertext + tag(16), matching OpenSSL.
@_cdecl("society_state_crypt")
func societyStateCrypt(_ encrypt: Int32, _ key: UnsafePointer<UInt8>,
                       _ input: UnsafePointer<UInt8>, _ size: Int32,
                       _ context: UnsafePointer<UInt8>, _ contextSize: Int32,
                       _ output: UnsafeMutablePointer<UInt8>) -> Int32 {
    do {
        let secret = SymmetricKey(data: Data(bytes: key, count: 32))
        let data = Data(bytes: input, count: Int(size))
        let aad = Data(bytes: context, count: Int(contextSize))
        let result: Data
        if encrypt != 0 {
            guard let combined = try AES.GCM.seal(data, using: secret, authenticating: aad).combined else { return -1 }
            result = combined
        } else {
            result = try AES.GCM.open(AES.GCM.SealedBox(combined: data), using: secret, authenticating: aad)
        }
        result.copyBytes(to: output, count: result.count)
        return Int32(result.count)
    } catch { return -1 }
}
