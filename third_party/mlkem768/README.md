# ML-KEM-768, from PQClean

The `clean` implementation of ML-KEM-768 (FIPS 203) and the Keccak/SHAKE it
is built on, from [PQClean](https://github.com/PQClean/PQClean) at commit
`0586a82`: `crypto_kem/ml-kem-768/clean/`, and `common/fips202.[ch]` and
`common/compat.h`. Public domain (CC0); see LICENSE and the notes at the top
of fips202.c. Unmodified, except that `randombytes.h` is our own: sshd
supplies the random bytes from its CTR-DRBG.

Used by sshd for the `mlkem768x25519-sha256` key exchange, and only for
encapsulation: the server never makes an ML-KEM key or decapsulates one.
