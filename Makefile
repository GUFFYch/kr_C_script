CC = gcc
CFLAGS = -Wall -Wextra -std=c11
LDFLAGS =
TARGET = system_logger
SOURCE = system_logger.c
INSTALL_DIR = /usr/local/bin
SYSTEMD_DIR = /etc/systemd/system
USER_TEMPLATE = user-13-61

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCE) $(LDFLAGS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	@echo "Установка программы..."
	sudo cp $(TARGET) $(INSTALL_DIR)/
	sudo chown $(USER_TEMPLATE):$(USER_TEMPLATE) $(INSTALL_DIR)/$(TARGET)
	sudo chmod 4755 $(INSTALL_DIR)/$(TARGET)  # setuid для повышения привилегий
	sudo cp system_logger.service $(SYSTEMD_DIR)/
	sudo systemctl daemon-reload
	@echo "Установка завершена. Используйте: sudo systemctl enable system_logger.service"
	@echo "Затем: sudo systemctl start system_logger.service"

uninstall:
	sudo systemctl stop system_logger.service || true
	sudo systemctl disable system_logger.service || true
	sudo rm -f $(SYSTEMD_DIR)/system_logger.service
	sudo rm -f $(INSTALL_DIR)/$(TARGET)
	sudo systemctl daemon-reload
	@echo "Удаление завершено"
