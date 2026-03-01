#!/bin/bash
# List monitors with their indices for use with GRAB_OVERRIDE_SCREEN
echo "Available monitors:"
echo "-------------------"
xrandr --listmonitors 2>/dev/null | tail -n +2 | while IFS= read -r line; do
    idx=$(echo "$line" | awk '{print $1}' | tr -d ':')
    res=$(echo "$line" | awk '{print $3}')
    name=$(echo "$line" | awk '{print $NF}')
    primary=""
    echo "$line" | grep -q '*' && primary=" (primary)"
    echo "  #${idx}: ${name} ${res}${primary}"
done
echo ""
echo "Usage: GRAB_OVERRIDE_SCREEN=0      - show only monitor #0"
echo "       GRAB_OVERRIDE_SCREEN=0,2    - show monitors #0 and #2"
