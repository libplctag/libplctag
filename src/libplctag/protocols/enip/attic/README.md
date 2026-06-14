# attic — superseded EtherNet/IP implementation

Everything in this directory is the **previous** ENIP implementation and its
documentation. It is retained for reference only and is **not to be reused
wholesale**. The authoritative design for the rewrite is
[`../ENIP-SESSION-DESIGN.md`](../ENIP-SESSION-DESIGN.md).

Lift small, self-contained helpers (e.g. EIP header encode/decode, CPF item
framing, CIP path encoding) only after checking them against the new design and
the coding guidelines. Do not wire any of these files back into the build.
