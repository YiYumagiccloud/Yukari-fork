#!/system/bin/sh

MODULE_DIR="/data/adb/modules/Yukari"
CONFIG="$MODULE_DIR/config.json"
TEMP_CONFIG="$MODULE_DIR/config.json.tmp"
PACKAGE_LIST="$MODULE_DIR/targets.tmp"

# Tricky Store 列表路径
TRICKY_TARGET="/data/adb/tricky_store/target.txt"

if [ "$(id -u)" != "0" ]; then
    echo "错误：需要 Root 权限"
    exit 1
fi

if [ ! -d "$MODULE_DIR" ]; then
    echo "错误：未找到 Yukari 模块目录 $MODULE_DIR"
    exit 1
fi

# 保留原有的 enabled 状态
ENABLED="true"
if [ -f "$CONFIG" ]; then
    grep -q '"enabled"[[:space:]]*:[[:space:]]*false' "$CONFIG" && ENABLED="false"
fi

# ----- 生成包名列表 -----
if [ -f "$TRICKY_TARGET" ] && [ -s "$TRICKY_TARGET" ]; then
    echo "找到 Tricky Store 的目标列表，正在使用它..."
    # 去除空行和注释行（#开头）
    grep -v '^[[:space:]]*$' "$TRICKY_TARGET" | grep -v '^[[:space:]]*#' > "$PACKAGE_LIST"
    
    if [ ! -s "$PACKAGE_LIST" ]; then
        echo "警告：Tricky Store 列表过滤后为空，回退到生成所有第三方应用。"
        # 如果过滤后为空，回退到原方案
        CURRENT_USER="$(am get-current-user 2>/dev/null)"
        [ -z "$CURRENT_USER" ] && CURRENT_USER="0"
        pm list packages -3 --user "$CURRENT_USER" 2>/dev/null |
            sed -n 's/^package://p' |
            sort -u > "$PACKAGE_LIST"
    fi
else
    echo "未找到 Tricky Store 列表，正在生成所有第三方应用..."
    CURRENT_USER="$(am get-current-user 2>/dev/null)"
    [ -z "$CURRENT_USER" ] && CURRENT_USER="0"
    pm list packages -3 --user "$CURRENT_USER" 2>/dev/null |
        sed -n 's/^package://p' |
        sort -u > "$PACKAGE_LIST"
fi
# ----- 生成结束 -----

PACKAGE_COUNT="$(wc -l < "$PACKAGE_LIST" | tr -d ' ')"

{
    echo "{"
    echo "  \"enabled\": $ENABLED,"
    echo "  \"targets\": ["

    INDEX=0
    while IFS= read -r PACKAGE_NAME; do
        [ -z "$PACKAGE_NAME" ] && continue
        INDEX=$((INDEX + 1))
        if [ "$INDEX" -lt "$PACKAGE_COUNT" ]; then
            printf '    "%s",\n' "$PACKAGE_NAME"
        else
            printf '    "%s"\n' "$PACKAGE_NAME"
        fi
    done < "$PACKAGE_LIST"

    echo "  ]"
    echo "}"
} > "$TEMP_CONFIG"

chmod 0644 "$TEMP_CONFIG"
mv -f "$TEMP_CONFIG" "$CONFIG"
rm -f "$PACKAGE_LIST"

echo "--------------------------------"
echo "Yukari 配置已更新。"
echo "  启用状态：$ENABLED"
echo "  用户 ID：$CURRENT_USER"
echo "  目标应用数量：$PACKAGE_COUNT"
echo "  配置文件路径：$CONFIG"
echo "--------------------------------"
echo
echo "请强制停止目标应用或重启手机以使配置生效。"

# ----- 延迟关闭，方便查看输出 -----
echo "窗口将在 9 秒后自动关闭……"
sleep 9
