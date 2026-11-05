#!/usr/bin/env bash
# Local certificate authority for the feeder MQTT broker. Needs only openssl.
# See ../README.md. Reference/dev tooling: in production keep the CA key offline.
set -euo pipefail
umask 077

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OUT=${FEEDER_CERTS_OUT:-$SCRIPT_DIR/out}
FORCE=0

CA_DAYS=7300       # CA: 20 years
BROKER_DAYS=825
LEAF_DAYS=3650     # device and backend certificates: 10 years

# Device id contract (docs/architecture.md, MQTT protocol): the cert CN equals the id.
ID_RE='^[a-z0-9][a-z0-9-]{2,31}$'
# Reserved: this CN is the backend's MQTT username and gets the backend ACL.
RESERVED_ID='backend'

usage() {
  cat <<USAGE
Usage: make-certs.sh [--out DIR] [--force] <command> [args]

Commands:
  ca                       create the CA (EC P-256, 20 years)
  broker <hostname> [ip..] create the broker server cert (SAN: DNS hostname, optional IPs)
  device <id>              create a device client cert, CN = id (^[a-z0-9][a-z0-9-]{2,31}\$)
  backend                  create the backend client cert, CN = backend

Options:
  --out DIR   output directory (default: \$FEEDER_CERTS_OUT or ./out next to this script)
  --force     overwrite existing keys and certificates (default: refuse)

The CA key never leaves the output directory. Regenerating the CA with --force
invalidates every certificate it signed.
USAGE
}

die() { echo "make-certs: $*" >&2; exit 1; }

# --- argument parsing --------------------------------------------------------
pos=()
while (($#)); do
  case $1 in
    --force) FORCE=1 ;;
    --out) [[ $# -ge 2 ]] || die "--out needs a directory"; OUT=$2; shift ;;
    --out=*) OUT=${1#--out=} ;;
    -h|--help) usage; exit 0 ;;
    --) shift; while (($#)); do pos+=("$1"); shift; done; break ;;
    -*) usage >&2; die "unknown option: $1" ;;
    *) pos+=("$1") ;;
  esac
  shift
done
((${#pos[@]})) || { usage >&2; exit 2; }
cmd=${pos[0]}
set -- "${pos[@]:1}"

command -v openssl >/dev/null || die "openssl not found"

# --- helpers -----------------------------------------------------------------
TMP=""
cleanup() { [[ -n $TMP ]] && rm -rf "$TMP"; return 0; }
trap cleanup EXIT

prepare() {
  mkdir -p "$OUT"
  chmod 700 "$OUT"
  TMP=$(mktemp -d "$OUT/.tmp.XXXXXX")
}

# refuse_existing <file>...  (unless --force)
refuse_existing() {
  local f
  for f in "$@"; do
    if [[ -e $f && $FORCE -ne 1 ]]; then
      die "$f already exists; refusing to overwrite (use --force)"
    fi
  done
}

need_ca() {
  [[ -f $OUT/ca.key && -f $OUT/ca.crt ]] || die "no CA in $OUT; run: make-certs.sh ca"
}

new_key() { openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$1" 2>/dev/null; }

random_serial() { echo "0x01$(openssl rand -hex 15)"; }  # positive, 128-bit

# install_pair <name>: move $TMP/<name>.{key,crt} into $OUT
install_pair() {
  chmod 600 "$TMP/$1.key"
  chmod 644 "$TMP/$1.crt"
  mv -f "$TMP/$1.key" "$OUT/$1.key"
  mv -f "$TMP/$1.crt" "$OUT/$1.crt"
  echo "wrote $OUT/$1.key (secret)"
  echo "wrote $OUT/$1.crt"
}

# issue <name> <cn> <days> <ext-lines>: key + CA-signed leaf in $TMP
issue() {
  local name=$1 cn=$2 days=$3 ext=$4
  need_ca
  new_key "$TMP/$name.key"
  openssl req -new -key "$TMP/$name.key" -subj "/CN=$cn" -out "$TMP/$name.csr"
  printf '[leaf]\n%s\n' "$ext" > "$TMP/$name.ext"
  openssl x509 -req -in "$TMP/$name.csr" -CA "$OUT/ca.crt" -CAkey "$OUT/ca.key" \
    -set_serial "$(random_serial)" -days "$days" -sha256 \
    -extfile "$TMP/$name.ext" -extensions leaf -out "$TMP/$name.crt" 2>/dev/null
}

LEAF_BASE='basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid'

client_ext() { printf '%s\nextendedKeyUsage = clientAuth' "$LEAF_BASE"; }

# --- commands ----------------------------------------------------------------
do_ca() {
  refuse_existing "$OUT/ca.key" "$OUT/ca.crt"
  prepare
  cat > "$TMP/ca.cnf" <<CNF
[req]
distinguished_name = dn
prompt = no
x509_extensions = ca_ext
[dn]
CN = Feeder Local CA
[ca_ext]
basicConstraints = critical,CA:TRUE,pathlen:0
keyUsage = critical,keyCertSign,cRLSign
subjectKeyIdentifier = hash
CNF
  new_key "$TMP/ca.key"
  openssl req -x509 -new -key "$TMP/ca.key" -config "$TMP/ca.cnf" \
    -days "$CA_DAYS" -sha256 -out "$TMP/ca.crt"
  install_pair ca
}

do_broker() {
  (($# >= 1)) || die "usage: broker <hostname> [ip...]"
  local host=$1 san ip
  shift
  [[ $host =~ ^[A-Za-z0-9]([A-Za-z0-9.-]*[A-Za-z0-9])?$ ]] || die "invalid hostname: $host"
  ((${#host} <= 64)) || die "hostname longer than 64 characters"
  san="DNS:$host"
  for ip in "$@"; do
    if [[ $ip =~ ^([0-9]{1,3})\.([0-9]{1,3})\.([0-9]{1,3})\.([0-9]{1,3})$ \
          && ${BASH_REMATCH[1]} -le 255 && ${BASH_REMATCH[2]} -le 255 \
          && ${BASH_REMATCH[3]} -le 255 && ${BASH_REMATCH[4]} -le 255 ]] \
       || [[ $ip == *:* && $ip =~ ^[0-9A-Fa-f:]+$ ]]; then
      san+=",IP:$ip"
    else
      die "invalid IP address: $ip"
    fi
  done
  refuse_existing "$OUT/broker.key" "$OUT/broker.crt"
  prepare
  issue broker "$host" "$BROKER_DAYS" "$(printf '%s\nextendedKeyUsage = serverAuth\nsubjectAltName = %s' "$LEAF_BASE" "$san")"
  install_pair broker
}

do_device() {
  (($# == 1)) || die "usage: device <id>"
  local id=$1
  [[ $id != "$RESERVED_ID" ]] || die "refusing id '$RESERVED_ID': that CN is the backend's identity and carries the backend ACL"
  [[ $id =~ $ID_RE ]] || die "invalid device id '$id' (must match $ID_RE)"
  refuse_existing "$OUT/device-$id.key" "$OUT/device-$id.crt"
  prepare
  issue "device-$id" "$id" "$LEAF_DAYS" "$(client_ext)"
  install_pair "device-$id"
}

do_backend() {
  (($# == 0)) || die "usage: backend"
  refuse_existing "$OUT/backend.key" "$OUT/backend.crt"
  prepare
  issue backend "$RESERVED_ID" "$LEAF_DAYS" "$(client_ext)"
  install_pair backend
}

case $cmd in
  ca) do_ca "$@" ;;
  broker) do_broker "$@" ;;
  device) do_device "$@" ;;
  backend) do_backend "$@" ;;
  *) usage >&2; die "unknown command: $cmd" ;;
esac
