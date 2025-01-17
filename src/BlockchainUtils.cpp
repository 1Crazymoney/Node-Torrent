#include "BlockchainUtils.h"

#include <memory>

#include <cstring>
#include <openssl/ripemd.h>
#include <openssl/sha.h>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>
#include <openssl/bn.h>
#include <openssl/x509v3.h>
#include <openssl/x509.h>
#include <openssl/objects.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/opensslconf.h>

#include <secp256k1.h>

#include <atomic>

#include "log.h"
#include "check.h"
#include "stringUtils.h"
#include "convertStrings.h"

using namespace common;

namespace torrent_node_lib {
    
static bool isInitialized = false;

static const secp256k1_context* getCtx() {
    static thread_local std::unique_ptr<secp256k1_context, decltype(&secp256k1_context_destroy)> s_ctx{
        secp256k1_context_create(SECP256K1_CONTEXT_VERIFY | SECP256K1_CONTEXT_SIGN),
        &secp256k1_context_destroy
    };
    return s_ctx.get();
}

std::vector<unsigned char> hex2bin(const std::string & src) {
    static const unsigned char DecLookup[256] = {
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, // gap before first hex digit
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,1,2,3,4,5,6,7,8,9,       // 0123456789
        0,0,0,0,0,0,0,             // :;<=>?@ (gap)
        10,11,12,13,14,15,         // ABCDEF
        0,0,0,0,0,0,0,0,0,0,0,0,0, // GHIJKLMNOPQRS (gap)
        0,0,0,0,0,0,0,0,0,0,0,0,0, // TUVWXYZ[/]^_` (gap)
        10,11,12,13,14,15,         // abcdef
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 // fill zeroes
    };
    
    uint i = 0;
    if (src.size() > 2 && src[0] == '0' && src[1] == 'x') {
        i = 2;
    }
    
    std::vector<unsigned char> dest;
    dest.reserve(src.length()/2);
    for (; i < src.length(); i += 2 ) {
        unsigned char d =  DecLookup[(unsigned char)src[i]] << 4;
        d |= DecLookup[(unsigned char)src[i + 1]];
        dest.push_back(d);
    }
    
    return dest;
}

void ssl_init() {
    SSL_load_error_strings();
    SSL_library_init();
    OpenSSL_add_all_algorithms();
}


std::array<unsigned char, 32> get_double_sha256(unsigned char * data, size_t size) {
    CHECK(isInitialized, "Not initialized");
    
    std::array<unsigned char, SHA256_DIGEST_LENGTH> hash1;
    std::array<unsigned char, SHA256_DIGEST_LENGTH> hash2;
    
    // First pass
    SHA256(data, size, hash1.data());
    
    // Second pass
    SHA256(hash1.data(), SHA256_DIGEST_LENGTH, hash2.data());
    
    return hash2;
}


bool IsValidECKey(EVP_PKEY* key) {
    CHECK(isInitialized, "Not initialized");
    
    bool rslt = false;
    EC_KEY *ec_key = EVP_PKEY_get1_EC_KEY(key);
    if (ec_key) {
        rslt = (EC_KEY_check_key(ec_key) == 1);
        EC_KEY_free(ec_key);
    }
    return rslt;
}

EVP_PKEY* ReadPublicKey(const std::vector<unsigned char> & binary) {
    CHECK(isInitialized, "Not initialized");
    
    const unsigned char * data = binary.data();
    if (binary.size()) {
        EVP_PKEY* key = d2i_PUBKEY(NULL, const_cast<const unsigned char **>(&data), binary.size());
        if (key) {
            return key;
        }
    }
    
    return NULL;
}


std::string get_address(const std::string & pubk) {
    std::vector<unsigned char> binary = hex2bin(pubk);
    return get_address(binary);
}

std::string makeAddressFromSecpKey(const std::vector<unsigned char> &pubkey) {
    secp256k1_pubkey pubkeySecp;
    CHECK(secp256k1_ec_pubkey_parse(getCtx(), &pubkeySecp, pubkey.data(), pubkey.size()) == 1, "Incorrect pubkey");
    std::vector<unsigned char> pubkeyData(65);
    size_t size = pubkeyData.size();
    CHECK(secp256k1_ec_pubkey_serialize(getCtx(), pubkeyData.data(), &size, &pubkeySecp, SECP256K1_EC_UNCOMPRESSED) == 1, "Incorrect pubkey");
    const std::string calculatedAddress = torrent_node_lib::get_address(pubkeyData);
    return calculatedAddress;
}

static bool get_address_comp(const std::vector<unsigned char> & bpubk, std::array<unsigned char, RIPEMD160_DIGEST_LENGTH + 1> &wide_h, std::array<unsigned char, SHA256_DIGEST_LENGTH> &hash2) {
    CHECK(isInitialized, "Not initialized");
    
    std::vector<unsigned char> binary(bpubk);
    unsigned char* data = binary.data();
    int datasize = binary.size();
    if (data && datasize >= 65) {
        data[datasize - 65] = 0x04;
        
        std::array<unsigned char, SHA256_DIGEST_LENGTH> sha_1;
        std::array<unsigned char, RIPEMD160_DIGEST_LENGTH> r160;
        
        SHA256(data + (datasize - 65), 65, sha_1.data());
        RIPEMD160(sha_1.data(), SHA256_DIGEST_LENGTH, r160.data());
        
        wide_h[0] = 0;
        for (size_t i = 0; i < RIPEMD160_DIGEST_LENGTH; i++) {
            wide_h[i + 1] = r160[i];
        }
        
        std::array<unsigned char, SHA256_DIGEST_LENGTH> hash1;
        SHA256(wide_h.data(), RIPEMD160_DIGEST_LENGTH + 1, hash1.data());
        
        SHA256(hash1.data(), SHA256_DIGEST_LENGTH, hash2.data());
        return true;
    }
    
    return false;
}

std::string get_address(const std::vector<unsigned char> & bpubk) {
    std::array<unsigned char, RIPEMD160_DIGEST_LENGTH + 1> wide_h;
    std::array<unsigned char, SHA256_DIGEST_LENGTH> hash2;
    const bool res = get_address_comp(bpubk, wide_h, hash2);
    if (!res) {
        return "Invalid";
    }
            
    std::string address;
    {
        address.reserve(55);
        
        address.insert(address.end(), '0');
        address.insert(address.end(), 'x');
        
        static const char HexLookup[513]= {
            "000102030405060708090a0b0c0d0e0f"
            "101112131415161718191a1b1c1d1e1f"
            "202122232425262728292a2b2c2d2e2f"
            "303132333435363738393a3b3c3d3e3f"
            "404142434445464748494a4b4c4d4e4f"
            "505152535455565758595a5b5c5d5e5f"
            "606162636465666768696a6b6c6d6e6f"
            "707172737475767778797a7b7c7d7e7f"
            "808182838485868788898a8b8c8d8e8f"
            "909192939495969798999a9b9c9d9e9f"
            "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf"
            "b0b1b2b3b4b5b6b7b8b9babbbcbdbebf"
            "c0c1c2c3c4c5c6c7c8c9cacbcccdcecf"
            "d0d1d2d3d4d5d6d7d8d9dadbdcdddedf"
            "e0e1e2e3e4e5e6e7e8e9eaebecedeeef"
            "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff"
        };
        
        for (uint i = 0; i < wide_h.size(); i++) {
            const char * hex = HexLookup + 2 * wide_h[i];
            address.insert(address.end(), hex, hex + 2);
        }
        
        
        for (size_t i = 0; i < 4; i++) {
            const char * hex = HexLookup + 2 * hash2[i];
            address.insert(address.end(), hex, hex + 2);
        }
    }
    
    return address;
}

std::vector<unsigned char> get_address_bin(const std::vector<unsigned char> & bpubk) {
    std::array<unsigned char, RIPEMD160_DIGEST_LENGTH + 1> wide_h;
    std::array<unsigned char, SHA256_DIGEST_LENGTH> hash2;
    const bool res = get_address_comp(bpubk, wide_h, hash2);
    if (!res) {
        return {};
    }
    
    std::vector<unsigned char> result(wide_h.begin(), wide_h.end());
    result.insert(result.end(), hash2.begin(), hash2.begin() + 4);
    
    return result;
}

/*std::vector<unsigned char> crypto_sign_data(
    const std::vector<unsigned char>& private_key, 
    const unsigned char *data,
    size_t data_size)
{
    secp256k1_pubkey pubkeySecp;
    CHECK(secp256k1_ec_pubkey_parse(getCtx(), &pubkeySecp, public_key.data() + public_key.size() - 65, 65) == 1, "Incorrect publik key");
    secp256k1_ecdsa_signature signSecp;
    CHECK(secp256k1_ecdsa_signature_parse_der(getCtx(), &signSecp, (const unsigned char*)sign.data(), sign.size()) == 1, "Incorrect sign key");
    secp256k1_ecdsa_signature sig_norm;
    secp256k1_ecdsa_signature_normalize(getCtx(), &sig_norm, &signSecp);
    
    std::array<unsigned char, SHA256_DIGEST_LENGTH> hash;
    SHA256(data, data_size, hash.data());
    
    const int result = secp256k1_ecdsa_verify(getCtx(), &sig_norm, hash.data(), &pubkeySecp);
    if (result == 1) {
        return true;
    } else if (result == 0) {
        return false;
    } else {
        throwErr("Serious error of signature verification");
    }
}*/

static std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> crypto_load_key(
    const std::vector<unsigned char> &key_buf,
    bool pubkey)
{
    CHECK(!key_buf.empty(), "empty key_buf");
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new_mem_buf(key_buf.data(), key_buf.size()), BIO_free);
    CHECK(bio != nullptr, "Cannot create BIO object");
    
    if(pubkey) {
        return std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>(d2i_PUBKEY_bio(bio.get(), NULL), EVP_PKEY_free);
    } else {
        return std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>(d2i_PrivateKey_bio(bio.get(), NULL), EVP_PKEY_free);
    }
}

static std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> crypto_load_public_key(const std::vector<unsigned char> &public_key_buf) {
    return crypto_load_key(public_key_buf, true);
}

bool CheckBufferSignature(EVP_PKEY* publicKey, const std::vector<char>& data, ECDSA_SIG* signature) {
    size_t bufsize = data.size();
    const char* buff = data.data();
    
    EVP_MD_CTX* mdctx;
    static const EVP_MD* md = EVP_sha256();
    unsigned char md_value[EVP_MAX_MD_SIZE];
    unsigned int md_len;
    
    md = EVP_sha256();
    mdctx = EVP_MD_CTX_create();
    EVP_DigestInit_ex(mdctx, md, nullptr);
    EVP_DigestUpdate(mdctx, buff, bufsize);
    EVP_DigestFinal_ex(mdctx, md_value, &md_len);
    EVP_MD_CTX_destroy(mdctx);
    
    EC_KEY* ec_key = EVP_PKEY_get1_EC_KEY(publicKey);
    if (ECDSA_do_verify(md_value, md_len, signature, ec_key) == 1) {
        EC_KEY_free(ec_key);
        return true;
    }
    EC_KEY_free(ec_key);
    return false;
}

template <typename SignContainer>
ECDSA_SIG* ReadSignature(const SignContainer& binsign) {
    const auto* data = reinterpret_cast<const unsigned char*>(binsign.data());
    
    ECDSA_SIG* signature = d2i_ECDSA_SIG(nullptr, &data, binsign.size());
    return signature;
}

template <typename PubKContainer>
EVP_PKEY* ReadPublicKey(const PubKContainer& binpubk) {
    const auto* data = reinterpret_cast<const unsigned char*>(binpubk.data());
    
    EVP_PKEY* key = d2i_PUBKEY(nullptr, &data, binpubk.size());
    return key;
}

template <typename PrivKContainer>
EVP_PKEY* ReadPrivateKey(const PrivKContainer& binprivk) {
    const auto* data = reinterpret_cast<const unsigned char*>(binprivk.data());
    
    EVP_PKEY* key = d2i_AutoPrivateKey(nullptr, &data, binprivk.size());
    return key;
}

template <typename DataContainer, typename SignContainer, typename PubKContainer>
bool check_sign(const DataContainer& data, const SignContainer& sign, const PubKContainer& pubk) {
    EVP_PKEY* pubkey = ReadPublicKey(pubk);
    if (!pubkey) {
        return false;
    }
    ECDSA_SIG* signature = ReadSignature(sign);
    if (!signature) {
        EVP_PKEY_free(pubkey);
        return false;
    }
    
    std::vector<char> data_as_vector;
    data_as_vector.insert(data_as_vector.end(), data.begin(), data.end());
    
    if (CheckBufferSignature(pubkey, data_as_vector, signature)) {
        EVP_PKEY_free(pubkey);
        ECDSA_SIG_free(signature);
        return true;
    }
    
    EVP_PKEY_free(pubkey);
    ECDSA_SIG_free(signature);
    return false;
}

bool crypto_check_sign_data_old1(
    const std::vector<char>& sign, 
    const std::vector<unsigned char>& public_key, 
    const unsigned char *data,
    size_t data_size)
{
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> mdctx(EVP_MD_CTX_create(), EVP_MD_CTX_free);
    CHECK(mdctx != nullptr, "Cannot create digest context");
    
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> pkey = crypto_load_public_key(public_key);
    CHECK(pkey != nullptr, "Cannot load pulic key");
    
    const bool res1 = EVP_DigestVerifyInit(mdctx.get(), NULL, EVP_sha256(), NULL, pkey.get());
    CHECK(res1 == 1, "Cannot initialise SHA-256 verifying");
    
    const bool res2 = EVP_DigestVerifyUpdate(mdctx.get(), data, data_size);
    CHECK(res2 == 1, "Cannot verify the data");
    
    const int rc = EVP_DigestVerifyFinal(mdctx.get(), (const unsigned char*)sign.data(), sign.size());
    if(rc == 0) {
        return false;
    } else if(rc == 1) {
        return true;
    } else {
        throwErr("Serious error of signature verification");
    }
}

bool crypto_check_sign_data_old2(
    std::vector<char> sign, 
    const std::vector<unsigned char>& public_key, 
    const unsigned char *data,
    size_t data_size)
{
    secp256k1_pubkey pubkeySecp;
    if (secp256k1_ec_pubkey_parse(getCtx(), &pubkeySecp, public_key.data() + public_key.size() - 65, 65) != 1) {
        return crypto_check_sign_data_old1(sign, public_key, data, data_size);
    }
    
    secp256k1_ecdsa_signature signSecp;
    
    while (secp256k1_ecdsa_signature_parse_der(getCtx(), &signSecp, (const unsigned char*)sign.data(), sign.size()) != 1) {
        if (sign.size() < 70) {
            throwErr("Incorrect signature");
        }
        sign.resize(sign.size() - 1);
    }
    secp256k1_ecdsa_signature sig_norm;
    secp256k1_ecdsa_signature_normalize(getCtx(), &sig_norm, &signSecp);
    
    std::array<unsigned char, SHA256_DIGEST_LENGTH> hash;
    SHA256(data, data_size, hash.data());
    
    const int result = secp256k1_ecdsa_verify(getCtx(), &sig_norm, hash.data(), &pubkeySecp);
    if (result == 1) {
        return true;
    } else {
        return false;
    }
}

bool crypto_check_sign_data(
    std::vector<char> sign, 
    const std::vector<unsigned char>& public_key, 
    const unsigned char *data,
    size_t data_size)
{
    return check_sign(std::vector(data, data + data_size), sign, public_key);
}

void initBlockchainUtilsImpl() {
    ssl_init();
    isInitialized = true;
}

template<typename PODType>
size_t NumberSize(const PODType* value) {
    PODType val = *value;
    size_t currVal = 0;
    while (val != 0) {
        currVal++;
        val /= 256;
    }
    return currVal;
}

template <typename IntType>
std::string IntegerToBuffer(IntType val)
{
    uint8_t buf[sizeof(IntType)] = {0};
    IntType value = val;
    uint8_t rem = 0;
    for (size_t i = 0; i < sizeof(IntType); ++i)
    {
        rem = value % 256;
        buf[i] = rem;
        value = value / 256;
    }
    return std::string((char*)buf, sizeof(IntType));
}

std::string EncodeField(std::string field) {
    size_t fs = field.size();
    std::string rslt = "";
    if (fs == 1 && field.at(0) >= 0x0 && (uint8_t)field.at(0) <= 0x7F) {
        if (field.at(0) == 0) {
            rslt += 0x80;
        } else {
            rslt += field;
        }
    } else if (fs <= 55) {
        char sz = 0x80 + char(fs);
        rslt += sz;
        rslt += field;
    } else if (fs > 55 && fs < 0xFFFFFFFFFFFFFFFF) {
        size_t sizelen = NumberSize(&fs);
        
        const std::string bigint = IntegerToBuffer(fs);
        
        char prefix = 0xB7 + char(sizelen);
        rslt += prefix;
        rslt += bigint.substr(0, sizelen);
        rslt += field;
    }
    
    return rslt;
}

std::string CalcTotalSize(std::string dump) {
    std::string rslt = "";
    size_t ds = dump.size();
    if (ds <= 55) {
        char sz = 0xC0 + char(ds);
        rslt += sz;
        rslt += dump;
    } else {
        size_t sizelen = NumberSize(&ds);
        
        const std::string bigint = IntegerToBuffer(ds);
        
        char prefix = 0xF7 + char(sizelen);
        rslt += prefix;
        rslt += bigint.substr(0, sizelen);
        rslt += dump;
    }
    
    return rslt;
}

std::string RLP(const std::vector<std::string> fields) {
    std::string dump = "";
    for (size_t i = 0; i < fields.size(); ++i) {
        dump += EncodeField(fields[i]);
    }
    dump = CalcTotalSize(dump);
    return dump;
}

std::string IntToRLP(int val) {
    if (val == 0)
        return std::string(1, '\x00');
    uint8_t rlpval[sizeof(val)];
    unsigned char* valptr = (unsigned char*)&val + sizeof(val) - 1;
    
    size_t j = 0;
    bool start = false;
    for (size_t i = 0; i < sizeof(val); ++i)
    {
        if (*(valptr-i))
            start = true;
        if (start)
            rlpval[j++] = *(valptr-i);
    }
    
    return std::string((const char*)rlpval, j);
}

std::string doubleSha(const std::string& data) {
    std::string hash1(SHA256_DIGEST_LENGTH, 0);
    std::string hash2(SHA256_DIGEST_LENGTH, 0);
    SHA256((const unsigned char*)(data.data()), data.size(), (unsigned char*)(hash1.data()));
    SHA256((const unsigned char*)(hash1.data()), SHA256_DIGEST_LENGTH, (unsigned char*)(hash2.data()));
    return hash2;
}

std::string keccak(const std::string& input) {
    uint32_t digest_length = SHA256_DIGEST_LENGTH;
    const EVP_MD* algorithm = EVP_sha3_256();
    uint8_t* digest = static_cast<uint8_t*>(OPENSSL_malloc(digest_length));
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    EVP_DigestInit_ex(context, algorithm, nullptr);
    EVP_DigestUpdate(context, input.c_str(), input.size());
    EVP_DigestFinal_ex(context, digest, &digest_length);
    EVP_MD_CTX_destroy(context);
    std::string output(digest, digest + digest_length);
    OPENSSL_free(digest);
    return output;
}

std::string create_custom_address(const std::string &binowner, int nonce, char number) {
    std::vector<std::string> fields;
    fields.push_back(binowner);
    if (nonce > 0) {
        std::string rlpnonce = IntToRLP(nonce);
        fields.push_back(rlpnonce);
    } else {
        fields.push_back("");
    }
    std::string rlpenc = RLP(fields);
    const std::string hs = keccak(rlpenc);
    std::string address;
    address += number;
    address += hs.substr(12, 20);
    address += doubleSha(address).substr(0, 4);
    return address;
}

}
