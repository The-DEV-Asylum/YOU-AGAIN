/*
 * ===========================================================================
 *  AntiStalker :: TrustResolver.h
 *  Recognises your own devices through their disguises. Pure C++, testable.
 * ===========================================================================
 *
 *  Background, in one breath: modern phones do not advertise their real MAC.
 *  They use a Resolvable Private Address (RPA) that changes every ~15 min:
 *
 *      RPA (6 bytes, MSB first) = [ prand (3 bytes) | hash (3 bytes) ]
 *                                   ^ top bits 01
 *
 *      hash = ah(IRK, prand) = AES-128(IRK, 13 zero bytes || prand) mod 2^24
 *
 *  Whoever holds the IRK (Identity Resolving Key) can check any RPA in
 *  microseconds. Pairing gives us the IRK. So, offline and passively:
 *  "is this random-looking address actually Antonio's phone?" Yes or no.
 *
 *  Spec: Bluetooth Core Specification, Vol 3, Part H, 2.2.2.
 * ===========================================================================
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace antistalker {

// One device the owner paired once. During pairing it handed over its IRK and
// its real identity address; from then on its rotating RPAs are recognisable.
struct TrustIdentity {
    char name[25] = {0};
    uint8_t irk[16] = {0};    // AIR order (LSB first), exactly as pairing delivered it.
    uint8_t address[6] = {0}; // Identity address, most significant byte first.
    uint8_t addressType = 0;  // 0 = public, 1 = random.
    uint32_t resolvedCount = 0;
    bool used = false;
};

class TrustResolver {
  public:
    static constexpr size_t kMaxIdentities = 4;
    static constexpr uint8_t kPublicAddressType = 0;
    static constexpr uint8_t kRandomAddressType = 1;

    // RPAs carry 0b01 in their two most significant bits. Static random
    // addresses are 0b11, non-resolvable ones 0b00.
    static bool isResolvablePrivateAddress(const uint8_t address[6]);

    // hash = ah(IRK, prand) = e(IRK, 13 zero bytes || prand) mod 2^24.
    // The address layout is prand (3 most significant bytes) || hash.
    static void addressHash(const uint8_t irk[16], const uint8_t address[6], uint8_t hash[3]);

    // Accepts "aa:bb:cc:dd:ee:ff" in either case, ignoring separators.
    static bool parseAddress(const char* text, uint8_t out[6]);
    static void formatAddress(const uint8_t address[6], char* out, size_t outSize);

    bool addIdentity(const char* name, const uint8_t irk[16], const uint8_t address[6],
                     uint8_t addressType);
    bool removeIdentity(const uint8_t address[6], uint8_t addressType);
    void clear();
    const TrustIdentity* findIdentity(const uint8_t address[6], uint8_t addressType) const;
    bool setName(const uint8_t address[6], uint8_t addressType, const char* name);

    // Tries every stored IRK against an observed RPA. A match means the frame
    // came from that bonded device (well, from something holding its IRK).
    TrustIdentity* resolve(const uint8_t rpa[6], const char* advertisedName);
    const TrustIdentity* identityAt(size_t index) const;
    size_t count() const;

  private:
    TrustIdentity identities_[kMaxIdentities];
};

} // namespace antistalker
