#include "File.h"
#include <openssl/md5.h>
#include <fstream>

std::string generateFileHash(const std::string& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file) return "";

    unsigned char digest[MD5_DIGEST_LENGTH];
    MD5_CTX md5Context;
    MD5_Init(&md5Context);

    char buffer[1024];
    while (file.read(buffer, sizeof(buffer))) {
        MD5_Update(&md5Context, buffer, file.gcount());
    }
    MD5_Final(digest, &md5Context);

    char md5String[33];
    for (int i = 0; i < 16; ++i) {
        sprintf(&md5String[i*2], "%02x", digest[i]);
    }
    return md5String;
}