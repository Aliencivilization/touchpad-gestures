#!/usr/bin/env bash
# ============================================================================
# touchpad-gestures — установка демона жестов тачпада
#
#   ./install.sh            — пользовательская установка (без sudo, рекомендуется)
#   ./install.sh --system   — системная установка в /usr/local/bin (нужен sudo)
#   ./install.sh --uninstall— удалить демон и его сервис
#
# Что делает:
#   1. Проверяет зависимости (cmake, g++, libevdev, dbus-1, libpulse)
#   2. Собирает демон из main.cpp
#   3. Ставит бинарь (~/.local/bin или /usr/local/bin)
#   4. При необходимости — udev-правило для доступа к тачпаду без root
#   5. Ставит и включает systemd user-сервис
#   6. Проверяет, что сервис поднялся
# ============================================================================
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

BIN_NAME="touchpad-gestures"
UNIT_NAME="touchpad-gestures.service"
MODE="user"

for arg in "$@"; do
  case "$arg" in
    --system)   MODE="system" ;;
    --uninstall) MODE="uninstall" ;;
    -h|--help)  grep '^#' "$0" | sed 's/^# \{0,1\}//' | tail -n +2; exit 0 ;;
    *) echo "неизвестный аргумент: $arg (см. --help)"; exit 2 ;;
  esac
done

err()  { echo "✘ $*" >&2; }
ok()   { echo "✔ $*"; }
info() { echo "→ $*"; }

# ---------------------------------------------------------------- uninstall
if [[ "$MODE" == "uninstall" ]]; then
  info "останавливаю и отключаю сервис..."
  systemctl --user disable --now "$UNIT_NAME" 2>/dev/null || true
  rm -f "${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/$UNIT_NAME"
  systemctl --user daemon-reload
  rm -f "$HOME/.local/bin/$BIN_NAME"
  if [[ -w /usr/local/bin && -e /usr/local/bin/$BIN_NAME ]]; then
    sudo rm -f "/usr/local/bin/$BIN_NAME"
  fi
  # udev-правило: удаляем только если есть права (иначе — с sudo, молча)
  if [[ -w /etc/udev/rules.d/99-touchpad-evdev.rules ]]; then
    rm -f /etc/udev/rules.d/99-touchpad-evdev.rules
  elif command -v sudo >/dev/null; then
    sudo -n rm -f /etc/udev/rules.d/99-touchpad-evdev.rules 2>/dev/null || \
      echo "  (udev-правило не тронуто — снимите вручную или запустите с sudo)" >&2
  fi
  ok "удалено (исходники и бэкапы не тронуты)"
  exit 0
fi

# ------------------------------------------------------- 1. зависимости
info "проверяю зависимости..."
missing=()
command -v cmake   >/dev/null || missing+=(cmake)
command -v g++     >/dev/null || missing+=(gcc)
command -v pkg-config >/dev/null || missing+=(pkgconfig)
if command -v pkg-config >/dev/null; then
  pkg-config --exists libevdev || missing+=(libevdev)
  pkg-config --exists dbus-1    || missing+=(dbus)
  pkg-config --exists libpulse  || missing+=(libpulse)
fi
if ((${#missing[@]})); then
  err "нет зависимостей: ${missing[*]}"
  echo "  Arch:  sudo pacman -S cmake gcc pkgconf libevdev dbus libpulse" >&2
  echo "  Debian: sudo apt install cmake g++ pkg-config libevdev-dev libdbus-1-dev libpulse-dev" >&2
  exit 1
fi
ok "зависимости на месте"

# ----------------------------------------------------------------- 2. сборка
info "собираю..."
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j"$(nproc)" 2>&1 | grep -E 'warning|error' >&2 || true
[[ -x build/$BIN_NAME ]] || { err "сборка не удалась"; exit 1; }
ok "собрано: build/$BIN_NAME"

# ------------------------------------------------- 3..5 в зависимости от режима
if [[ "$MODE" == "system" ]]; then
  SUDO="sudo"
else
  SUDO=""
fi

# --- 3. бинарь
if [[ "$MODE" == "system" ]]; then
  info "ставлю бинарь в /usr/local/bin (нужен sudo)..."
  $SUDO install -m755 "build/$BIN_NAME" "/usr/local/bin/$BIN_NAME"
  BIN_PATH="/usr/local/bin/$BIN_NAME"
else
  mkdir -p "$HOME/.local/bin"
  # сервис может держать файл открытым — сначала останавливаем
  systemctl --user stop "$UNIT_NAME" 2>/dev/null || true
  install -m755 "build/$BIN_NAME" "$HOME/.local/bin/$BIN_NAME"
  BIN_PATH="$HOME/.local/bin/$BIN_NAME"
fi
ok "бинарь: $BIN_PATH"

# --- 4. права на тачпад: группа input или udev-правило
# Правило по стандартной метке ID_INPUT_TOUCHPAD (ставится udev для ЛЮБОГО
# тачпада, независимо от модели и имени) — универсально для всех ноутбуков.
TP_NODE=""
for n in /dev/input/event*; do
  if udevadm info -q property --name="$n" 2>/dev/null | grep -q '^ID_INPUT_TOUCHPAD=1'; then
    TP_NODE="$n"; break
  fi
done
if [[ -n "$TP_NODE" && ! -r "$TP_NODE" ]] && ! groups | grep -qw input; then
  info "ставлю udev-правило для доступа к тачпаду..."
  echo 'KERNEL=="event*", ENV{ID_INPUT_TOUCHPAD}=="1", MODE="0664", GROUP="input"' | \
    $SUDO tee /etc/udev/rules.d/99-touchpad-evdev.rules >/dev/null
  $SUDO udevadm control --reload-rules && $SUDO udevadm trigger
  if [[ "$MODE" == "user" ]]; then
    info "входите в группу input: sudo usermod -aG input $USER (после перелогина заработает)"
  fi
fi

# --- 5. systemd user-сервис
info "ставлю systemd-сервис..."
UNIT_DST="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/$UNIT_NAME"
mkdir -p "$(dirname "$UNIT_DST")"
# подставляем фактический путь к бинарю в юнит
sed "s|^ExecStart=.*|ExecStart=$BIN_PATH|" "$UNIT_NAME" > "$UNIT_DST"
systemctl --user daemon-reload
systemctl --user enable --now "$UNIT_NAME"
ok "сервис включён и запущен"

# ------------------------------------------------------------- 6. верификация
sleep 1
if systemctl --user is-active --quiet "$UNIT_NAME"; then
  RSS=$(systemctl --user show "$UNIT_NAME" -p MemoryCurrent --no-pager | cut -d= -f2)
  ok "сервис активен (RSS: $((RSS / 1024)) КБ)"
else
  err "сервис не активен — смотрите: journalctl --user -u $UNIT_NAME -n 20"
  exit 1
fi

echo
echo "Готово! Жесты:"
echo "  правый край палец вверх/вниз  → громкость"
echo "  правый край, 2 пальца вниз    → play/pause"
echo "  левый край вверх/вниз         → next/prev трек"
echo "  двойной тап по левому краю    → play/pause"
echo "  печатаете? жесты подавлены 400 мс после последней клавиши (DWT)"
echo
echo "Логи:   journalctl --user -u $BIN_NAME -f"
echo "Тест:   $BIN_NAME -v          (запуск вручную с логами)"
echo "Откат:  $BIN_NAME --uninstall или восстановите python-версию из бэкапа"