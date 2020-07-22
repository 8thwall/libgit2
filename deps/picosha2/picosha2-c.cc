#include "picosha2-c.h"
#include "picosha2.h"

#include <vector>
#include <string>
#include <cstring>

extern "C" {

void picosha2_256(const char* buffer, int len, char* dest) {
  std::vector<uint8_t> hash(picosha2::k_digest_size);
  picosha2::hash256(buffer, buffer + len, hash.begin(), hash.end());
  std::string sha256 = picosha2::bytes_to_hex_string(hash.begin(), hash.end());
  std::memcpy(dest, sha256.c_str(), sha256.size() + 1);
}

}
