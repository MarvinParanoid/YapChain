# SPDX-License-Identifier: Apache-2.0
"""Independent reference Bech32m (BIP-350) and YapChain address codec.

Error kinds and their check order mirror docs/protocol.md section 5.3 and the
C++ implementation contract; they are not consensus-critical, but must match
for the test vectors.
"""
CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l"
BECH32M_CONST = 0x2BC830A3
BECH32_CONST = 1
GEN = (0x3B6A57B2, 0x26508E6D, 0x1EA119FA, 0x3D4233DD, 0x2A1462B3)


def polymod(values):
    chk = 1
    for v in values:
        top = chk >> 25
        chk = ((chk & 0x1FFFFFF) << 5) ^ v
        for i in range(5):
            if (top >> i) & 1:
                chk ^= GEN[i]
    return chk


def hrp_expand(hrp):
    return [ord(c) >> 5 for c in hrp] + [0] + [ord(c) & 31 for c in hrp]


def create_checksum(hrp, data, const=BECH32M_CONST):
    pm = polymod(hrp_expand(hrp) + list(data) + [0] * 6) ^ const
    return [(pm >> (5 * (5 - i))) & 31 for i in range(6)]


def encode(hrp, data, const=BECH32M_CONST):
    combined = list(data) + create_checksum(hrp, data, const)
    return hrp + "1" + "".join(CHARSET[d] for d in combined)


def decode(raw):
    """raw: bytes. Returns (hrp, data) or raises ValueError(kind)."""
    if len(raw) > 90:
        raise ValueError("invalid_length")
    if any(b < 33 or b > 126 for b in raw):
        raise ValueError("invalid_character")
    s = raw.decode("ascii")
    if s.lower() != s and s.upper() != s:
        raise ValueError("mixed_case")
    s = s.lower()
    pos = s.rfind("1")
    if pos < 0:
        raise ValueError("missing_separator")
    if pos == 0:
        raise ValueError("invalid_hrp")
    if len(s) - pos - 1 < 6:
        raise ValueError("data_too_short")
    part = s[pos + 1:]
    if any(c not in CHARSET for c in part):
        raise ValueError("invalid_character")
    hrp = s[:pos]
    data = [CHARSET.find(c) for c in part]
    if polymod(hrp_expand(hrp) + data) != BECH32M_CONST:
        raise ValueError("invalid_checksum")
    return hrp, data[:-6]


def convertbits(data, frombits, tobits, pad):
    acc = 0
    bits = 0
    out = []
    maxv = (1 << tobits) - 1
    for v in data:
        if v < 0 or v >> frombits:
            return None
        acc = (acc << frombits) | v
        bits += frombits
        while bits >= tobits:
            bits -= tobits
            out.append((acc >> bits) & maxv)
    if pad:
        if bits:
            out.append((acc << (tobits - bits)) & maxv)
    elif bits >= frombits or ((acc << (tobits - bits)) & maxv):
        return None
    return out


def encode_address(hrp, account_id):
    return encode(hrp, [0] + convertbits(account_id, 8, 5, True))


def decode_address(raw, expected_hrp):
    """Returns account_id bytes or raises ValueError(kind)."""
    try:
        hrp, data = decode(raw)
    except ValueError:
        raise ValueError("bech32")
    if hrp != expected_hrp:
        raise ValueError("wrong_hrp")
    if len(data) == 0:
        raise ValueError("wrong_length")
    if data[0] != 0:
        raise ValueError("wrong_version")
    if len(data) != 53:
        raise ValueError("wrong_length")
    payload = data[1:]
    acc = 0
    for v in payload:
        acc = (acc << 5) | v
    if acc & 0xF:
        raise ValueError("non_zero_padding")
    return int.to_bytes(acc >> 4, 32, "big")
