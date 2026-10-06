.PHONY: all clean install

all:
	@mkdir -p build
	@cd build && cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON .. && make

clean:
	@rm -rf build
	@rm -f compile_commands.json

install:
	@echo "👉 [1/4] Создаем udev-правило для доступа к тачпаду без root..."
	@echo 'KERNEL=="event*", ENV{ID_INPUT_TOUCHPAD}=="1", MODE="0664", GROUP="input"' | sudo tee /etc/udev/rules.d/99-touchpad-evdev.rules > /dev/null
	@sudo udevadm control --reload-rules && sudo udevadm trigger

	@echo "👉 [2/4] Установка бинарника в систему (потребуется sudo)..."
	@sudo cmake --install build

	@echo "👉 [3/4] Копирование пользовательского сервиса в ~/.config/systemd/user/..."
	@mkdir -p $(HOME)/.config/systemd/user
	@cp touchpad-gestures.service $(HOME)/.config/systemd/user/

	@echo "👉 [4/4] Активация и запуск пользовательского сервиса..."
	@systemctl --user daemon-reload
	@systemctl --user enable --now touchpad-gestures.service

	@echo "------------------------------------------------------------"
	@echo " 🎉 Полная автоматизация сработала!"
	@echo " Проверить статус: systemctl --user status touchpad-gestures.service"
	@echo "------------------------------------------------------------"
