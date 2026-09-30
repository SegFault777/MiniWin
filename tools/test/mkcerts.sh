#!/bin/bash
# Throwaway test PKI for the QEMU end-to-end TLS tests (tls_e2e.sh). usage: mkcerts.sh <leaf: rsa|ec256|ec384> <ca: rsa|ec384>
# -> /tmp/pki/{ca.pem,leaf.pem,leaf.key,test_roots.h}. The leaf is for host "10.0.2.2" (QEMU's name for the host) with SAN DNS:10.0.2.2, because QEMU's DNS proxy ignores /etc/hosts.
set -e; cd /tmp/pki; rm -f *.pem *.key *.h *.csr *.srl
LEAF=${1:-rsa}; CA=${2:-rsa}
if [ "$CA" = rsa ]; then openssl req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.pem -days 30 -subj "/CN=MW Test Root" -sha256 -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" 2>/dev/null
else openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:secp384r1 -nodes -keyout ca.key -out ca.pem -days 30 -subj "/CN=MW Test Root" -sha384 -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" 2>/dev/null; fi
case $LEAF in
 rsa) openssl req -newkey rsa:2048 -nodes -keyout leaf.key -out leaf.csr -subj "/CN=${SAN:-10.0.2.2}" 2>/dev/null;;
 ec256) openssl req -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout leaf.key -out leaf.csr -subj "/CN=${SAN:-10.0.2.2}" 2>/dev/null;;
 ec384) openssl req -newkey ec -pkeyopt ec_paramgen_curve:secp384r1 -nodes -keyout leaf.key -out leaf.csr -subj "/CN=${SAN:-10.0.2.2}" 2>/dev/null;;
esac
printf "subjectAltName=DNS:${SAN:-10.0.2.2}\nbasicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n" > ext.cnf
openssl x509 -req -in leaf.csr -CA ca.pem -CAkey ca.key -CAcreateserial -out leaf.pem -days 30 -extfile ext.cnf 2>/dev/null
python3 - <<'PY'
import subprocess
import os
if os.environ.get("WRONG_ROOT"):   # negative test: trust a DIFFERENT CA than the one that signed the leaf
    subprocess.check_call("openssl req -x509 -newkey rsa:2048 -nodes -keyout /tmp/pki/other.key -out /tmp/pki/other.pem -days 30 -subj /CN=Someone-Else -addext basicConstraints=critical,CA:TRUE 2>/dev/null", shell=True)
caf = "/tmp/pki/other.pem" if os.environ.get("WRONG_ROOT") else "/tmp/pki/ca.pem"
der = subprocess.check_output(["openssl","x509","-in",caf,"-outform","DER"])
body = ",".join("0x%02x"%b for b in der)
open("/tmp/pki/test_roots.h","w").write("static const unsigned char mw_test_root0[] = {%s};\nstatic const unsigned char *const mw_test_roots[] = {mw_test_root0};\nstatic const unsigned int mw_test_root_lens[] = {%d};\n#define MW_TEST_ROOT_COUNT 1\n" % (body,len(der)))
print("root der bytes:", len(der))
PY
