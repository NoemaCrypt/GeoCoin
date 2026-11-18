#!/usr/bin/env bash
set -euo pipefail

# Import credentials only in this release runner's temporary keychain.
identity="${GEOCOIN_MACOS_SIGN_IDENTITY:-}"
certificate="${GEOCOIN_MACOS_CERTIFICATE_B64:-}"
password="${GEOCOIN_MACOS_CERTIFICATE_PASSWORD:-}"
notary_key="${GEOCOIN_MACOS_NOTARY_KEY_B64:-}"
notary_key_id="${GEOCOIN_MACOS_NOTARY_KEY_ID:-}"
notary_issuer="${GEOCOIN_MACOS_NOTARY_ISSUER:-}"

if [[ -z "$identity$certificate$password$notary_key$notary_key_id$notary_issuer" ]]; then
  echo "Using ad-hoc signatures for this macOS build."
  exit 0
fi

if [[ -z "$identity" || -z "$certificate" || -z "$password" ]]; then
  echo "Set the macOS Developer ID identity, PKCS#12 certificate and password together." >&2
  exit 1
fi
if [[ -n "$notary_key$notary_key_id$notary_issuer" && ( -z "$notary_key" || -z "$notary_key_id" || -z "$notary_issuer" ) ]]; then
  echo "Set the App Store Connect API key, key ID and issuer together to enable notarization." >&2
  exit 1
fi

umask 077
temporary="$(mktemp -d "${RUNNER_TEMP:?}/geocoin-signing.XXXXXX")"
keychain="$temporary/GeoCoin.keychain-db"
p12="$temporary/DeveloperID.p12"
p8="$temporary/AuthKey.p8"
trap 'rm -f "$p12" "$p8"; if [[ -n "$keychain" ]]; then security delete-keychain "$keychain" 2>/dev/null || true; fi' EXIT

keychain_password="$(openssl rand -hex 32)"
security create-keychain -p "$keychain_password" "$keychain"
security set-keychain-settings -lut 21600 "$keychain"
security unlock-keychain -p "$keychain_password" "$keychain"
printf '%s' "$certificate" | /usr/bin/base64 -D > "$p12"
security import "$p12" -f pkcs12 -k "$keychain" -P "$password" -T /usr/bin/codesign -x
security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$keychain_password" "$keychain" >/dev/null
if ! security find-identity -v -p codesigning "$keychain" | grep -F "$identity" >/dev/null; then
  echo "The Developer ID identity was not found in the supplied certificate." >&2
  exit 1
fi

profile=""
if [[ -n "$notary_key" ]]; then
  profile="GeoCoin-CI"
  printf '%s' "$notary_key" | /usr/bin/base64 -D > "$p8"
  xcrun notarytool store-credentials "$profile" --key "$p8" --key-id "$notary_key_id" \
    --issuer "$notary_issuer" --keychain "$keychain"
fi

{
  printf 'GEOCOIN_MACOS_SIGN_IDENTITY=%s\n' "$identity"
  printf 'GEOCOIN_MACOS_SIGN_KEYCHAIN=%s\n' "$keychain"
  printf 'GEOCOIN_MACOS_CI_KEYCHAIN=%s\n' "$keychain"
  printf 'GEOCOIN_MACOS_NOTARY_PROFILE=%s\n' "$profile"
} >> "${GITHUB_ENV:?}"

keychain=""
