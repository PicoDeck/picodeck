---
title: "API Crypto"
---

Cryptographic primitives for hashing, encryption, key exchange, and signature verification. All functions operate on binary strings.

> **Simulator note:** `picocalc.crypto` is absent in the simulator (mbedTLS is firmware-only). Test crypto code on hardware.

## picocalc.crypto

### Functions

#### `picocalc.crypto.randomBytes(n)`
Generate cryptographically random bytes.

Draws from a hardware-seeded CTR_DRBG. Raises an error when no seeded, healthy generator is available (it never returns weak bytes).

- **Parameters:**
  - `n` (number): Number of random bytes to generate (1-4096)
- **Returns:** (string) Binary string of `n` random bytes

```lua
local nonce = picocalc.crypto.randomBytes(16)
```

---

#### `picocalc.crypto.sha256(data)`
Compute the SHA-256 hash of the input data.

- **Parameters:**
  - `data` (string): Input data to hash
- **Returns:** (string) 32-byte binary hash

```lua
local hash = picocalc.crypto.sha256("hello world")
-- hash is 32 bytes
```

---

#### `picocalc.crypto.sha1(data)`
Compute the SHA-1 hash of the input data.

- **Parameters:**
  - `data` (string): Input data to hash
- **Returns:** (string) 20-byte binary hash

```lua
local hash = picocalc.crypto.sha1("hello world")
-- hash is 20 bytes
```

---

#### `picocalc.crypto.sha256File(path)`
Compute the SHA-256 hash of a file, streamed from the SD card in small chunks (the file is never loaded whole into memory). Unlike `sha256`, which returns a 32-byte binary string, `sha256File` returns a lowercase hex digest, ready to compare against published checksums.

- **Parameters:**
  - `path` (string): Path to the file to hash
- **Returns:** (string or nil, string) 64-character lowercase hex digest, or `nil, errorString` if the file cannot be read

```lua
local hex, err = picocalc.crypto.sha256File("/data/com.example.mygame/levels.zip")
if hex ~= expected_checksum then
    error("checksum mismatch")
end
```

---

#### `picocalc.crypto.hmacSHA256(key, data)`
Compute an HMAC-SHA-256 message authentication code.

- **Parameters:**
  - `key` (string): HMAC key
  - `data` (string): Input data
- **Returns:** (string) 32-byte binary HMAC

```lua
local mac = picocalc.crypto.hmacSHA256(key, message)
```

---

#### `picocalc.crypto.hmacSHA1(key, data)`
Compute an HMAC-SHA-1 message authentication code.

- **Parameters:**
  - `key` (string): HMAC key
  - `data` (string): Input data
- **Returns:** (string) 20-byte binary HMAC

```lua
local mac = picocalc.crypto.hmacSHA1(key, message)
```

---

#### `picocalc.crypto.deriveKey(K, H, sessionId, letter, needed)`
Derive encryption keys using the SSH key derivation function (RFC 4253 Section 7.2).

- **Parameters:**
  - `K` (string): Shared secret, mpint-encoded
  - `H` (string): Exchange hash
  - `sessionId` (string): Session identifier
  - `letter` (string): Single character A-F selecting the key type
  - `needed` (number): Number of bytes to derive (1-256)
- **Returns:** (string) Derived key of `needed` bytes

```lua
-- Derive client-to-server encryption key (letter "C")
local enc_key = picocalc.crypto.deriveKey(K, H, session_id, "C", 32)
```

---

#### `picocalc.crypto.aes_ctr_new(key, iv)`
Create a new AES-CTR cipher context for encrypting or decrypting data.

- **Parameters:**
  - `key` (string): Encryption key, must be 16 or 32 bytes (AES-128 or AES-256)
  - `iv` (string): Initialization vector, must be 16 bytes
- **Returns:** (userdata) AES-CTR cipher object

```lua
local cipher = picocalc.crypto.aes_ctr_new(key, iv)
local encrypted = cipher:update(plaintext)
cipher:free()
```

---

#### `picocalc.crypto.ecdh_x25519_new()`
Create a new X25519 Elliptic Curve Diffie-Hellman context. Generates a fresh keypair automatically.

- **Parameters:** None
- **Returns:** (userdata) ECDH context object

```lua
local ecdh = picocalc.crypto.ecdh_x25519_new()
local my_pub = ecdh:getPublicKey()
-- send my_pub to peer, receive peer_pub
local shared = ecdh:computeShared(peer_pub)
ecdh:free()
```

---

#### `picocalc.crypto.ecdh_p256_new()`
Create a new P-256 (NIST) Elliptic Curve Diffie-Hellman context. Generates a fresh keypair automatically.

- **Parameters:** None
- **Returns:** (userdata) ECDH context object

```lua
local ecdh = picocalc.crypto.ecdh_p256_new()
local my_pub = ecdh:getPublicKey()
local shared = ecdh:computeShared(peer_pub)
ecdh:free()
```

---

#### `picocalc.crypto.rsaVerify(pubkeyBlob, sigBlob, hash)`
Verify an RSA signature (PKCS#1 v1.5) against a SHA-256 hash.

- **Parameters:**
  - `pubkeyBlob` (string): RSA public key in SSH wire format (string "ssh-rsa" + mpint e + mpint n)
  - `sigBlob` (string): Raw PKCS#1 v1.5 signature bytes, exactly the modulus length (any other length returns `false`)
  - `hash` (string): 32-byte SHA-256 hash to verify against (any other length raises an error)
- **Returns:** (boolean) `true` if the signature is valid

```lua
local ok = picocalc.crypto.rsaVerify(host_key_blob, signature, hash)
if not ok then error("host key verification failed") end
```

---

#### `picocalc.crypto.ecdsaP256Verify(pubkeyBlob, sigBlob, hash)`
Verify an ECDSA P-256 signature against a SHA-256 hash.

- **Parameters:**
  - `pubkeyBlob` (string): ECDSA public key in SSH wire format (string "ecdsa-sha2-nistp256" + string "nistp256" + string Q)
  - `sigBlob` (string): The inner signature blob, mpint r + mpint s, without the outer algorithm-name wrapper of an SSH signature
  - `hash` (string): 32-byte SHA-256 hash to verify against (any other length raises an error)
- **Returns:** (boolean) `true` if the signature is valid

```lua
local ok = picocalc.crypto.ecdsaP256Verify(host_key_blob, signature, hash)
```

---

### AES-CTR Cipher Methods

Objects returned by `picocalc.crypto.aes_ctr_new()`.

#### `cipher:update(data)`
Encrypt or decrypt data using the AES-CTR cipher. AES-CTR is symmetric, so the same operation is used for both encryption and decryption.

- **Parameters:**
  - `data` (string): Input data to transform
- **Returns:** (string) Transformed output of the same length as input

```lua
local cipher = picocalc.crypto.aes_ctr_new(key, iv)
local ciphertext = cipher:update(plaintext)
```

---

#### `cipher:free()`
Explicitly free the cipher resources. Also called automatically by the garbage collector.

- **Parameters:** None
- **Returns:** None

```lua
cipher:free()
```

---

### ECDH Methods

Objects returned by `picocalc.crypto.ecdh_x25519_new()` or `picocalc.crypto.ecdh_p256_new()`.

#### `ecdh:getPublicKey()`
Get the public key from the ECDH context.

- **Parameters:** None
- **Returns:** (string) Binary public key bytes

```lua
local pub = ecdh:getPublicKey()
```

---

#### `ecdh:computeShared(peerPublicKey)`
Compute the shared secret using the peer's public key.

- **Parameters:**
  - `peerPublicKey` (string): The peer's public key bytes
- **Returns:** (string) Shared secret bytes, or `nil, err` on failure (e.g. an invalid peer key)

```lua
local shared = ecdh:computeShared(peer_pub)
```

---

#### `ecdh:free()`
Explicitly free the ECDH context. Also called automatically by the garbage collector.

- **Parameters:** None
- **Returns:** None

```lua
ecdh:free()
```
