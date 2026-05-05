#!/bin/bash
# PHASE 0 - UNIT TEST
echo "=== PHASE 0 ==="
ssh docker 'docker compose up -d qemu-ph0'
