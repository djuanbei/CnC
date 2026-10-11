# Dependency record

The new orchestration target uses these dependencies. Legacy targets do not
include the graph or JSON libraries.

| Dependency | Version / pin | Use | License |
| --- | --- | --- | --- |
| dejavu | `4c275e9ebac4fe51a26aac406682b9bcbfd03a9d` | Verified automorphism generators | MIT, `dejavu/LICENSE` |
| nlohmann/json | 3.11.3 | Logical checkpoint and diagnostic serialization | MIT, `JSON-LICENSE` |
| nauty | 2.9.3, system development library | Exact colored-graph canonical labeling | Apache-2.0 |
| OpenSSL libcrypto | 3.x, system development library | SHA-256 integrity and identity | Apache-2.0 |

The dejavu headers were fetched from
<https://github.com/markusa4/dejavu/tree/4c275e9ebac4fe51a26aac406682b9bcbfd03a9d>.
The JSON single header was fetched from
<https://github.com/nlohmann/json/tree/v3.11.3>.
Vendored headers are unchanged upstream files. nauty and OpenSSL are linked, not
vendored. Install their development headers/libraries before building this target.
The canonicalizer adapter has a compile-time guard for nauty 2.9.3 and records
its version/word size in checkpoints. It uses `sparsenauty` with `getcanon` and
an explicit initial color partition. It does not use dejavu as a canonicalizer.

Validated environment: Apple Clang 21, C++17, macOS arm64, nauty 2.9.3 (64 bits),
OpenSSL 3.6.4. POSIX process, signal, file-lock, and durability primitives are
required; a native Windows port is outside this implementation.
