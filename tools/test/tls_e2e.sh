#!/usr/bin/env bash
# End-to-end TLS test in QEMU against a local `openssl s_server` -- the only way to exercise the
# whole stack (NIC -> IP -> TCP -> TLS -> x509 -> HTTPS) headlessly, since real sites can't be
# reached with a trustworthy certificate path from a sandboxed build host.
#
#   tools/test/tls_e2e.sh <leaf: rsa|ec256|ec384> <ca: rsa|ec384> <secs> [s_server args...]
#   e.g. tools/test/tls_e2e.sh rsa ec384 30 -tls1_2 -cipher ECDHE-RSA-AES128-GCM-SHA256 -curves X25519
#
# Env: PAGE (file in /tmp/www served, default small.html), NIC (rtl8139|e1000), LINES_OUT.
# Needs: nasm, gcc, qemu-system-i386, openssl, python3. Root (binds :443).
# Result: `[AUTOTEST] RESULT https-ok` in the output = the whole session worked.
# GOTCHAS learned the hard way: background processes die when the calling shell exits, so
# everything (server + QEMU + reading the log) happens inside this one script; QEMU needs an
# ABSOLUTE image path; SLIRP's DNS ignores /etc/hosts, hence the IP-literal host.
set -uo pipefail
cd "$(dirname "$0")/../.."
LEAF=$1; CA=$2; T=$3; shift 3
PAGE=${PAGE:-small.html}; NIC=${NIC:-rtl8139}
mkdir -p /tmp/www
[ -f /tmp/www/small.html ] || python3 -c "
print('<html><body>')
for i in range(40): print('<p>small page line %02d ok</p>' % i)
print('</body></html>')" > /tmp/www/small.html
tools/test/mkcerts.sh "$LEAF" "$CA" >/dev/null
IMG=/tmp/mw_e2e; rm -rf $IMG
BUILD=$IMG EXTRA_CFLAGS="-DMW_TLS_TEST_ROOTS=\\\"/tmp/pki/test_roots.h\\\" -DMW_AUTOTEST_URL=\\\"https://10.0.2.2/$PAGE\\\" -DMW_AUTOTEST_DUMP=100" ./build.sh >$IMG.log 2>&1 \
  || { echo "BUILD FAILED"; tail -5 $IMG.log; exit 1; }
pkill -x openssl 2>/dev/null; pkill -x qemu-system-i386 2>/dev/null; sleep 0.5
(cd /tmp/www && setsid openssl s_server -accept 443 -cert /tmp/pki/leaf.pem -key /tmp/pki/leaf.key -HTTP "$@" </dev/null >/tmp/sserver.log 2>&1 &)
sleep 1
rm -f /tmp/serial.log
(setsid timeout "$T" qemu-system-i386 -m 128 -drive file=$IMG/os-image.img,format=raw \
   -netdev user,id=n0 -device "$NIC",netdev=n0 -display none -serial file:/tmp/serial.log \
   -no-reboot -monitor none </dev/null >/tmp/qemu.log 2>&1 &)
sleep $((T + 3))
grep -av "^\[NET\]\|^$\|\[PCI\]\|\[DHCP\]\|\[RTL\|\[E1000" /tmp/serial.log | tail -"${LINES_OUT:-14}"
echo "--- s_server:"; grep -a "ACCEPT\|FILE\|error" /tmp/sserver.log | head -6
pkill -x openssl; true
