# TweetNaCl

Public-domain reference implementation, downloaded from:
https://tweetnacl.cr.yp.to/20140427/tweetnacl.c
https://tweetnacl.cr.yp.to/20140427/tweetnacl.h

Project and authors: https://tweetnacl.cr.yp.to/
Only Ed25519 key generation, SHA-512, and X25519 public-key derivation are used.
The firmware build removes unused functions. Entropy comes from the Flipper
hardware RNG, supplied by identity.c. No signing or private-message feature is
advertised by this app yet. Keep this reference source unformatted.

Local changes: a GCC diagnostic pragma suppresses reference-source signedness warnings; trailing whitespace is removed.
