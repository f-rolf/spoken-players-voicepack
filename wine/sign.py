#!/usr/bin/env python3
"""Signs sapifix.dll (Authenticode, SHA-256) with a certificate made for it, as
engine/tools/Sign-Engine.ps1 signs the engine: WoW loads no DLL into its process whose signature is
not trusted. The certificate is self-signed, for code signing only, and its private key is never
written anywhere, so it can sign nothing else. Its public part goes to the .cer, which the setup has
the prefix trust (Root and TrustedPublisher), as the voicepack's setup does the engine's.

sign.py <dll> <cer>    (needs Python's cryptography package)
"""
import datetime
import hashlib
import struct
import sys

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, rsa
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

NAME = "Spoken Players SapiFix (local, this build only)"


# ---- DER, as much as a signature needs ----

def der(tag, content):
    n = len(content)
    if n < 0x80:
        size = bytes([n])
    else:
        body = n.to_bytes((n.bit_length() + 7) // 8, "big")
        size = bytes([0x80 | len(body)]) + body
    return bytes([tag]) + size + content


def seq(*items):
    return der(0x30, b"".join(items))


def set_of(*items):
    return der(0x31, b"".join(sorted(items)))  # DER sorts a SET OF by encoding


def oid(dotted):
    parts = [int(p) for p in dotted.split(".")]
    body = bytes([parts[0] * 40 + parts[1]])
    for p in parts[2:]:
        chunk = [p & 0x7F]
        while p > 0x7F:
            p >>= 7
            chunk.insert(0, 0x80 | (p & 0x7F))
        body += bytes(chunk)
    return der(0x06, body)


def integer(n):
    return der(0x02, n.to_bytes(n.bit_length() // 8 + 1, "big", signed=True))


def octets(data):
    return der(0x04, data)


def explicit(number, content):
    return der(0xA0 + number, content)


NULL = b"\x05\x00"
SHA256 = seq(oid("2.16.840.1.101.3.4.2.1"), NULL)
RSA = seq(oid("1.2.840.113549.1.1.1"), NULL)
SPC_INDIRECT_DATA = "1.3.6.1.4.1.311.2.1.4"


# ---- the PE ----

def pe_fields(image):
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    if image[pe:pe + 4] != b"PE\0\0":
        sys.exit("not a PE file")
    optional = pe + 24
    magic = struct.unpack_from("<H", image, optional)[0]
    directories = optional + (112 if magic == 0x20B else 96)
    return optional + 64, directories + 4 * 8  # the checksum's offset, the certificate table entry's


def authenticode_hash(image, checksum, security):
    # The whole file but its checksum, the certificate table's entry and the table (none yet).
    return hashlib.sha256(image[:checksum] + image[checksum + 4:security] + image[security + 8:]).digest()


def pe_checksum(image, checksum):
    data = image[:checksum] + b"\0\0\0\0" + image[checksum + 4:]  # the field itself counts as zero
    total = 0
    for (word,) in struct.iter_unpack("<H", data + b"\0" * (len(data) % 2)):
        total += word
        total = (total & 0xFFFF) + (total >> 16)
    return (total + len(image)) & 0xFFFFFFFF


# ---- the certificate and the signature ----

def make_certificate():
    key = rsa.generate_private_key(public_exponent=65537, key_size=3072)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, NAME)])
    now = datetime.datetime.now(datetime.timezone.utc)
    certificate = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(hours=1))
        .not_valid_after(now + datetime.timedelta(days=3653))
        .add_extension(x509.KeyUsage(True, False, False, False, False, False, False, False, False), critical=True)
        .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.CODE_SIGNING]), critical=False)
        .add_extension(x509.SubjectKeyIdentifier.from_public_key(key.public_key()), critical=False)
        .sign(key, hashes.SHA256())
    )
    return key, certificate


def signed_data(image_hash, key, certificate):
    obsolete = der(0x80, "<<<Obsolete>>>".encode("utf-16-be"))
    pe_image_data = seq(der(0x03, b"\x00"), explicit(0, explicit(2, obsolete)))
    # SpcIndirectDataContent's content: its digest leaves out the SEQUENCE's own tag and length
    indirect = seq(oid("1.3.6.1.4.1.311.2.1.15"), pe_image_data) + seq(SHA256, octets(image_hash))

    def attribute(kind, value):
        return seq(oid(kind), set_of(value))

    attributes = sorted([
        attribute("1.2.840.113549.1.9.3", oid(SPC_INDIRECT_DATA)),                # contentType
        attribute("1.3.6.1.4.1.311.2.1.12", seq()),                               # SpcSpOpusInfo
        attribute("1.3.6.1.4.1.311.2.1.11", seq(oid("1.3.6.1.4.1.311.2.1.21"))),  # individual code signing
        attribute("1.2.840.113549.1.9.4", octets(hashlib.sha256(indirect).digest())),  # messageDigest
    ])
    signature = key.sign(der(0x31, b"".join(attributes)), padding.PKCS1v15(), hashes.SHA256())
    signer = seq(
        integer(1),
        seq(certificate.issuer.public_bytes(), integer(certificate.serial_number)),
        SHA256,
        der(0xA0, b"".join(attributes)),
        RSA,
        octets(signature),
    )
    return seq(
        oid("1.2.840.113549.1.7.2"),
        explicit(0, seq(
            integer(1),
            set_of(SHA256),
            seq(oid(SPC_INDIRECT_DATA), explicit(0, seq(indirect))),
            der(0xA0, certificate.public_bytes(serialization.Encoding.DER)),
            set_of(signer),
        )),
    )


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    dll, cer = sys.argv[1:]
    with open(dll, "rb") as f:
        image = bytearray(f.read())
    checksum, security = pe_fields(image)
    table, size = struct.unpack_from("<II", image, security)
    if size:
        del image[table:]  # signed before: sign again
        struct.pack_into("<II", image, security, 0, 0)
    image += b"\0" * (-len(image) % 8)  # the table starts on 8 bytes

    key, certificate = make_certificate()
    blob = signed_data(authenticode_hash(bytes(image), checksum, security), key, certificate)
    del key
    length = 8 + len(blob)
    entry = struct.pack("<IHH", length + (-length % 8), 0x0200, 0x0002) + blob + b"\0" * (-length % 8)
    struct.pack_into("<II", image, security, len(image), len(entry))
    image += entry
    struct.pack_into("<I", image, checksum, pe_checksum(bytes(image), checksum))

    with open(dll, "wb") as f:
        f.write(image)
    with open(cer, "wb") as f:
        f.write(certificate.public_bytes(serialization.Encoding.DER))
    print(f"signed {dll}: {certificate.fingerprint(hashes.SHA1()).hex().upper()}")


if __name__ == "__main__":
    main()
