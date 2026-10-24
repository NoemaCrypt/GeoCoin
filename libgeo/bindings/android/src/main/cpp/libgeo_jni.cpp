#include <jni.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "libgeo/libgeo.h"

namespace {

std::vector<uint8_t> jbytearray_to_vec(JNIEnv* env, jbyteArray arr) {
    const jsize len = env->GetArrayLength(arr);
    std::vector<uint8_t> out(static_cast<size_t>(len));
    if (len > 0) {
        env->GetByteArrayRegion(arr, 0, len, reinterpret_cast<jbyte*>(out.data()));
    }
    return out;
}

jbyteArray vec_to_jbytearray(JNIEnv* env, const uint8_t* data, size_t len) {
    jbyteArray arr = env->NewByteArray(static_cast<jsize>(len));
    if (arr == nullptr) {
        return nullptr;
    }
    if (len > 0) {
        env->SetByteArrayRegion(arr, 0, static_cast<jsize>(len), reinterpret_cast<const jbyte*>(data));
    }
    return arr;
}

jstring new_string_or_null(JNIEnv* env, const char* s) {
    if (s == nullptr) {
        return nullptr;
    }
    return env->NewStringUTF(s);
}

}  // namespace

extern "C" JNIEXPORT jint JNICALL
Java_com_geocoin_libgeo_LibGeo_nativeApiVersion(JNIEnv*, jclass) {
    return static_cast<jint>(geo_api_version());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_geocoin_libgeo_LibGeo_nativeLastError(JNIEnv* env, jclass) {
    return new_string_or_null(env, geo_last_error());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_geocoin_libgeo_LibGeo_nativeVerifyAddress(JNIEnv* env, jclass, jstring address) {
    if (address == nullptr) {
        return JNI_FALSE;
    }

    const char* addr_chars = env->GetStringUTFChars(address, nullptr);
    if (addr_chars == nullptr) {
        return JNI_FALSE;
    }

    int is_valid = 0;
    const geo_status_t st = geo_verify_address(addr_chars, &is_valid);
    env->ReleaseStringUTFChars(address, addr_chars);

    return (st == GEO_OK && is_valid != 0) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_geocoin_libgeo_LibGeo_nativeDeriveAddressFromPrivateKey(JNIEnv* env, jclass, jbyteArray private_key) {
    if (private_key == nullptr) {
        return nullptr;
    }

    std::vector<uint8_t> pk = jbytearray_to_vec(env, private_key);
    char out[LIBGEO_ADDRESS_MAX_LEN] = {0};

    const geo_status_t st = geo_derive_address_from_private_key(pk.data(), pk.size(), out, sizeof(out));
    if (st != GEO_OK) {
        return nullptr;
    }

    return env->NewStringUTF(out);
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_geocoin_libgeo_LibGeo_nativeSignDigest32Recoverable(
    JNIEnv* env,
    jclass,
    jbyteArray digest32,
    jbyteArray private_key
) {
    if (digest32 == nullptr || private_key == nullptr) {
        return nullptr;
    }

    std::vector<uint8_t> msg = jbytearray_to_vec(env, digest32);
    std::vector<uint8_t> pk = jbytearray_to_vec(env, private_key);
    if (msg.size() != 32) {
        return nullptr;
    }

    std::array<uint8_t, 4096> sig_buf{};
    size_t sig_written = 0;

    const geo_status_t st = geo_sign_digest32_recoverable(
        msg.data(),
        pk.data(),
        pk.size(),
        sig_buf.data(),
        sig_buf.size(),
        &sig_written
    );
    if (st != GEO_OK) {
        return nullptr;
    }

    return vec_to_jbytearray(env, sig_buf.data(), sig_written);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_geocoin_libgeo_LibGeo_nativeVerifySignedMessage(
    JNIEnv* env,
    jclass,
    jbyteArray digest32,
    jbyteArray signature
) {
    if (digest32 == nullptr || signature == nullptr) {
        return JNI_FALSE;
    }

    std::vector<uint8_t> msg = jbytearray_to_vec(env, digest32);
    std::vector<uint8_t> sig = jbytearray_to_vec(env, signature);
    if (msg.size() != 32) {
        return JNI_FALSE;
    }

    int is_valid = 0;
    const geo_status_t st = geo_verify_signed_message(msg.data(), sig.data(), sig.size(), &is_valid);
    return (st == GEO_OK && is_valid != 0) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_geocoin_libgeo_LibGeo_nativeRecoverSignerAddress(
    JNIEnv* env,
    jclass,
    jbyteArray digest32,
    jbyteArray signature
) {
    if (digest32 == nullptr || signature == nullptr) {
        return nullptr;
    }

    std::vector<uint8_t> msg = jbytearray_to_vec(env, digest32);
    std::vector<uint8_t> sig = jbytearray_to_vec(env, signature);
    if (msg.size() != 32) {
        return nullptr;
    }

    char out[LIBGEO_ADDRESS_MAX_LEN] = {0};
    const geo_status_t st = geo_recover_signer_address(msg.data(), sig.data(), sig.size(), out, sizeof(out));
    if (st != GEO_OK) {
        return nullptr;
    }

    return env->NewStringUTF(out);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_geocoin_libgeo_LibGeo_nativeMultisigCreate(
    JNIEnv* env,
    jclass,
    jint m,
    jint n,
    jobjectArray participants
) {
    if (participants == nullptr) {
        return nullptr;
    }

    const jsize size = env->GetArrayLength(participants);
    if (size != n) {
        return nullptr;
    }

    std::vector<std::string> storage;
    std::vector<const char*> ptrs;
    storage.reserve(static_cast<size_t>(size));
    ptrs.reserve(static_cast<size_t>(size));

    for (jsize i = 0; i < size; ++i) {
        auto* s = static_cast<jstring>(env->GetObjectArrayElement(participants, i));
        if (s == nullptr) {
            return nullptr;
        }
        const char* chars = env->GetStringUTFChars(s, nullptr);
        if (chars == nullptr) {
            env->DeleteLocalRef(s);
            return nullptr;
        }
        storage.emplace_back(chars);
        env->ReleaseStringUTFChars(s, chars);
        env->DeleteLocalRef(s);
    }

    for (const auto& it : storage) {
        ptrs.push_back(it.c_str());
    }

    char out[LIBGEO_ADDRESS_MAX_LEN] = {0};
    const geo_status_t st = geo_multisig_create(
        static_cast<uint8_t>(m),
        static_cast<uint8_t>(n),
        ptrs.data(),
        out,
        sizeof(out)
    );
    if (st != GEO_OK) {
        return nullptr;
    }

    return env->NewStringUTF(out);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_geocoin_libgeo_LibGeo_nativeMultisigVerify(JNIEnv* env, jclass, jstring multisig_address) {
    if (multisig_address == nullptr) {
        return JNI_FALSE;
    }

    const char* chars = env->GetStringUTFChars(multisig_address, nullptr);
    if (chars == nullptr) {
        return JNI_FALSE;
    }

    int is_valid = 0;
    const geo_status_t st = geo_multisig_verify(chars, &is_valid);
    env->ReleaseStringUTFChars(multisig_address, chars);

    return (st == GEO_OK && is_valid != 0) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_geocoin_libgeo_LibGeo_nativeTxHashFromRaw(JNIEnv* env, jclass, jbyteArray raw_tx) {
    if (raw_tx == nullptr) {
        return nullptr;
    }
    std::vector<uint8_t> raw = jbytearray_to_vec(env, raw_tx);
    std::array<uint8_t, 32> out{};
    const geo_status_t st = geo_tx_hash_from_raw(raw.data(), raw.size(), out.data());
    if (st != GEO_OK) {
        return nullptr;
    }
    return vec_to_jbytearray(env, out.data(), out.size());
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_geocoin_libgeo_LibGeo_nativeTxSignInput(
    JNIEnv* env,
    jclass,
    jbyteArray tx_hash32,
    jlong input_index,
    jbyteArray private_key
) {
    if (tx_hash32 == nullptr || private_key == nullptr) {
        return nullptr;
    }

    std::vector<uint8_t> hash = jbytearray_to_vec(env, tx_hash32);
    std::vector<uint8_t> pk = jbytearray_to_vec(env, private_key);
    if (hash.size() != 32) {
        return nullptr;
    }

    std::array<uint8_t, 4096> sig_buf{};
    size_t sig_written = 0;
    const geo_status_t st = geo_tx_sign_input(
        hash.data(),
        static_cast<uint64_t>(input_index),
        pk.data(),
        pk.size(),
        sig_buf.data(),
        sig_buf.size(),
        &sig_written
    );
    if (st != GEO_OK) {
        return nullptr;
    }
    return vec_to_jbytearray(env, sig_buf.data(), sig_written);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_geocoin_libgeo_LibGeo_nativeTxVerifyInputSignature(
    JNIEnv* env,
    jclass,
    jbyteArray tx_hash32,
    jlong input_index,
    jbyteArray signature
) {
    if (tx_hash32 == nullptr || signature == nullptr) {
        return JNI_FALSE;
    }
    std::vector<uint8_t> hash = jbytearray_to_vec(env, tx_hash32);
    std::vector<uint8_t> sig = jbytearray_to_vec(env, signature);
    if (hash.size() != 32) {
        return JNI_FALSE;
    }

    int valid = 0;
    const geo_status_t st = geo_tx_verify_input_signature(
        hash.data(),
        static_cast<uint64_t>(input_index),
        sig.data(),
        sig.size(),
        &valid
    );
    return (st == GEO_OK && valid != 0) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_geocoin_libgeo_LibGeo_nativeTxRecoverInputSignerAddress(
    JNIEnv* env,
    jclass,
    jbyteArray tx_hash32,
    jlong input_index,
    jbyteArray signature
) {
    if (tx_hash32 == nullptr || signature == nullptr) {
        return nullptr;
    }
    std::vector<uint8_t> hash = jbytearray_to_vec(env, tx_hash32);
    std::vector<uint8_t> sig = jbytearray_to_vec(env, signature);
    if (hash.size() != 32) {
        return nullptr;
    }

    char out[LIBGEO_ADDRESS_MAX_LEN] = {0};
    const geo_status_t st = geo_tx_recover_input_signer_address(
        hash.data(),
        static_cast<uint64_t>(input_index),
        sig.data(),
        sig.size(),
        out,
        sizeof(out)
    );
    if (st != GEO_OK) {
        return nullptr;
    }
    return env->NewStringUTF(out);
}
