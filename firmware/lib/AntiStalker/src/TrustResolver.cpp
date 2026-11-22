/*
 * ===========================================================================
 *  AntiStalker :: TrustResolver.cpp
 *  ah(), address parsing and the tiny 4-slot identity table.
 * ===========================================================================
 */

#include "TrustResolver.h"

// tiny-AES-c is plain C and its header has no extern "C" guard. Without this
// you get a linker error that looks like a riddle.
extern "C" {
#include "tiny-aes/aes.h"
}

#include <ctype.h>
#include <stdio.h>
#include <string.h>

namespace antistalker {
namespace {

// BLE names are untrusted input: printable ASCII only, and short.
void copyName(char* destination, size_t size, const char* source) {
    size_t out = 0;
    for (size_t i = 0; source != nullptr && source[i] != '\0' && out + 1 < size; ++i) {
        const uint8_t c = static_cast<uint8_t>(source[i]);
        destination[out++] = (c >= 0x20 && c <= 0x7E) ? static_cast<char>(c) : '.';
    }
    destination[out] = '\0';
}

} // namespace

bool TrustResolver::isResolvablePrivateAddress(const uint8_t address[6]) {
    return address != nullptr && (address[0] & 0xC0) == 0x40;
}

void TrustResolver::addressHash(const uint8_t irk[16], const uint8_t address[6], uint8_t hash[3]) {
    // Bluetooth Core Spec Vol 3 Part H 2.2.2:
    //   ah(k, r) = e(k, r') mod 2^24
    //   r'       = 13 zero bytes || prand
    //   result   = the last (least significant) 3 bytes of the ciphertext
    //
    // THE gotcha of this whole project: pairing hands over the IRK in air
    // order (least significant byte first), which is how we store it. AES
    // wants its key most significant byte first. So the key gets reversed
    // before the key schedule. Get this wrong and ah() quietly returns
    // garbage, nothing ever matches, and you question your life choices.
    // Pinned down with a real address captured from a bonded phone, see
    // testResolveLiveCapturedPhoneAddress in the tests.
    uint8_t block[16] = {0};
    block[13] = address[0];
    block[14] = address[1];
    block[15] = address[2];
    uint8_t key[16];
    for (size_t i = 0; i < sizeof(key); ++i) {
        key[i] = irk[15 - i];
    }
    struct AES_ctx ctx;
    AES_init_ctx(&ctx, key);
    AES_ECB_encrypt(&ctx, block);
    hash[0] = block[13];
    hash[1] = block[14];
    hash[2] = block[15];
}

bool TrustResolver::parseAddress(const char* text, uint8_t out[6]) {
    if (text == nullptr || out == nullptr) {
        return false;
    }
    size_t found = 0;
    int high = -1;
    for (const char* p = text; *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(tolower(static_cast<unsigned char>(*p)));
        int hex;
        if (c >= '0' && c <= '9') {
            hex = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            hex = c - 'a' + 10;
        } else if (c == ':' || c == '-' || c == ' ') {
            continue;
        } else {
            return false;
        }
        if (high < 0) {
            high = hex;
            continue;
        }
        out[found++] = static_cast<uint8_t>(high * 16 + hex);
        high = -1;
        if (found == 6) {
            // After 6 bytes only separators are allowed. "AA:BB:CC:DD:EE:FF:00"
            // is not a MAC, it is a typo.
            for (const char* q = p + 1; *q != '\0'; ++q) {
                if (*q != ':' && *q != '-' && *q != ' ') {
                    return false;
                }
            }
            return true;
        }
    }
    return false;
}

void TrustResolver::formatAddress(const uint8_t address[6], char* out, size_t outSize) {
    if (out == nullptr || outSize < 18) {
        return;
    }
    snprintf(out, outSize, "%02X:%02X:%02X:%02X:%02X:%02X", address[0], address[1], address[2],
             address[3], address[4], address[5]);
}

bool TrustResolver::addIdentity(const char* name, const uint8_t irk[16], const uint8_t address[6],
                                uint8_t addressType) {
    // Re-pairing the same device refreshes its key but keeps the name the
    // owner picked. Nobody wants "My Phone" to turn back into "Trusted 3".
    TrustIdentity* target = nullptr;
    TrustIdentity* freeSlot = nullptr;
    for (TrustIdentity& identity : identities_) {
        if (identity.used && identity.addressType == addressType &&
            memcmp(identity.address, address, 6) == 0) {
            target = &identity;
            break;
        }
        if (!identity.used && freeSlot == nullptr) {
            freeSlot = &identity;
        }
    }
    if (target == nullptr) {
        target = freeSlot;
    }
    if (target == nullptr) {
        return false;
    }
    if (!target->used) {
        // Only a brand new entry takes the caller's (auto-generated) name.
        copyName(target->name, sizeof(target->name), name);
    }
    target->used = true;
    target->addressType = addressType;
    memcpy(target->irk, irk, 16);
    memcpy(target->address, address, 6);
    return true;
}

bool TrustResolver::removeIdentity(const uint8_t address[6], uint8_t addressType) {
    for (size_t i = 0; i < kMaxIdentities; ++i) {
        const TrustIdentity& identity = identities_[i];
        if (identity.used && identity.addressType == addressType &&
            memcmp(identity.address, address, 6) == 0) {
            // Shift the rest down so slots stay dense: NVS key "idN" == index N.
            for (size_t j = i; j + 1 < kMaxIdentities; ++j) {
                identities_[j] = identities_[j + 1];
            }
            identities_[kMaxIdentities - 1] = TrustIdentity();
            return true;
        }
    }
    return false;
}

void TrustResolver::clear() {
    for (TrustIdentity& identity : identities_) {
        identity = TrustIdentity();
    }
}

const TrustIdentity* TrustResolver::findIdentity(const uint8_t address[6],
                                                 uint8_t addressType) const {
    for (const TrustIdentity& identity : identities_) {
        if (identity.used && identity.addressType == addressType &&
            memcmp(identity.address, address, 6) == 0) {
            return &identity;
        }
    }
    return nullptr;
}

bool TrustResolver::setName(const uint8_t address[6], uint8_t addressType, const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    for (TrustIdentity& identity : identities_) {
        if (identity.used && identity.addressType == addressType &&
            memcmp(identity.address, address, 6) == 0) {
            copyName(identity.name, sizeof(identity.name), name);
            return true;
        }
    }
    return false;
}

TrustIdentity* TrustResolver::resolve(const uint8_t rpa[6], const char* advertisedName) {
    if (!isResolvablePrivateAddress(rpa)) {
        return nullptr;
    }
    uint8_t hash[3];
    for (TrustIdentity& identity : identities_) {
        if (!identity.used) {
            continue;
        }
        addressHash(identity.irk, rpa, hash);
        if (hash[0] == rpa[3] && hash[1] == rpa[4] && hash[2] == rpa[5]) {
            ++identity.resolvedCount;
            // Some devices advertise their name. If we have none yet, grab it
            // once. Never overwrite a name that is already there.
            if (identity.name[0] == '\0' && advertisedName != nullptr && advertisedName[0] != '\0') {
                copyName(identity.name, sizeof(identity.name), advertisedName);
            }
            return &identity;
        }
    }
    return nullptr;
}

const TrustIdentity* TrustResolver::identityAt(size_t index) const {
    return index < kMaxIdentities ? &identities_[index] : nullptr;
}

size_t TrustResolver::count() const {
    size_t total = 0;
    for (const TrustIdentity& identity : identities_) {
        total += identity.used ? 1U : 0U;
    }
    return total;
}

} // namespace antistalker
