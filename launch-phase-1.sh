#!/bin/bash
# PHASE 1 - UNIT TEST
echo "=== PHASE 1 ==="
ssh docker 'docker compose up -d qemu-ph1'
