# SPDX-License-Identifier: Apache-2.0
"""Independent reference YCS codec (docs/protocol.md section 3) for test vectors.

Schemas used by the vectors (mirrored by tests/vectors/ycs_vectors_test.cpp):
  u8 u16 u32 u64 bool fixed4 bytes8 ascii8 option_u32 vec_u16_3 sample
where
  bytes8     = bytes, max 8
  ascii8     = string, max 8, content 0x20..0x7E
  option_u32 = option<u32>
  vec_u16_3  = vec<u16>, max 3 elements (min element size 2)
  sample     = struct {
                 version: u16
                 name:    ascii8 (max 16 here: "ascii16")
                 tag:     fixed4
                 amount:  u64
                 maybe:   option<u64>
                 items:   vec<bytes, max 4>, max 3 elements (min element size 4)
                 flag:    bool
                 kind:    enum { 0: Empty, 1: Value { v: u32 } }
               }
Error kinds and check order: length limit is checked before remaining bytes.
"""


class DecodeError(Exception):
    pass


class Reader:
    def __init__(self, data):
        self.d = bytes(data)
        self.i = 0

    def remaining(self):
        return len(self.d) - self.i

    def take(self, n):
        if self.remaining() < n:
            raise DecodeError("truncated")
        out = self.d[self.i:self.i + n]
        self.i += n
        return out

    def uint(self, width):
        return int.from_bytes(self.take(width), "little")

    def boolean(self):
        b = self.uint(1)
        if b > 1:
            raise DecodeError("invalid_bool")
        return b == 1

    def length(self, max_len, min_elem):
        n = self.uint(4)
        if n > max_len:
            raise DecodeError("length_limit")
        if n * min_elem > self.remaining():
            raise DecodeError("truncated")
        return n

    def bytes_(self, max_len):
        n = self.length(max_len, 1)
        return self.take(n)

    def ascii(self, max_len):
        b = self.bytes_(max_len)
        if any(c < 0x20 or c > 0x7E for c in b):
            raise DecodeError("invalid_content")
        return b.decode("ascii")

    def option_tag(self):
        t = self.uint(1)
        if t > 1:
            raise DecodeError("invalid_option_tag")
        return t == 1

    def finish(self):
        if self.remaining() != 0:
            raise DecodeError("trailing_bytes")


def u(width, v):
    return int(v).to_bytes(width, "little")


def enc_bytes(b):
    return u(4, len(b)) + bytes(b)


def encode(schema, v):
    if schema in ("u8", "u16", "u32", "u64"):
        return u({"u8": 1, "u16": 2, "u32": 4, "u64": 8}[schema], int(v))
    if schema == "bool":
        return b"\x01" if v else b"\x00"
    if schema == "fixed4":
        b = bytes.fromhex(v)
        assert len(b) == 4
        return b
    if schema == "bytes8":
        return enc_bytes(bytes.fromhex(v))
    if schema == "ascii8":
        return enc_bytes(v.encode("ascii"))
    if schema == "option_u32":
        return b"\x00" if v is None else b"\x01" + u(4, int(v))
    if schema == "vec_u16_3":
        return u(4, len(v)) + b"".join(u(2, int(x)) for x in v)
    if schema == "sample":
        out = u(2, int(v["version"]))
        out += enc_bytes(v["name"].encode("ascii"))
        out += bytes.fromhex(v["tag"])
        out += u(8, int(v["amount"]))
        out += b"\x00" if v["maybe"] is None else b"\x01" + u(8, int(v["maybe"]))
        out += u(4, len(v["items"])) + b"".join(enc_bytes(bytes.fromhex(x)) for x in v["items"])
        out += b"\x01" if v["flag"] else b"\x00"
        if v["kind"]["tag"] == 0:
            out += b"\x00"
        else:
            out += b"\x01" + u(4, int(v["kind"]["v"]))
        return out
    raise ValueError(schema)


def decode(schema, data):
    r = Reader(data)
    if schema in ("u8", "u16", "u32", "u64"):
        v = str(r.uint({"u8": 1, "u16": 2, "u32": 4, "u64": 8}[schema]))
    elif schema == "bool":
        v = r.boolean()
    elif schema == "fixed4":
        v = r.take(4).hex()
    elif schema == "bytes8":
        v = r.bytes_(8).hex()
    elif schema == "ascii8":
        v = r.ascii(8)
    elif schema == "option_u32":
        v = str(r.uint(4)) if r.option_tag() else None
    elif schema == "vec_u16_3":
        n = r.length(3, 2)
        v = [str(r.uint(2)) for _ in range(n)]
    elif schema == "sample":
        v = {"version": str(r.uint(2)), "name": r.ascii(16), "tag": r.take(4).hex(), "amount": str(r.uint(8))}
        v["maybe"] = str(r.uint(8)) if r.option_tag() else None
        n = r.length(3, 4)
        v["items"] = [r.bytes_(4).hex() for _ in range(n)]
        v["flag"] = r.boolean()
        t = r.uint(1)
        if t == 0:
            v["kind"] = {"tag": 0}
        elif t == 1:
            v["kind"] = {"tag": 1, "v": str(r.uint(4))}
        else:
            raise DecodeError("invalid_enum_tag")
    else:
        raise ValueError(schema)
    r.finish()
    return v


def _sample(**kw):
    base = {"version": "1", "name": "yap", "tag": "deadbeef", "amount": "1000000", "maybe": None,
            "items": [], "flag": False, "kind": {"tag": 0}}
    base.update(kw)
    return base


def gen_vectors():
    m64 = str(2**64 - 1)
    valid_values = [
        ("u8", "0"), ("u8", "255"), ("u16", "1"), ("u16", "65535"), ("u32", "305419896"), ("u32", "4294967295"),
        ("u64", "0"), ("u64", "72623859790382856"), ("u64", m64),
        ("bool", False), ("bool", True),
        ("fixed4", "00ff10ab"),
        ("bytes8", ""), ("bytes8", "01"), ("bytes8", "0102030405060708"),
        ("ascii8", ""), ("ascii8", "yap chn~"),
        ("option_u32", None), ("option_u32", "7"),
        ("vec_u16_3", []), ("vec_u16_3", ["1", "65535", "258"]),
        ("sample", _sample()),
        ("sample", _sample(name="memes-forever", amount=m64, maybe="42", items=["", "aa", "01020304"],
                           flag=True, kind={"tag": 1, "v": "4294967295"})),
    ]
    valid = []
    for schema, v in valid_values:
        enc = encode(schema, v)
        assert decode(schema, enc) == v, (schema, v)
        valid.append({"schema": schema, "value": v, "hex": enc.hex()})

    good_sample = encode("sample", _sample(items=["aa"]))
    invalid_raw = [
        ("u8", b"", "truncated"),
        ("u16", b"\x01", "truncated"),
        ("u64", b"\x00" * 9, "trailing_bytes"),
        ("bool", b"\x02", "invalid_bool"),
        ("bool", b"\xff", "invalid_bool"),
        ("option_u32", b"\x02\x00\x00\x00\x00", "invalid_option_tag"),
        ("option_u32", b"\x01\x00\x00", "truncated"),
        ("bytes8", u(4, 9) + b"\x00" * 9, "length_limit"),
        ("bytes8", u(4, 5) + b"\x00" * 3, "truncated"),
        ("bytes8", b"\xff\xff\xff\xff", "length_limit"),
        ("bytes8", b"\x00\x00\x00", "truncated"),
        ("ascii8", enc_bytes(b"ab\x7f"), "invalid_content"),
        ("ascii8", enc_bytes(b"\x1f"), "invalid_content"),
        ("ascii8", enc_bytes(b"\xc3\xa9"), "invalid_content"),
        ("vec_u16_3", u(4, 4) + b"\x00" * 8, "length_limit"),
        ("vec_u16_3", u(4, 3) + b"\x00" * 4, "truncated"),
        ("vec_u16_3", u(4, 1) + b"\x00" * 3, "trailing_bytes"),
        ("sample", good_sample[:-1] + b"\x02", "invalid_enum_tag"),
        ("sample", good_sample + b"\x00", "trailing_bytes"),
        ("sample", good_sample[:-3], "truncated"),
    ]
    invalid = []
    for schema, raw, kind in invalid_raw:
        try:
            decode(schema, raw)
            raise AssertionError(f"accepted {schema} {raw.hex()}")
        except DecodeError as e:
            assert str(e) == kind, (schema, raw.hex(), str(e), kind)
        invalid.append({"schema": schema, "hex": raw.hex(), "error": kind})
    return {"valid": valid, "invalid": invalid}

