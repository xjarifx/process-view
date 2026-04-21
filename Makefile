CC = gcc
CFLAGS = -std=c99 -Wall -Wextra -pedantic -O2 -D_POSIX_C_SOURCE=200809L
LDFLAGS = -pthread
INCLUDES = -Iinclude
SRC_DIR = src
BUILD_DIR = build
BIN_DIR = bin
TARGET = $(BIN_DIR)/process_monitor
SRCS = $(SRC_DIR)/main.c $(SRC_DIR)/monitor.c $(SRC_DIR)/action.c $(SRC_DIR)/dashboard.c $(SRC_DIR)/utils.c
OBJS = $(BUILD_DIR)/main.o $(BUILD_DIR)/monitor.o $(BUILD_DIR)/action.o $(BUILD_DIR)/dashboard.o $(BUILD_DIR)/utils.o

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -o $(TARGET) $(OBJS) $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all clean
