#!/usr/bin/env bash
set -euo pipefail

# GeoCoin public node firewall profile.
# Public surface: SSH and P2P only. RPC must stay loopback-only.

if [[ "${EUID}" -ne 0 ]]; then
  echo "Run as root" >&2
  exit 1
fi

ufw --force reset
ufw default deny incoming
ufw default allow outgoing
ufw allow 22/tcp
ufw allow 3030/tcp
ufw deny 3033/tcp
ufw deny 13030/tcp
ufw deny 13033/tcp
ufw --force enable

# Defense in depth before UFW chains. This should make external RPC fail even
# if a future service accidentally binds 0.0.0.0.
for port in 3033 13030 13033; do
  while iptables -C INPUT ! -i lo -p tcp --dport "${port}" -j REJECT --reject-with tcp-reset 2>/dev/null; do
    iptables -D INPUT ! -i lo -p tcp --dport "${port}" -j REJECT --reject-with tcp-reset
  done
  if ! iptables -C INPUT ! -i lo -p tcp --dport "${port}" -j DROP 2>/dev/null; then
    iptables -I INPUT 1 ! -i lo -p tcp --dport "${port}" -j DROP
  fi

  if command -v ip6tables >/dev/null 2>&1; then
    while ip6tables -C INPUT ! -i lo -p tcp --dport "${port}" -j REJECT --reject-with tcp-reset 2>/dev/null; do
      ip6tables -D INPUT ! -i lo -p tcp --dport "${port}" -j REJECT --reject-with tcp-reset
    done
    if ! ip6tables -C INPUT ! -i lo -p tcp --dport "${port}" -j DROP 2>/dev/null; then
      ip6tables -I INPUT 1 ! -i lo -p tcp --dport "${port}" -j DROP
    fi
  fi
done

ufw status verbose
ss -lntp | grep -E '3030|3033|13030|13033' || true

cat <<'EOF'

Provider security group must also allow only:
- 22/tcp
- 3030/tcp

Provider security group must deny:
- 3033/tcp
- 13030/tcp
- 13033/tcp
- every unrelated public port

If an external TCP connect succeeds on random closed ports, the provider-side
security group is still permissive and must be fixed in the provider control panel.
EOF
