#!/system/bin/sh
# Yukari Guard - Target App Configuration
# Run this script through Magisk/KSU/APatch manager

MODDIR=${0%/*}
CONFIG="$MODDIR/config.json"

if [ "$(id -u)" != "0" ]; then
    echo "Error: Root required"
    exit 1
fi

# Ensure config exists
if [ ! -f "$CONFIG" ]; then
    cat > "$CONFIG" <<'EOC'
{
  "enabled": true,
  "enhancedMode": false,
  "targets": []
}
EOC
    chmod 0644 "$CONFIG"
fi

# Read current settings
ENABLED=$(grep -o '"enabled"[[:space:]]*:[[:space:]]*\(true\|false\)' "$CONFIG" | grep -o '\(true\|false\)' | head -1)
[ -z "$ENABLED" ] && ENABLED="true"

ENHANCED=$(grep -o '"enhancedMode"[[:space:]]*:[[:space:]]*\(true\|false\)' "$CONFIG" | grep -o '\(true\|false\)' | head -1)
[ -z "$ENHANCED" ] && ENHANCED="false"

# Extract targets to temp file
TARGETS_FILE=$(mktemp)
sed -n '/"targets"/,/]/p' "$CONFIG" | grep -o '"[^"]*"' | grep -v '"targets"' | tr -d '"' > "$TARGETS_FILE"

save_config() {
    count=$(wc -l < "$TARGETS_FILE" | tr -d ' ')
    {
        echo "{"  
        echo "  \"enabled\": $ENABLED,"
        echo "  \"enhancedMode\": $ENHANCED,"
        echo "  \"targets\": ["
        if [ "$count" -gt 0 ]; then
            i=0
            while IFS= read -r pkg; do
                i=$((i + 1))
                if [ "$i" -lt "$count" ]; then
                    echo "    \"$pkg\","
                else
                    echo "    \"$pkg\""
                fi
            done < "$TARGETS_FILE"
        fi
        echo "  ]"
        echo "}"
    } > "$CONFIG"
    chmod 0644 "$CONFIG"
}

show_targets() {
    if [ -s "$TARGETS_FILE" ]; then
        echo ""
        echo "Current targets:"
        nl -ba "$TARGETS_FILE" | sed 's/^/  /'
    else
        echo ""
        echo "Current targets: (none)"
    fi
}

while true; do
    echo ""
    echo "================================"
    echo " Yukari Guard - Configuration"
    echo "================================"
    show_targets
    echo ""
    echo "  Enabled: $ENABLED | Enhanced: $ENHANCED"
    echo ""
    echo "  1. Add from user apps"
    echo "  2. Add by package name"
    echo "  3. Remove by index"
    echo "  4. Clear all targets"
    echo "  5. Toggle enabled (current: $ENABLED)"
    echo "  6. Toggle enhanced mode (current: $ENHANCED)"
    echo "  7. Save & exit"
    echo "  0. Exit without saving"
    echo ""
    printf "Choice: "
    read -r choice

    case "$choice" in
        1)
            echo ""
            echo "Loading user apps..."
            pm list packages -3 2>/dev/null | sed 's/^package://' | sort > /tmp/yukari_apps.txt
            if [ ! -s /tmp/yukari_apps.txt ]; then
                echo "No user apps found"
                continue
            fi
            echo ""
            total=$(wc -l < /tmp/yukari_apps.txt | tr -d ' ')
            page=0
            per_page=20
            while true; do
                start=$((page * per_page + 1))
                end=$((start + per_page - 1))
                [ "$end" -gt "$total" ] && end=$total
                echo "User apps ($start-$end of $total):"
                sed -n "${start},${end}p" /tmp/yukari_apps.txt | nl -ba -v "$start" | sed 's/^/  /'
                echo ""
                echo "  Enter number to toggle, n=next, p=prev, 0=back"
                printf "Choice: "
                read -r sub
                case "$sub" in
                    n|N) [ "$end" -lt "$total" ] && page=$((page + 1)) ;;
                    p|P) [ "$page" -gt 0 ] && page=$((page - 1)) ;;
                    0) break ;;
                    *[0-9]*)
                        if [ "$sub" -ge 1 ] && [ "$sub" -le "$total" ]; then
                            app=$(sed -n "${sub}p" /tmp/yukari_apps.txt)
                            if grep -q "^${app}$" "$TARGETS_FILE"; then
                                sed -i "/^${app}$/d" "$TARGETS_FILE"
                                echo "Removed: $app"
                            else
                                echo "$app" >> "$TARGETS_FILE"
                                sort -o "$TARGETS_FILE" "$TARGETS_FILE"
                                echo "Added: $app"
                            fi
                        fi
                        ;;
                    *) ;;
                esac
            done
            rm -f /tmp/yukari_apps.txt
            ;;
        2)
            printf "Enter package name: "
            read -r pkg
            if [ -n "$pkg" ]; then
                if grep -q "^${pkg}$" "$TARGETS_FILE"; then
                    echo "Already in targets"
                else
                    echo "$pkg" >> "$TARGETS_FILE"
                    sort -o "$TARGETS_FILE" "$TARGETS_FILE"
                    echo "Added: $pkg"
                fi
            fi
            ;;
        3)
            show_targets
            printf "Enter index to remove: "
            read -r idx
            if [ -n "$idx" ] && [ "$idx" -gt 0 ] 2>/dev/null; then
                count=$(wc -l < "$TARGETS_FILE" | tr -d ' ')
                if [ "$idx" -le "$count" ]; then
                    removed=$(sed -n "${idx}p" "$TARGETS_FILE")
                    sed -i "${idx}d" "$TARGETS_FILE"
                    echo "Removed: $removed"
                else
                    echo "Invalid index"
                fi
            fi
            ;;
        4)
            > "$TARGETS_FILE"
            echo "Cleared all targets"
            ;;
        5)
            if [ "$ENABLED" = "true" ]; then
                ENABLED="false"
            else
                ENABLED="true"
            fi
            echo "Enabled: $ENABLED"
            ;;
        6)
            if [ "$ENHANCED" = "true" ]; then
                ENHANCED="false"
            else
                ENHANCED="true"
            fi
            echo "Enhanced mode: $ENHANCED"
            ;;
        7)
            save_config
            echo ""
            echo "Config saved to $CONFIG"
            echo "Force-stop target apps or reboot to apply."
            rm -f "$TARGETS_FILE"
            exit 0
            ;;
        0)
            rm -f "$TARGETS_FILE"
            exit 0
            ;;
        *)
            echo "Invalid choice"
            ;;
    esac
done
