#!/usr/bin/env bash
# Certificate-extension tests (audit M-02). Needs gcc and openssl (3.x). Generates one DER certificate per
# variant into /tmp/x509t, then runs host_x509_test against MiniWin's real x509.h.
set -euo pipefail
cd "$(dirname "$0")"
D=/tmp/x509t; rm -rf $D; mkdir -p $D
mk() {   # mk name "subject" -addext ... : self-signed EC P-256 certificate
    local name=$1; shift
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout $D/k.pem -out $D/$name.pem \
        -subj "/CN=$name.test" -days 30 "$@" >/dev/null 2>&1
    openssl x509 -in $D/$name.pem -outform DER -out $D/$name.der
}
NOBC='-addext basicConstraints=critical,CA:FALSE'
mk leaf_plain                ;
mk leaf_ku_ds                -addext keyUsage=critical,digitalSignature
mk leaf_ku_ds_ke             -addext keyUsage=critical,digitalSignature,keyEncipherment
mk leaf_ku_noDS              -addext keyUsage=critical,keyEncipherment
mk leaf_ku_certsign          -addext basicConstraints=critical,CA:TRUE -addext keyUsage=critical,keyCertSign
mk leaf_unknown_crit         -addext 1.2.3.4=critical,ASN1:NULL
mk leaf_unknown_noncrit      -addext 1.2.3.4=ASN1:NULL
mk leaf_crldp_crit           -addext crlDistributionPoints=critical,URI:http://example.test/crl
mk leaf_policies_crit        -addext certificatePolicies=critical,1.2.3.4.5
mk leaf_eku_crit             -addext extendedKeyUsage=critical,serverAuth
mk leaf_san_crit             -addext subjectAltName=critical,DNS:a.test
mk leaf_bc_crit_false        -addext basicConstraints=critical,CA:FALSE
mk leaf_ku_noncrit_noDS      -addext keyUsage=keyEncipherment
mk ca_ok                     -addext basicConstraints=critical,CA:TRUE -addext keyUsage=critical,keyCertSign,cRLSign
mk ca_no_ku                  -addext basicConstraints=critical,CA:TRUE
mk ca_ku_noKCS               -addext basicConstraints=critical,CA:TRUE -addext keyUsage=critical,digitalSignature
mk notca                     -addext basicConstraints=critical,CA:FALSE
mk ca_unknown_crit           -addext basicConstraints=critical,CA:TRUE -addext 1.2.3.4=critical,ASN1:NULL
mk ca_nameconstraints_crit   -addext basicConstraints=critical,CA:TRUE -addext nameConstraints=critical,permitted\;DNS:example.test
mk ca_nameconstraints_noncrit -addext basicConstraints=critical,CA:TRUE -addext nameConstraints=permitted\;DNS:example.test
gcc -O1 -Wall -Wextra -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter -o /tmp/host_x509_test host_x509_test.c
/tmp/host_x509_test $D
