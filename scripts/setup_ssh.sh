#!/usr/bin/env bash
# scripts/setup_ssh.sh
# One-time SSH passwordless key setup from Client to Server.
# Usage: ./scripts/setup_ssh.sh [server_user] [server_host]
# Default: ./scripts/setup_ssh.sh koko 192.168.50.1

set -euo pipefail

SERVER_USER="${1:-koko}"
SERVER_HOST="${2:-192.168.50.1}"

echo "================================================================="
echo "   Setting up passwordless SSH: Client -> ${SERVER_USER}@${SERVER_HOST}"
echo "================================================================="

# 1. Check if SSH is already working without password
if ssh -o BatchMode=yes -o ConnectTimeout=3 "${SERVER_USER}@${SERVER_HOST}" "echo SSH_TEST_OK" &>/dev/null; then
    echo "[OK] Passwordless SSH is already configured and working!"
    exit 0
fi

# 2. Generate SSH key if not present
SSH_KEY="$HOME/.ssh/id_ed25519"
if [[ ! -f "$SSH_KEY" && ! -f "$HOME/.ssh/id_rsa" ]]; then
    echo "[INFO] Generating new Ed25519 SSH keypair..."
    ssh-keygen -t ed25519 -N "" -f "$SSH_KEY"
elif [[ -f "$HOME/.ssh/id_rsa" && ! -f "$SSH_KEY" ]]; then
    SSH_KEY="$HOME/.ssh/id_rsa"
fi

# 3. Copy key to server
echo "[INFO] Copying public key to ${SERVER_USER}@${SERVER_HOST}..."
echo "       (You may be prompted for ${SERVER_USER}'s password once)"
ssh-copy-id -o StrictHostKeyChecking=accept-new -i "${SSH_KEY}.pub" "${SERVER_USER}@${SERVER_HOST}"

# 4. Verify passwordless access
echo "[INFO] Verifying passwordless SSH access..."
if ssh -o BatchMode=yes -o ConnectTimeout=5 "${SERVER_USER}@${SERVER_HOST}" "echo SSH_VERIFIED" | grep -q "SSH_VERIFIED"; then
    echo "[SUCCESS] Passwordless SSH successfully verified!"
else
    echo "[ERROR] SSH verification failed. Please check credentials and network connectivity."
    exit 1
fi
