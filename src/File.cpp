#include "File.h"
#include <openssl/evp.h>
#include <fstream>
#include <iomanip>
#include <sstream>

std::string File::generateFileHash(const std::string& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file) return "";

    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    const EVP_MD* md = EVP_md5();
    unsigned char md_value[EVP_MAX_MD_SIZE];
    unsigned int md_len;

    EVP_DigestInit_ex(mdctx, md, NULL);

    char buffer[1024];
    while (file.read(buffer, sizeof(buffer))) {
        EVP_DigestUpdate(mdctx, buffer, file.gcount());
    }

    EVP_DigestFinal_ex(mdctx, md_value, &md_len);
    EVP_MD_CTX_free(mdctx);

    std::stringstream ss;
    for(unsigned int i = 0; i < md_len; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)md_value[i];
    }

    return ss.str();
}