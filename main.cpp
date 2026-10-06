// touchpad-gestures — демон жестов тачпада (C++26, linux evdev + D-Bus + libpulse)
//
// Ресурсы: блокирующий poll(), в простое ~0% CPU; без fork (никаких wpctl/playerctl).
//
// Жесты (см. ~/Downloads/touchpad-gestures.md):
//   правый край, палец вверх/вниз  — громкость = позиция пальца
//   правый край, 2 пальца вниз     — play/pause
//   левый край, вверх/вниз         — next/prev трек
//   двойной тап по левому краю     — play/pause
//
// Защита от случайных касаний:
//   * DWT: TYPING_GUARD_MS после последней клавиши жесты не срабатывают;
//     касание, начатое во время печати, гасится до самого отрыва
//   * arm-зона: жест работает только если касание НАЧАЛОСЬ в этой зоне
//   * фильтр ладони (MT_TOOL_PALM) и жестов с 3+ пальцами
//   * пороги намеренного движения (ARM_MOVE_PX, TRACK_THRESHOLD)

#include <dbus/dbus.h>
#include <libevdev/libevdev.h>
#include <linux/input-event-codes.h>
#include <linux/input.h>
#include <pulse/pulseaudio.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <print>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

// ───────────────────────────── конфигурация ─────────────────────────────
// Тачпад определяется автоматически по возможностям ядра (см. open_touchpad),
// имена устройств нигде не захардкожены.

static constexpr double LEFT_EDGE  = 0.05;
static constexpr double RIGHT_EDGE = 0.95;

static constexpr int       ARM_MOVE_PX             = 25;   // «взвод» жеста в зоне, px
static constexpr int       TRACK_THRESHOLD         = 60;   // px — смена трека (было 30)
static constexpr long long TRACK_COOLDOWN_MS       = 400;
static constexpr int       TWO_SWIPE_THRESHOLD     = 120;  // px вниз двумя пальцами
static constexpr int       TAP_MOVE_THRESHOLD      = 20;   // px — смещение отменяет тап
static constexpr long long DOUBLE_TAP_TIMEOUT_MS   = 400;
static constexpr long long TYPING_GUARD_MS         = 400;  // DWT

static constexpr int PALM_TOOL_TYPE = 1;  // MT_TOOL_PALM

static bool g_verbose = false;
static volatile std::sig_atomic_t g_stop = 0;
static long long g_last_key_ms = -1'000'000'000;

static void on_signal(int) { g_stop = 1; }

static long long now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

static bool typing_guarded() { return now_ms() - g_last_key_ms < TYPING_GUARD_MS; }

enum class Zone : uint8_t { None, Left, Center, Right };

static Zone zone_of(int x, int max_x) {
  const double r = static_cast<double>(x) / static_cast<double>(max_x);
  if (r < LEFT_EDGE) return Zone::Left;
  if (r > RIGHT_EDGE) return Zone::Right;
  return Zone::Center;
}

// ─────────────────────────── MPRIS: любой плеер ───────────────────────────
// Предпочтительно шлём в playerctld (D-Bus авто-запуск): он сам выбирает
// «последний» активный плеер — Яндекс Музыку, YouTube в браузере, VLC и т.д.
// Если playerctld недоступен — сами ищем плееры в шине (ListNames).
struct Mpris {
  static constexpr const char* PTLD = "org.mpris.MediaPlayer2.playerctld";
  DBusConnection* conn = nullptr;

  bool init() {
    DBusError err;
    dbus_error_init(&err);
    conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (dbus_error_is_set(&err)) {
      std::println(stderr, "D-Bus: {}", err.message ? err.message : "?");
      dbus_error_free(&err);
    }
    return conn != nullptr;
  }

  std::string resolve() {
    if (!conn) return {};
    if (activate_playerctld()) return PTLD;
    const auto players = list_players();
    for (const auto& p : players)
      if (playback_status(p) == "Playing") return p;
    if (!players.empty()) return players.front();
    return {};
  }

  bool send(const char* method) {
    std::string dest = resolve();
    if (dest.empty()) {
      if (g_verbose) std::println(stderr, "MPRIS: нет активного плеера ({})", method);
      return false;
    }
    if (try_send(dest, method)) return true;
    const auto players = list_players();
    for (const auto& p : players)
      if (p != dest && try_send(p, method)) return true;
    return false;
  }

 private:
  bool activate_playerctld() {
    DBusError err;
    dbus_error_init(&err);
    const char* name = PTLD;
    uint32_t flags = 0;
    DBusMessage* m = dbus_message_new_method_call(
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "StartServiceByName");
    if (!m) return false;
    dbus_message_append_args(m, DBUS_TYPE_STRING, &name,
                             DBUS_TYPE_UINT32, &flags, DBUS_TYPE_INVALID);
    DBusMessage* r = dbus_connection_send_with_reply_and_block(conn, m, 1500, &err);
    dbus_message_unref(m);
    const bool ok = r != nullptr;
    if (r) dbus_message_unref(r);
    if (dbus_error_is_set(&err)) dbus_error_free(&err);
    return ok;
  }

  std::vector<std::string> list_players() const {
    std::vector<std::string> out;
    if (!conn) return out;
    DBusError err;
    dbus_error_init(&err);
    DBusMessage* m = dbus_message_new_method_call(
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "ListNames");
    if (!m) return out;
    DBusMessage* r = dbus_connection_send_with_reply_and_block(conn, m, 400, &err);
    dbus_message_unref(m);
    if (!r) { dbus_error_free(&err); return out; }
    DBusMessageIter it;
    if (dbus_message_iter_init(r, &it) &&
        dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
      DBusMessageIter arr;
      dbus_message_iter_recurse(&it, &arr);
      while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
        const char* s = nullptr;
        dbus_message_iter_get_basic(&arr, &s);
        const std::string n = s ? s : "";
        if (n.rfind("org.mpris.MediaPlayer2.", 0) == 0 && n != PTLD) out.push_back(n);
        dbus_message_iter_next(&arr);
      }
    }
    dbus_message_unref(r);
    if (dbus_error_is_set(&err)) dbus_error_free(&err);
    return out;
  }

  std::string playback_status(const std::string& dest) const {
    std::string out;
    if (!conn) return out;
    DBusError err;
    dbus_error_init(&err);
    DBusMessage* m = dbus_message_new_method_call(
        dest.c_str(), "/org/mpris/MediaPlayer2",
        "org.freedesktop.DBus.Properties", "Get");
    if (!m) return out;
    const char* iface = "org.mpris.MediaPlayer2.Player";
    const char* prop = "PlaybackStatus";
    dbus_message_append_args(m, DBUS_TYPE_STRING, &iface,
                             DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID);
    DBusMessage* r = dbus_connection_send_with_reply_and_block(conn, m, 200, &err);
    dbus_message_unref(m);
    if (r) {
      DBusMessageIter it, vi;
      if (dbus_message_iter_init(r, &it) &&
          dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_VARIANT) {
        dbus_message_iter_recurse(&it, &vi);
        if (dbus_message_iter_get_arg_type(&vi) == DBUS_TYPE_STRING) {
          const char* s = nullptr;
          dbus_message_iter_get_basic(&vi, &s);
          if (s) out = s;
        }
      }
      dbus_message_unref(r);
    }
    if (dbus_error_is_set(&err)) dbus_error_free(&err);
    return out;
  }

  bool try_send(const std::string& dest, const char* method) {
    if (!conn) return false;
    DBusError err;
    dbus_error_init(&err);
    DBusMessage* m = dbus_message_new_method_call(
        dest.c_str(), "/org/mpris/MediaPlayer2",
        "org.mpris.MediaPlayer2.Player", method);
    if (!m) return false;
    DBusMessage* r = dbus_connection_send_with_reply_and_block(conn, m, 400, &err);
    dbus_message_unref(m);
    if (dbus_error_is_set(&err)) {
      if (g_verbose)
        std::println(stderr, "MPRIS {} → {}: {}", method, dest, err.message ? err.message : "?");
      dbus_error_free(&err);
      return false;
    }
    const bool ok = r != nullptr;
    if (r) dbus_message_unref(r);
    if (g_verbose) std::println("MPRIS: {} → {}", method, dest);
    return ok;
  }
};

// ─────────────────────── Громкость: libpulse (без fork) ───────────────────────
// Одно постоянное соединение с pipewire-pulse; set_sink_volume — дешёвый вызов,
// вместо форка wpctl на каждое событие тачпада.
struct Pulse {
  pa_mainloop* ml = nullptr;
  pa_context* ctx = nullptr;
  std::string sink_name;
  int channels = 0;
  int last_set = -1;
  long long next_retry_ms = 0;
  bool ready = false;

  ~Pulse() { disconnect(); }

  void disconnect() {
    if (ctx) { pa_context_disconnect(ctx); pa_context_unref(ctx); ctx = nullptr; }
    if (ml) { pa_mainloop_free(ml); ml = nullptr; }
    ready = false;
    sink_name.clear();
    channels = 0;
  }

  // Колбэки идут строго цепочкой: READY → server_info → sink_info → ready.
  // state_cb НЕ может вычислять ready — данные приходят асинхронно позже.
  static void on_sink_info(pa_context*, const pa_sink_info* info, int eol, void* userdata) {
    auto* self = static_cast<Pulse*>(userdata);
    if (info && eol == 0 && self->channels == 0) {
      self->channels = static_cast<int>(info->channel_map.channels);
      self->ready = true;
      if (g_verbose)
        std::println("pulse: sink '{}', каналов {}", self->sink_name, self->channels);
    }
  }

  static void on_server_info(pa_context* c, const pa_server_info* i, void* userdata) {
    auto* self = static_cast<Pulse*>(userdata);
    if (!i || !i->default_sink_name) return;
    self->sink_name = i->default_sink_name;
    self->channels = 0;   // новый sink → перечитываем каналы, ready станет true позже
    pa_operation* op = pa_context_get_sink_info_by_name(
        c, self->sink_name.c_str(), &Pulse::on_sink_info, userdata);
    if (op) pa_operation_unref(op);
  }

  // default sink мог смениться (подключили наушники) — перечитываем
  static void on_subscribe(pa_context* c, pa_subscription_event_type_t t, uint32_t, void* userdata) {
    if ((t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SERVER) {
      pa_operation* op = pa_context_get_server_info(c, &Pulse::on_server_info, userdata);
      if (op) pa_operation_unref(op);
    }
  }

  static void state_cb(pa_context* c, void* userdata) {
    auto* self = static_cast<Pulse*>(userdata);
    const pa_context_state_t st = pa_context_get_state(c);
    if (st == PA_CONTEXT_READY) {
      self->ready = false;
      self->last_set = -1;   // после реконнекта громкость выставим заново
      pa_operation* op = pa_context_get_server_info(c, &Pulse::on_server_info, userdata);
      if (op) pa_operation_unref(op);
      pa_context_set_subscribe_callback(c, &Pulse::on_subscribe, userdata);
      pa_operation* s = pa_context_subscribe(c, PA_SUBSCRIPTION_MASK_SERVER, nullptr, nullptr);
      if (s) pa_operation_unref(s);
    } else if (st == PA_CONTEXT_FAILED || st == PA_CONTEXT_TERMINATED) {
      self->ready = false;
    }
  }

  bool ensure(long long now) {
    if (ready && ctx && pa_context_get_state(ctx) == PA_CONTEXT_READY) return true;
    if (now < next_retry_ms) return false;
    next_retry_ms = now + 3000;  // не долбимся чаще раза в 3 сек
    if (ctx) disconnect();

    if (!ml) {
      ml = pa_mainloop_new();
      if (!ml) return false;
    }
    ctx = pa_context_new(pa_mainloop_get_api(ml), "touchpad-gestures");
    if (!ctx) return false;
    pa_context_set_state_callback(ctx, &Pulse::state_cb, this);
    if (pa_context_connect(ctx, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
      disconnect();
      return false;
    }
    // прокачиваем mainloop до READY+данных; iterate возвращает <0 только при ошибке,
    // поэтому прерываемся лишь по ошибке или дедлайну (обычно единицы мс)
    const long long deadline = now + 1000;
    while (!ready) {
      if (pa_mainloop_iterate(ml, 0, nullptr) < 0) break;
      if (now_ms() > deadline) break;
      if (pa_context_get_state(ctx) == PA_CONTEXT_FAILED ||
          pa_context_get_state(ctx) == PA_CONTEXT_TERMINATED) break;
      usleep(200);
    }
    if (!ready && g_verbose) std::println(stderr, "pulse: не подключилось (state {})", (int)pa_context_get_state(ctx));
    return ready;
  }

  void set_volume(int vol, long long now) {
    if (!ensure(now)) return;
    if (vol != last_set) {
      pa_cvolume cv;
      pa_cvolume_set(&cv, static_cast<unsigned>(channels),
                     static_cast<pa_volume_t>(PA_VOLUME_NORM * vol / 100));
      pa_operation* op = pa_context_set_sink_volume_by_name(ctx, sink_name.c_str(), &cv, nullptr, nullptr);
      if (op) {
        // дожидаемся применения (сервер отвечает за единицы мс), иначе можем
        // выйти до того, как pipewire-pulse применит значение
        const long long deadline = now_ms() + 300;
        while (pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
          if (pa_mainloop_iterate(ml, 0, nullptr) < 0 || now_ms() > deadline) break;
          usleep(200);
        }
        const bool done = pa_operation_get_state(op) == PA_OPERATION_DONE;
        pa_operation_unref(op);
        if (done) last_set = vol;
        if (g_verbose) std::println("pulse: set_volume {}% → {} ({})", vol, sink_name,
                                    done ? "ok" : "timeout/err");
      }
    }
    // прокачиваем входящие ответы (ошибок нет — просто обрабатываем события)
    for (int i = 0; i < 16; ++i)
      if (pa_mainloop_iterate(ml, 0, nullptr) < 0) break;
  }
};

// ─────────────────────────── Жесты ───────────────────────────
struct Gestures {
  int max_x = 3683;
  int max_y = 1897;

  int cur_x = 0, cur_y = 0;

  // одиночное касание
  bool touching = false;
  Zone arm_zone = Zone::None;   // зона, ГДЕ НАЧАЛОСЬ касание (arm-защита)
  int start_y = 0;
  int start_x = 0;
  bool moved = false;           // превысило TAP_MOVE_THRESHOLD — не тап
  bool palm = false;            // MT_TOOL_PALM
  bool guarded = false;         // касание начато во время печати — гасим целиком

  // левый край: смена трека
  int track_anchor = 0;
  long long track_anchor_ms = 0;
  long long last_track_ms = 0;

  // двойной тап
  long long last_tap_ms = -10'000;

  // два пальца (правый край, свайп вниз)
  bool two_finger = false;
  bool two_zone_ok = false;
  int two_start_y = 0;
  bool two_fired = false;

  // громкость
  int last_vol = -1;

  Mpris* mpris = nullptr;
  Pulse* pulse = nullptr;

  void on_touch_start(long long now) {
    touching = true;
    arm_zone = zone_of(cur_x, max_x);
    start_y = cur_y;
    start_x = cur_x;
    moved = false;
    palm = false;
    guarded = typing_guarded();   // начали во время печати — жестов не будет
    track_anchor = cur_y;
    track_anchor_ms = now;
    two_finger = false;
    two_fired = false;
    if (arm_zone == Zone::Right) last_vol = -1;
  }

  void on_touch_end(long long now) {
    // двойной тап по левому краю
    if (!guarded && !palm && !moved && arm_zone == Zone::Left &&
        now - last_tap_ms < DOUBLE_TAP_TIMEOUT_MS) {
      mpris->send("PlayPause");
      if (g_verbose) std::println("жест: двойной тап → PlayPause");
      last_tap_ms = -10'000;
    } else if (arm_zone == Zone::Left && !moved && !guarded && !palm) {
      last_tap_ms = now;
    }
    touching = false;
    arm_zone = Zone::None;
    two_finger = false;
    last_vol = -1;
  }

  void on_move(long long now) {
    if (!touching || palm) return;
    if (std::abs(cur_x - start_x) > TAP_MOVE_THRESHOLD ||
        std::abs(cur_y - start_y) > TAP_MOVE_THRESHOLD)
      moved = true;
    if (guarded || typing_guarded()) return;   // DWT: молчим

    // два пальца: правый край, свайп вниз → play/pause
    if (two_finger) {
      if (two_zone_ok && !two_fired && (cur_y - two_start_y) > TWO_SWIPE_THRESHOLD) {
        mpris->send("PlayPause");
        two_fired = true;
        if (g_verbose) std::println("жест: 2 пальца вниз → PlayPause");
      }
      return;
    }

    // arm-зона: касание должно были НАЧАТЬСЯ в этой зоне
    if (arm_zone == Zone::Right) {
      // взвод: только после намеренного движения в зоне
      if (std::abs(cur_y - start_y) < ARM_MOVE_PX && last_vol < 0) return;
      int vol = static_cast<int>((1.0 - static_cast<double>(cur_y) / max_y) * 100.0);
      vol = std::clamp(vol, 0, 100);
      if (vol != last_vol) {
        last_vol = vol;
        pulse->set_volume(vol, now);
        if (g_verbose) std::println("громкость: {}%", vol);
      }
    } else if (arm_zone == Zone::Left) {
      const int delta = cur_y - track_anchor;
      // осознанный свайп: заметное смещение и не вялое дрейфование
      const long long dt = now - track_anchor_ms;
      if (std::abs(delta) >= TRACK_THRESHOLD && dt > 0 &&
          std::abs(delta) * 1000 / dt >= 150 &&               // ≥0.15 px/мс
          now - last_track_ms > TRACK_COOLDOWN_MS) {
        const char* cmd = delta > 0 ? "Previous" : "Next";     // вниз — назад, вверх — вперёд
        mpris->send(cmd);
        if (g_verbose) std::println("жест: {} (Δ{}px)", cmd, delta);
        last_track_ms = now;
        track_anchor = cur_y;
        track_anchor_ms = now;
      }
    }
  }

  void handle(const input_event& ev, long long now) {
    switch (ev.type) {
      case EV_KEY:
        if (ev.code == BTN_TOUCH) {
          if (ev.value == 1) on_touch_start(now);
          else if (ev.value == 0) on_touch_end(now);
        } else if (ev.code == BTN_TOOL_DOUBLETAP) {
          if (ev.value == 1 && touching && !guarded) {
            two_finger = true;
            two_zone_ok = arm_zone == Zone::Right;
            two_start_y = cur_y;
            two_fired = false;
          } else if (ev.value == 0) {
            two_finger = false;
          }
        }
        break;

      case EV_ABS:
        switch (ev.code) {
          case ABS_MT_POSITION_X: cur_x = ev.value; break;
          case ABS_MT_POSITION_Y:
            cur_y = ev.value;
            on_move(now);
            break;
          case ABS_MT_TOOL_TYPE:
            if (ev.value == PALM_TOOL_TYPE) palm = true;
            break;
          default: break;
        }
        break;

      default: break;
    }
  }
};

// ─────────────────────── Доступ к устройствам ввода ───────────────────────
// ——— Поиск тачпада: по возможностям ядра, без захардкоженных имён ———
// Любой тачпад (Goodix, ELAN, Synaptics, RMI...) поддерживает MT-координаты
// и BTN_TOUCH — этого достаточно для идентификации.
static int open_touchpad(int* max_x, int* max_y, char* name_out, size_t name_sz) {
  for (int i = 0; i < 256; ++i) {
    char path[64];
    snprintf(path, sizeof(path), "/dev/input/event%d", i);
    const int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) continue;
    libevdev* dev = nullptr;
    if (libevdev_new_from_fd(fd, &dev) < 0) { close(fd); continue; }
    const bool is_touchpad =
        libevdev_has_event_code(dev, EV_ABS, ABS_MT_POSITION_X) &&
        libevdev_has_event_code(dev, EV_KEY, BTN_TOUCH);
    if (!is_touchpad) {
      libevdev_free(dev);
      close(fd);
      continue;
    }
    if (name_out && name_sz) {
      const char* n = libevdev_get_name(dev);
      snprintf(name_out, name_sz, "%s", n ? n : "?");
    }
    if (max_x && max_y) {
      if (const input_absinfo* a = libevdev_get_abs_info(dev, ABS_MT_POSITION_X); a && a->maximum > 0)
        *max_x = a->maximum;
      if (const input_absinfo* a = libevdev_get_abs_info(dev, ABS_MT_POSITION_Y); a && a->maximum > 0)
        *max_y = a->maximum;
    }
    libevdev_free(dev);   // fd остаётся нашим, libevdev больше не нужен
    return fd;
  }
  return -1;
}

// DWT: читаем клавиатуры (параллельно с тачпадом, без захвата)
static std::vector<int> open_keyboards() {
  std::vector<int> fds;
  for (int i = 0; i < 256; ++i) {
    char path[64];
    snprintf(path, sizeof(path), "/dev/input/event%d", i);
    const int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) continue;
    libevdev* dev = nullptr;
    if (libevdev_new_from_fd(fd, &dev) < 0) { close(fd); continue; }
    const bool kb = libevdev_has_event_code(dev, EV_KEY, KEY_A) &&
                    libevdev_has_event_code(dev, EV_KEY, KEY_Z) &&
                    libevdev_has_event_code(dev, EV_KEY, KEY_SPACE) &&
                    !libevdev_has_event_code(dev, EV_ABS, ABS_MT_POSITION_X);
    libevdev_free(dev);
    if (kb) fds.push_back(fd);
    else close(fd);
  }
  return fds;
}

// ─────────────────────────────── main ───────────────────────────────
int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IOLBF, 0);   // логи сразу видны в journalctl
  int test_vol = -1;                     // --set-volume N: самопроверка pulse-пути
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) g_verbose = true;
    else if (strcmp(argv[i], "--set-volume") == 0 && i + 1 < argc) test_vol = atoi(argv[++i]);
  }

  if (test_vol >= 0) {
    // быстрый тест: подключаемся к pulse и выставляем громкость, затем выходим
    Pulse p;
    p.set_volume(std::clamp(test_vol, 0, 100), now_ms());
    std::println("test: ready={} sink='{}' channels={}", p.ready, p.sink_name, p.channels);
    return p.ready ? 0 : 1;
  }

  std::signal(SIGTERM, on_signal);
  std::signal(SIGINT, on_signal);
  std::signal(SIGPIPE, SIG_IGN);

  Mpris mpris;
  if (!mpris.init()) {
    std::println(stderr, "ошибка: сессионная D-Bus шина недоступна");
    return 1;
  }

  Pulse pulse;
  Gestures g;
  g.mpris = &mpris;
  g.pulse = &pulse;

  std::vector<int> kbd_fds = open_keyboards();
  if (kbd_fds.empty())
    std::println(stderr, "предупреждение: клавиатуры не найдены, DWT отключён");

  int touch_fd = -1;
  char tp_name[128] = "?";
  std::println("поиск тачпада (автоопределение)...");

  bool printed_wait = false;
  while (!g_stop) {
    if (touch_fd < 0) {
      touch_fd = open_touchpad(&g.max_x, &g.max_y, tp_name, sizeof(tp_name));
      if (touch_fd < 0) {
        if (!printed_wait) {
          std::println(stderr, "тачпад не найден, ждём появления (переподключение)...");
          printed_wait = true;
        }
        usleep(500'000);
        continue;
      }
      printed_wait = false;
      std::println("тачпад: {} ({}x{}), клавиатур: {}", tp_name, g.max_x, g.max_y, kbd_fds.size());
    }

    // poll: тачпад + клавиатуры, блокирующий — 0% CPU в простое
    std::vector<pollfd> pfds;
    pfds.push_back({touch_fd, POLLIN, 0});
    for (int fd : kbd_fds) pfds.push_back({fd, POLLIN, 0});

    const int rc = poll(pfds.data(), pfds.size(), -1);
    if (rc < 0) {
      if (errno == EINTR) continue;
      std::println(stderr, "poll: {}", strerror(errno));
      break;
    }

    // DWT: любое событие клавиатуры (нажатие/повтор) продлевает окно подавления
    for (size_t i = 1; i < pfds.size(); ++i) {
      if (!(pfds[i].revents & POLLIN)) continue;
      input_event ev;
      while (read(pfds[i].fd, &ev, sizeof(ev)) == sizeof(ev))
        if (ev.type == EV_KEY && ev.value != 0) g_last_key_ms = now_ms();
    }

    if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
      close(touch_fd);
      touch_fd = -1;
      continue;
    }

    if (pfds[0].revents & POLLIN) {
      input_event ev;
      for (;;) {
        const ssize_t n = read(touch_fd, &ev, sizeof(ev));
        if (n != sizeof(ev)) {
          if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
          // устройство отвалилось (например, после сна) — переоткроем
          close(touch_fd);
          touch_fd = -1;
          break;
        }
        g.handle(ev, now_ms());
      }
    }
  }

  if (touch_fd >= 0) close(touch_fd);
  for (int fd : kbd_fds) close(fd);
  std::println("жесты тачпада остановлены");
  return 0;
}