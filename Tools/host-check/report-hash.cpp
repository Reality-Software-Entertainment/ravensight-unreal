#include "../../Source/Ravensight/Private/RavensightReportHash.h"
#include <cassert>
#include <iostream>
int main() {
 using RavensightReportHash::sha256_hex;
 assert(sha256_hex("")=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
 assert(sha256_hex("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
 assert(sha256_hex(std::string(1000000,'a'))=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
 assert(sha256_hex(std::string("a\0b",3)) != sha256_hex("a"));
 std::cout << "4 report hash cases passed (not an Unreal subsystem build)\n";
}
