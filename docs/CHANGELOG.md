# Changelog

## MiniWin 1.0 (2026-10)

1.0 is the hardening release: no new features after 1.0-pre-29, every finding of two bug audits fixed and each
one covered by a regression test that fails without the fix. The release candidates, in order:

| build | what was fixed |
|---|---|
| 1.0-rc-1 | **critical**: ATA status polling could hang the whole kernel; a partly read program could be executed; TCP accepted any SYN-ACK from the right 4-tuple |
| 1.0-rc-2 | **critical**: IP/UDP checksums were never verified; TCP booked segments that never left the host as sent (new `IP_SEND_*` results); NIC drivers reported success when transmission never completed (and the e1000 TX-done bit was never actually re-read) |
| 1.0-rc-3 | protocol validation: TCP RST, TLS record headers and ServerHello, ARP cache poisoning, DNS answer binding, DHCP reply binding and a real lease lifecycle (renew / rebind / expire) |
| 1.0-rc-4 | storage: crash-safe journaled saves with CRC-32, oversized files refused instead of truncated, damaged files refused instead of opened, honest "save failed" UI |
| 1.0-rc-5 | oversized HTTP responses fail instead of showing a cut-off page, "page too big" notice, HTTP token lists, critical X.509 extensions and keyUsage, URL-encoding overflow, TCP peer window |

Known limits that are documented rather than fixed: no TLS 1.3, no certificate revocation or name-constraint
enforcement (such certificates are refused when critical), resolved ARP entries never expire, the disk journal
assumes a drive writes a single sector atomically and in order, and real-world TLS chains from public sites could
only be checked against the embedded roots in the development sandbox.
