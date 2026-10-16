#include "StateCrypto.h"
#ifdef Q_OS_IOS
extern "C" int society_state_crypt(int encrypt, const char *key, const char *input, int size,
                                   const char *context, int contextSize, char *output);
#else
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <memory>
#endif

namespace {
QByteArray crypt(bool encrypt, const QByteArray &input, const QByteArray &key, const QByteArray &context) {
    if (key.size() != 32 || input.size() > MaximumSocietyStateBytes + (encrypt ? 0 : 28)
        || (!encrypt && input.size() < 28)) return {};
    QByteArray output(encrypt ? input.size() + 28 : input.size() - 28, Qt::Uninitialized);
#ifdef Q_OS_IOS
    const auto size = society_state_crypt(encrypt, key.constData(), input.constData(), int(input.size()),
                                         context.constData(), int(context.size()), output.data());
    if (size < 0) return {};
    output.resize(size);
#else
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> cipher(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!cipher) return {};
    auto *nonce = encrypt ? reinterpret_cast<unsigned char *>(output.data())
                          : reinterpret_cast<unsigned char *>(const_cast<char *>(input.constData()));
    if (encrypt && RAND_bytes(nonce, 12) != 1) return {};
    if (EVP_CipherInit_ex(cipher.get(), EVP_aes_256_gcm(), nullptr,
        reinterpret_cast<const unsigned char *>(key.constData()), nonce, encrypt) != 1) return {};
    int count = 0;
    if (EVP_CipherUpdate(cipher.get(), nullptr, &count,
        reinterpret_cast<const unsigned char *>(context.constData()), int(context.size())) != 1) return {};
    if (!encrypt && EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_SET_TAG, 16,
        const_cast<char *>(input.constData() + input.size() - 16)) != 1) return {};
    auto *destination = reinterpret_cast<unsigned char *>(output.data() + (encrypt ? 12 : 0));
    const auto *source = reinterpret_cast<const unsigned char *>(input.constData() + (encrypt ? 0 : 12));
    if (EVP_CipherUpdate(cipher.get(), destination, &count, source, int(encrypt ? input.size() : input.size() - 28)) != 1) return {};
    int final = 0;
    if (EVP_CipherFinal_ex(cipher.get(), destination + count, &final) != 1) return {};
    if (encrypt && EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_GET_TAG, 16, output.data() + output.size() - 16) != 1) return {};
#endif
    return output;
}
}
QByteArray sealSocietyState(const QByteArray &plain, const QByteArray &key, const QByteArray &context) {
    return crypt(true, plain, key, context);
}
QByteArray openSocietyState(const QByteArray &sealed, const QByteArray &key, const QByteArray &context) {
    return crypt(false, sealed, key, context);
}
